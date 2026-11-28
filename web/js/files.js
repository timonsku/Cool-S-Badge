// The All tab under Media: every file and folder on the badge's storage, to upload, download,
// rename and delete files and to create, rename and delete folders. The GIF, PNG and Lua tabs
// manage their folders with checks for what the badge can show; this one takes anything.

import { app } from './app.js';
import { $, el, notify, confirmDialog, promptDialog } from './dom.js';
import { FORMATS, blocksFor, formatBytes } from './formats.js';
import {
  refreshMedia, storageInfo, fileManagerSupported, singleDefault, defaultFor, defaultKind, makeDefault,
} from './media.js';

// Left free for littlefs copy-on-write and the settings the badge saves itself, as in media.js.
const RESERVED_BLOCKS = 2;
// The firmware takes paths of up to CONFIG_MCUMGR_GRP_FS_PATH_LEN (64) bytes, '/lfs1/' included.
const MAX_PATH = 64 - '/lfs1/'.length;
// What the badge needs to work: it copes with them missing, but asks before they go.
const IMPORTANT = new Set(['settings.cbor', ...Object.keys(FORMATS).map((folder) => `default.${folder}`)]);
const utf8 = new TextEncoder();

// the folder shown, relative to the root; '' is the root
let cwd = '';
// [{ name, isDir, size }] in cwd; undefined: not loaded, null: the firmware cannot list it
let entries;
// { node, browse }: browse buttons only read, so they also work on firmware without the file manager
const buttons = [];

const join = (folder, name) => (folder ? `${folder}/${name}` : name);
const parentOf = (path) => path.split('/').slice(0, -1).join('/');

function showProgress(text, value = 0) {
  $('#browser-progress').hidden = text === null;
  if (text === null) return;
  $('#browser-label').textContent = text;
  $('#browser-bar').value = value;
}

// '' if name can be used for something in folder, else why not
function checkName(name, folder, current = null) {
  if (name.includes('/')) return 'A name cannot contain "/".';
  if (name.startsWith('.')) return 'Names starting with "." are for the badge\'s temporary files.';
  // eslint-disable-next-line no-control-regex
  if (/[\x00-\x1f\x7f]/.test(name)) return 'The name contains control characters.';
  if (utf8.encode(join(folder, name)).length > MAX_PATH) {
    return `The whole path can be at most ${MAX_PATH} bytes long.`;
  }
  if (name !== current && entries?.some((entry) => entry.name === name)) {
    return 'There is already something of that name here.';
  }
  return '';
}

export function resetBrowser() {
  cwd = '';
  entries = undefined;
  render();
}

// Loads the folder shown, or the closest one that is still there.
export async function refreshBrowser() {
  if (!app.ready) return;
  for (;;) {
    try {
      entries = await app.badge.entries(cwd);
      break;
    } catch (error) {
      if (!cwd) throw error;
      // the folder went away, from here or with the buttons
      cwd = parentOf(cwd);
    }
  }
  entries?.sort((a, b) => Number(b.isDir) - Number(a.isDir) || a.name.localeCompare(b.name));
  render();
}

function open(folder) {
  cwd = folder;
  app.run(refreshBrowser);
}

function renderPath() {
  const parts = cwd ? cwd.split('/') : [];
  const crumbs = [el('button', { type: 'button', className: 'link', textContent: 'Root' })];
  crumbs[0].addEventListener('click', () => open(''));
  parts.forEach((part, i) => {
    const target = parts.slice(0, i + 1).join('/');
    const crumb = i === parts.length - 1
      ? el('strong', { 'aria-current': 'location' }, part)
      : el('button', { type: 'button', className: 'link', textContent: part });
    if (crumb.tagName === 'BUTTON') crumb.addEventListener('click', () => open(target));
    crumbs.push(el('span', { 'aria-hidden': 'true' }, '/'), crumb);
  });
  if (!parts.length) crumbs[0] = el('strong', { 'aria-current': 'location' }, 'Root');
  $('#browser-path').replaceChildren(...crumbs);
}

function button(text, label, onClick, { className = '', browse = false } = {}) {
  const node = el('button', { type: 'button', className, textContent: text, 'aria-label': label });
  node.addEventListener('click', onClick);
  buttons.push({ node, browse });
  return node;
}

