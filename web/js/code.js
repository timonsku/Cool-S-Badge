// The Code tab: a small editor for Lua animation scripts (docs/LUA.md), with
// syntax colouring, upload and run on the badge, and the badge's error messages.

import { app } from './app.js';
import { $, el, notify, confirmDialog } from './dom.js';
import { FORMATS, SCRIPT_MAX_SIZE, deviceName, formatBytes } from './formats.js';
import { uploadFiles, filesOn, canUpload } from './media.js';

const STORAGE_KEY = 'cool-s-script';
const INDENT = '  ';
// how long a script gets to fail before the editor asks the badge how it went
const SETTLE_MS = 1500;
const POLL_MS = 3000;

// In web/examples, shipped with the page.
const EXAMPLES = [
  ['rainbow.lua', 'Rainbow along the chain'],
  ['rainbow_scroll.lua', 'Rainbow scrolling down the S'],
  ['breathe.lua', 'Breathing colour'],
  ['comet.lua', 'Comet with a tail'],
  ['sparkle.lua', 'Sparkles'],
  ['scan.lua', 'Bar sweeping up and down'],
  ['fire.lua', 'Fire'],
];

const STARTER = `-- Called 50 times a second, t is the time in seconds.
-- See the script reference for everything a script can use.

function frame(t)
  for i = 1, led.count do
    led.set(i, hsv(i / led.count - t * 0.2))
  end
end
`;

let name = 'script';
// what was last loaded or uploaded, to warn before throwing edits away
let savedText = '';
// the script this tab last ran on the badge, whose errors it shows
let ranName = null;
let status = null;
let uploading = false;

// ---- syntax colouring ----

const KEYWORDS = new Set(('and break do else elseif end false for function goto if in local nil not or repeat ' +
  'return then true until while').split(' '));
const BUILTINS = new Set(('led rgb hsv mix dim split sleep now millis wave tri clamp lerp frame fps math table ' +
  'print pairs ipairs tostring tonumber type select error assert pcall next rawget rawset setmetatable ' +
  'getmetatable collectgarbage BLACK WHITE RED GREEN BLUE YELLOW CYAN MAGENTA ORANGE PURPLE PINK').split(' '));

const TOKEN = new RegExp([
  /(--\[(=*)\[[\s\S]*?(?:\]\2\]|$))/.source, // long comment
  /(--[^\n]*)/.source, // comment
  /(\[(=*)\[[\s\S]*?(?:\]\5\]|$))/.source, // long string
  /("(?:\\[\s\S]|[^"\\\n])*"?|'(?:\\[\s\S]|[^'\\\n])*'?)/.source, // string
  /(0[xX][0-9a-fA-F]+|\d+\.?\d*(?:[eE][+-]?\d+)?|\.\d+(?:[eE][+-]?\d+)?)/.source, // number
  /([A-Za-z_]\w*)/.source, // name
].join('|'), 'g');