function renderEntry(entry) {
  const path = join(cwd, entry.name);
  const info = [];
  const actions = [];

  if (entry.isDir) {
    actions.push(button('Open', `Open ${entry.name}`, () => open(path), { browse: true }));
  } else {
    const storage = storageInfo();
    const onFlash = storage ? blocksFor(entry.size, storage.block) * storage.block : 0;
    info.push(el('span', { title: `Takes ${formatBytes(onFlash)} of flash` }, formatBytes(entry.size)));
    const kind = defaultKind(path);
    // up to v0.9 each mode has a default, from v0.10 on there is one
    const label = singleDefault() ? 'default' : `${FORMATS[kind]?.label} default`;
    if (kind && defaultFor(kind) === path) {
      info.unshift(el('span', { className: 'tag' }, label[0].toUpperCase() + label.slice(1)));
    } else if (kind) {
      actions.push(button('Make default', `Make ${entry.name} the ${label}`, () => makeDefault(kind, path)));
    }
    actions.push(button('Download', `Download ${entry.name}`, () => download(path), { browse: true }));
  }
  actions.push(button('Rename', `Rename ${entry.name}`, () => rename(entry)));
  actions.push(button('Delete', `Delete ${entry.name}`, () => remove(entry), { className: 'delete' }));

  const name = entry.isDir
    ? button(`${entry.name}/`, `Open ${entry.name}`, () => open(path), { className: 'link folder-name', browse: true })
    : el('strong', { title: path }, entry.name);
  return el('li', { className: `file${entry.isDir ? ' folder' : ''}` },
    el('div', { className: 'file-info' }, name, ...info),
    el('div', { className: 'file-actions' }, ...actions));
}

function render() {
  buttons.length = 0;
  renderPath();
  $('#browser-list').replaceChildren(...(entries ?? []).map(renderEntry));
  updateBrowser();
}

// cheap enough to run on every state change
export function updateBrowser() {
  const supported = fileManagerSupported();
  const usable = app.ready && Boolean(storageInfo()) && entries !== null;
  const locked = !usable || !supported || app.busy;
  $('#browser-mkdir').disabled = locked;
  $('#browser-upload').disabled = locked;
  for (const { node, browse } of buttons) node.disabled = browse ? !usable || app.busy : locked;

  let hint = 'Every file and folder on the badge. Files the badge does not know are kept and ignored. ' +
    'The badge copes with any of them missing: without its default it shows the first file of a folder.';
  if (app.ready && entries === null) hint = '';
  else if (app.ready && storageInfo() && !supported) {
    hint = 'The firmware on this badge can only list its files. Update it to upload, rename and delete them here.';
  }
  $('#browser-hint').textContent = hint;

  let empty = 'Not connected.';
  if (app.ready && entries === null) empty = 'The firmware on this badge cannot list all its files. Update it first.';
  else if (app.ready && entries === undefined) empty = 'Loading…';
  else if (app.ready) empty = cwd ? 'This folder is empty.' : 'There are no files on the badge yet.';
  $('#browser-empty').textContent = empty;
  $('#browser-empty').hidden = Boolean(entries?.length);
}

async function download(path) {
  await app.run(async () => {
    showProgress(`Downloading ${path}`);
    try {
      const data = await app.badge.readFile(path);
      if (!data) throw new Error(`${path} is no longer on the badge.`);
      const url = URL.createObjectURL(new Blob([data]));
      const link = el('a', { href: url, download: path.split('/').pop() });
      document.body.append(link);
      link.click();
      link.remove();
      setTimeout(() => URL.revokeObjectURL(url), 60000);
    } finally {
      showProgress(null);
    }
  });
}

async function rename(entry) {
  const kind = entry.isDir ? 'folder' : 'file';
  const name = await promptDialog(`Rename ${kind}`, 'New name', entry.name, 'Rename',
    (value) => checkName(value, cwd, entry.name));
  if (name === null || name === entry.name) return;
  await app.run(async () => {
    await app.badge.rename(join(cwd, entry.name), join(cwd, name));
    await refreshMedia();
  });
}

// files first, then the folders from the deepest up, as the badge only deletes empty folders
async function collect(path) {
  const found = { files: [], folders: [] };
  for (const { name, isDir } of (await app.badge.entries(path)) ?? []) {
    const child = join(path, name);
    if (!isDir) {
      found.files.push(child);
      continue;
    }
    const inner = await collect(child);
    found.files.push(...inner.files);
    found.folders.push(...inner.folders);
  }
  found.folders.push(path);
  return found;
}

async function remove(entry) {
  const path = join(cwd, entry.name);
  await app.run(async () => {
    let message = `${path} will be removed from the badge.`;
    let doomed = { files: [path], folders: [] };
    if (entry.isDir) {
      doomed = await collect(path);
      const count = doomed.files.length;
      message = count
        ? `${path} and the ${count} ${count === 1 ? 'file' : 'files'} in it will be removed from the badge.`
        : `The empty folder ${path} will be removed from the badge.`;
    }
    const important = doomed.files.filter((file) => IMPORTANT.has(file));
    if (important.length) {
      message += ` That includes ${important.join(' and ')}, which the badge uses by default.`;
    }
    if (!(await confirmDialog(`Delete this ${entry.isDir ? 'folder' : 'file'}?`, message, 'Delete'))) return;

    const all = [...doomed.files, ...doomed.folders];
    try {
      for (const [i, item] of all.entries()) {
        if (all.length > 1) showProgress(`Deleting ${item}`, i / all.length);
        await app.badge.removePath(item);
      }
    } finally {
      showProgress(null);
      if (app.smp.connected) await refreshMedia();
    }
  });
}

async function mkdir() {
  const name = await promptDialog('New folder', `Name of the folder in /${cwd}`, '', 'Create',
    (value) => checkName(value, cwd));
  if (name === null) return;
  await app.run(async () => {
    await app.badge.mkdir(join(cwd, name));
    await refreshMedia();
  });
}

function upload(selected) {
  if (!selected.length) return;
  const folder = cwd;
  app.run(async () => {
    const queue = [];
    for (const file of selected) {
      const problem = checkName(file.name, folder, file.name);
      if (problem) {
        notify(`${file.name} is left out: ${problem}`, 'error');
        continue;
      }
      if (entries?.some(({ name, isDir }) => name === file.name && isDir)) {
        notify(`${file.name} is left out: there is a folder of that name.`, 'error');
        continue;
      }
      queue.push(file);
    }
    if (!queue.length) return;

    const replaced = queue.filter(({ name }) => entries?.some((entry) => entry.name === name));
    if (replaced.length &&
        !(await confirmDialog('Replace existing files?', replaced.map(({ name }) => name).join(', '), 'Replace'))) {
      return;
    }

    let uploaded = 0;
    try {
      for (const [i, file] of queue.entries()) {
        const title = `Uploading ${file.name}${queue.length > 1 ? ` (${i + 1} of ${queue.length})` : ''}`;
        showProgress(title);
        const data = new Uint8Array(await file.arrayBuffer());
        // even when replacing, the old file keeps its blocks until the new one is complete
        const info = await app.badge.storage();
        const available = Math.max(0, info.free - RESERVED_BLOCKS);
        const needed = blocksFor(data.length, info.block);
        if (needed > available) {
          notify(`Not enough space for ${file.name}: it needs ${formatBytes(needed * info.block)} and ` +
            `${formatBytes(available * info.block)} is available.`, 'error');
          continue;
        }
        await app.badge.put(join(folder, file.name), data, (value) => showProgress(title, value));
        uploaded++;
      }
    } finally {
      showProgress(null);
      if (app.smp.connected) await refreshMedia();
    }
    if (uploaded) notify(`Uploaded ${uploaded} ${uploaded === 1 ? 'file' : 'files'}.`, 'success');
  });
}

export function setupBrowser() {
  $('#browser-mkdir').addEventListener('click', mkdir);
  const input = $('#browser-input');
  $('#browser-upload').addEventListener('click', () => input.click());
  input.addEventListener('change', () => {
    upload([...input.files]);
    input.value = '';
  });

  const panel = $('#panel-all');
  panel.addEventListener('dragover', (event) => {
    event.preventDefault();
    if (!$('#browser-upload').disabled) panel.classList.add('over');
  });
  panel.addEventListener('dragleave', (event) => {
    if (!panel.contains(event.relatedTarget)) panel.classList.remove('over');
  });
  panel.addEventListener('drop', (event) => {
    event.preventDefault();
    panel.classList.remove('over');
    if (!$('#browser-upload').disabled) upload([...event.dataTransfer.files]);
  });
  render();
}