const escapeHtml = (text) => text.replace(/[&<>]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' })[c]);

function highlight(text) {
  let html = '';
  let last = 0;
  for (const match of text.matchAll(TOKEN)) {
    html += escapeHtml(text.slice(last, match.index));
    last = match.index + match[0].length;
    const [token, longComment, , comment, longString, , string, number, word] = match;
    let kind = null;
    if (longComment || comment) kind = 'comment';
    else if (longString || string) kind = 'string';
    else if (number) kind = 'number';
    else if (KEYWORDS.has(word)) kind = 'keyword';
    else if (BUILTINS.has(word)) kind = 'builtin';
    html += kind ? `<span class="tok-${kind}">${escapeHtml(token)}</span>` : escapeHtml(token);
  }
  // a final newline needs something after it to take up a line
  return `${html}${escapeHtml(text.slice(last))}\n`;
}

// ---- editor ----

const area = () => $('#code-text');
const text = () => area().value;

// The line the badge's message points at, as in "fire.lua:12: ...".
function errorLine() {
  const match = status?.error?.match(/^[^:]+:(\d+):/);
  return match && ranName && status.path?.endsWith(`/${ranName}`) ? Number(match[1]) : null;
}

function render() {
  const value = text();
  $('#code-highlight').innerHTML = highlight(value);
  const lines = value.split('\n').length;
  const bad = errorLine();
  const gutter = $('#code-gutter');
  if (gutter.childElementCount !== lines || gutter.dataset.bad !== String(bad)) {
    gutter.replaceChildren(...Array.from({ length: lines }, (_, i) =>
      el('span', { className: i + 1 === bad ? 'bad' : '' }, String(i + 1))));
    gutter.dataset.bad = String(bad);
  }
  syncScroll();
  update();
}

function syncScroll() {
  $('#code-highlight').style.transform = `translate(${-area().scrollLeft}px, ${-area().scrollTop}px)`;
  $('#code-gutter').style.transform = `translateY(${-area().scrollTop}px)`;
}

function save() {
  localStorage.setItem(STORAGE_KEY, JSON.stringify({ name, text: text(), savedText }));
}

function load() {
  try {
    const saved = JSON.parse(localStorage.getItem(STORAGE_KEY));
    if (saved && typeof saved.text === 'string') {
      name = typeof saved.name === 'string' ? saved.name : name;
      savedText = typeof saved.savedText === 'string' ? saved.savedText : '';
      return saved.text;
    }
  } catch {
    // start over with the starter script
  }
  // nothing to lose in the starter script
  savedText = STARTER;
  return STARTER;
}

// Replaces the selection through the browser, so that undo keeps working.
function insert(value) {
  if (!document.execCommand('insertText', false, value)) {
    area().setRangeText(value, area().selectionStart, area().selectionEnd, 'end');
  }
}

function lineStart(value, pos) {
  return value.lastIndexOf('\n', pos - 1) + 1;
}

// Tab and Shift+Tab indent and outdent, Enter keeps the indentation and adds a level after an opening line.
function onKeyDown(event) {
  const input = area();
  const value = input.value;
  const { selectionStart: start, selectionEnd: end } = input;

  if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 's') {
    event.preventDefault();
    if (!$('#code-run').disabled) uploadScript(true);
    return;
  }
  if (event.key === 'Tab' && !event.ctrlKey && !event.altKey && !event.metaKey) {
    event.preventDefault();
    if (start === end && !event.shiftKey) {
      insert(INDENT);
      return;
    }
    // whole lines: select them, then put them back changed
    const from = lineStart(value, start);
    const to = end > start && value[end - 1] === '\n' ? end - 1 : end;
    const lines = value.slice(from, to).split('\n');
    const changed = lines.map((line) => (event.shiftKey ? line.replace(/^ {1,2}|^\t/, '') : INDENT + line));
    input.setSelectionRange(from, to);
    insert(changed.join('\n'));
    input.setSelectionRange(from, from + changed.join('\n').length);
    return;
  }
  if (event.key === 'Enter' && !event.ctrlKey && !event.altKey && !event.metaKey) {
    event.preventDefault();
    const current = value.slice(lineStart(value, start), start);
    let indent = current.match(/^\s*/)[0];
    const code = current.replace(/--.*$/, '');
    // "f(function() end)" opens and closes on the same line
    const opens = /(\b(then|do|else|repeat)|\bfunction\b.*\)|[{(])\s*$/.test(code) && !/\bend\b[\s)}]*$/.test(code);
    if (opens) indent += INDENT;
    insert(`\n${indent}`);
    return;
  }
  // typing "end" on an indented line that opened nothing lines it up with its block
  if (event.key === 'd' && value.slice(lineStart(value, start), start).match(/^\s+en$/) && start === end) {
    const from = lineStart(value, start);
    const line = value.slice(from, start);
    if (line.startsWith(INDENT)) {
      event.preventDefault();
      input.setSelectionRange(from, start);
      insert(`${line.slice(INDENT.length)}d`);
    }
  }
}

// ---- files ----

const fileName = () => deviceName(`${name.trim() || 'script'}.lua`, 'lua');
const dirty = () => text() !== savedText;

async function replaceText(value, newName) {
  if (dirty() && text() !== value &&
      !(await confirmDialog('Discard your changes?', `The changes to ${fileName()} are not on the badge or downloaded.`,
        'Discard'))) {
    return false;
  }
  area().value = value;
  area().setSelectionRange(0, 0);
  area().scrollTop = 0;
  name = newName.replace(/\.lua$/i, '');
  $('#code-name').value = name;
  savedText = value;
  status = null;
  ranName = null;
  save();
  render();
  return true;
}

function fillOpenMenu() {
  const menu = $('#code-open');
  const badgeFiles = filesOn('lua');
  const groups = [
    el('option', { value: '', textContent: 'Open…', disabled: true, selected: true }),
    el('option', { value: 'new', textContent: 'New script' }),
    el('option', { value: 'file', textContent: 'File on this computer…' }),
    el('optgroup', { label: 'Examples' },
      ...EXAMPLES.map(([file, label]) => el('option', { value: `example:${file}`, textContent: label }))),
  ];
  if (app.ready && badgeFiles?.length) {
    groups.push(el('optgroup', { label: 'On the badge' },
      ...badgeFiles.map(({ name: file }) => el('option', { value: `badge:${file}`, textContent: file }))));
  }
  menu.replaceChildren(...groups);
  menu.disabled = app.busy;
}

async function openChoice(choice) {
  if (choice === 'new') {
    await replaceText(STARTER, 'script');
  } else if (choice === 'file') {
    $('#code-file').click();
  } else if (choice.startsWith('example:')) {
    const file = choice.slice('example:'.length);
    const response = await fetch(`examples/${file}`);
    if (!response.ok) throw new Error(`Could not load the example ${file}.`);
    await replaceText(await response.text(), file);
  } else if (choice.startsWith('badge:')) {
    await editScript(choice.slice('badge:'.length));
  }
}

// Opens a script from the badge's lua folder. Also used by the Media tab.
export async function editScript(file) {
  $('#tab-code').click();
  let data = null;
  await app.run(async () => {
    data = await app.badge.download('lua', file);
  });
  if (!data) return;
  const value = new TextDecoder().decode(data);
  if (await replaceText(value, file)) ranName = file;
}

async function uploadScript(run) {
  const data = new TextEncoder().encode(text());
  if (!FORMATS.lua.inspect(data)) {
    notify(`The script is empty or larger than ${formatBytes(SCRIPT_MAX_SIZE)}.`, 'error');
    return;
  }
  const file = fileName();
  uploading = true;
  update();
  try {
    const uploaded = await uploadFiles([new File([data], file)],
      { folder: 'lua', progress: showProgress, replace: true });
    if (!uploaded) return;
    savedText = text();
    save();
    if (run) await runOnBadge(file);
  } finally {
    uploading = false;
    update();
  }
}

async function runOnBadge(file) {
  await app.run(async () => {
    if (!(await app.badge.runScript(file))) {
      notify('The firmware on this badge cannot start scripts from here. Update it, or pick the script with ' +
        'the buttons on the badge.');
      return;
    }
    ranName = file;
    status = { running: true, path: '', error: '' };
    render();
    await new Promise((resolve) => setTimeout(resolve, SETTLE_MS));
    status = await app.badge.scriptStatus();
  });
  render();
  pollStatus();
}

// Asks the badge now and then, as a script can also fail after a while.
function pollStatus() {
  if (!ranName || $('#panel-code').hidden) return;
  app.background(async (signal) => {
    while (!signal.aborted && app.ready && ranName && !$('#panel-code').hidden) {
      await new Promise((resolve) => setTimeout(resolve, POLL_MS));
      if (signal.aborted) return;
      const next = await app.badge.scriptStatus();
      if (JSON.stringify(next) !== JSON.stringify(status)) {
        status = next;
        render();
      }
    }
  });
}

function download() {
  const url = URL.createObjectURL(new Blob([text()], { type: 'text/plain' }));
  const link = el('a', { href: url, download: fileName() });
  document.body.append(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
  savedText = text();
  save();
}

function showProgress(label, value = 0) {
  $('#code-progress').hidden = label === null;
  if (label === null) return;
  $('#code-progress-label').textContent = label;
  $('#code-progress-bar').value = value;
}

function showStatus() {
  const line = $('#code-status');
  line.className = 'status';
  line.replaceChildren();
  if (!ranName) {
    line.textContent = 'Upload and run shows the script on the badge, and any error it has here.';
    return;
  }
  if (!status) {
    line.textContent = app.ready ? `${ranName} was sent to the badge.` : '';
    return;
  }
  if (!status.path?.endsWith(`/${ranName}`)) {
    line.textContent = `The badge is showing something else now than ${ranName}.`;
    return;
  }
  if (status.error) {
    line.classList.add('error');
    const bad = errorLine();
    line.append(`${status.error} `);
    if (bad) {
      const jump = el('button', { type: 'button', className: 'link', textContent: `Go to line ${bad}` });
      jump.addEventListener('click', () => goToLine(bad));
      line.append(jump);
    }
    return;
  }
  line.classList.add('success');
  line.textContent = status.running ? `${ranName} is running on the badge.` : `${ranName} finished.`;
}

function goToLine(number) {
  const value = text();
  let pos = 0;
  for (let i = 1; i < number && pos !== -1; i++) pos = value.indexOf('\n', pos) + 1 || -1;
  if (pos < 0) return;
  const end = value.indexOf('\n', pos);
  area().focus();
  area().setSelectionRange(pos, end < 0 ? value.length : end);
  const lineHeight = parseFloat(getComputedStyle(area()).lineHeight);
  area().scrollTop = Math.max(0, (number - 3) * lineHeight);
}

function update() {
  const size = new TextEncoder().encode(text()).length;
  const sizeLabel = $('#code-size');
  sizeLabel.textContent = `${formatBytes(size)} of ${formatBytes(SCRIPT_MAX_SIZE)}`;
  sizeLabel.classList.toggle('over', size > SCRIPT_MAX_SIZE);

  const supported = filesOn('lua') !== null;
  const blocked = uploading || !canUpload() || !supported;
  $('#code-run').disabled = blocked;
  $('#code-upload').disabled = blocked;
  const why = !app.ready ? 'Connect a badge first' : !supported ? 'The firmware on this badge cannot run Lua scripts' : '';
  $('#code-run').title = why;
  $('#code-upload').title = why;
  $('#code-filename').textContent = `Saved as /lua/${fileName()} on the badge`;
  showStatus();
}

export function setupCode() {
  const input = area();
  input.value = load();
  $('#code-name').value = name;

  input.addEventListener('input', () => {
    render();
    save();
  });
  input.addEventListener('scroll', syncScroll);
  input.addEventListener('keydown', onKeyDown);

  $('#code-name').addEventListener('input', ({ target }) => {
    name = target.value;
    save();
    update();
  });

  const menu = $('#code-open');
  menu.addEventListener('change', () => {
    const choice = menu.value;
    menu.value = '';
    openChoice(choice).catch((error) => notify(error.message, 'error'));
  });
  $('#code-file').addEventListener('change', async ({ target }) => {
    const [file] = target.files;
    target.value = '';
    if (!file) return;
    if (file.size > SCRIPT_MAX_SIZE) {
      notify(`${file.name} is larger than the badge's ${formatBytes(SCRIPT_MAX_SIZE)} limit for scripts.`, 'error');
      return;
    }
    await replaceText(await file.text(), file.name);
    savedText = '';
    save();
  });

  $('#code-run').addEventListener('click', () => uploadScript(true));
  $('#code-upload').addEventListener('click', () => uploadScript(false));
  $('#code-download').addEventListener('click', download);

  app.addEventListener('change', () => {
    fillOpenMenu();
    update();
  });
  app.smp.addEventListener('disconnect', () => {
    status = null;
    render();
  });
  fillOpenMenu();
  render();

  return () => {
    fillOpenMenu();
    render();
    pollStatus();
  };
}
