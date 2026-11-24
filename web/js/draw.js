// Draw pictures and animations on a model of the badge, then upload them.

import { app } from './app.js';
import { $, el, notify } from './dom.js';
import { LEDS, LED_COUNT, OUTER, INNER, BUTTONS, VIEW } from './badge-layout.js';
import { encodeGif, encodePng } from './image-encode.js';
import { canUpload, uploadFiles } from './media.js';
import { PREVIEW_MAX } from './badge.js';
import { formatBytes } from './formats.js';

const STORAGE_KEY = 'cool-s-drawing';
const MAX_FRAMES = 300;
const MAX_UNDO = 100;
// GIF delays are centiseconds, and the badge redraws as fast as a delay of 0 allows
const MIN_DELAY = 20;
const MAX_DELAY = 10000;
const CELL = 5; // mm, the touch target around each LED; neighbours are 5.4 mm apart or more

const SWATCHES = [
  ['Off', 0x000000], ['White', 0xffffff], ['Red', 0xff0000], ['Orange', 0xff6a00],
  ['Yellow', 0xffd000], ['Green', 0x00ff40], ['Cyan', 0x00e0ff], ['Blue', 0x0030ff],
  ['Purple', 0x8a00ff], ['Pink', 0xff2a8a],
];

const SVG_NS = 'http://www.w3.org/2000/svg';
const hex = (color) => `#${color.toString(16).padStart(6, '0')}`;
const blank = () => new Array(LED_COUNT).fill(0);

let frames = [blank()];
let current = 0;
let delay = 200;
let name = 'drawing';
let color = 0xff0000;
let undoStack = [];
let redoStack = [];
let playing = null;
let shown = null; // the frame on the badge model, differs from `current` while playing
let painting = false;
let strokeChanged = false;
let uploading = false;
let live = false;
let liveSent = []; // what the badge shows; unknown (undefined) until sent
let liveSending = false;
let liveDone = Promise.resolve();
// While Play runs with live preview on, the badge plays the animation itself from RAM,
// and no pixels go to it until that preview has ended.
let badgeHold = false;
let badgeSession = null; // resolves to whether the badge got the preview
let badgeEnded = Promise.resolve();

const cells = [];
const glow = document.createElement('canvas');
const outer = new Path2D(OUTER);
const inner = new Path2D(INNER);
const CASE = '#c9ced6';
const LINES = '#1d2026';
const thumbs = [];
let renderQueued = false;

// state

function load() {
  try {
    const saved = JSON.parse(localStorage.getItem(STORAGE_KEY));
    const valid = (frame) => Array.isArray(frame) && frame.length === LED_COUNT &&
      frame.every((c) => Number.isInteger(c) && c >= 0 && c <= 0xffffff);
    if (Array.isArray(saved?.frames) && saved.frames.length && saved.frames.every(valid)) {
      frames = saved.frames.slice(0, MAX_FRAMES);
    }
    if (Number.isInteger(saved?.delay)) delay = clampDelay(saved.delay);
    if (typeof saved?.name === 'string') name = saved.name;
    if (Number.isInteger(saved?.color)) color = saved.color & 0xffffff;
  } catch {
    // nothing saved, or storage is blocked
  }
}

function save() {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify({ frames, delay, name, color }));
  } catch {
    // private browsing or full storage: the drawing just won't survive a reload
  }
}

function clampDelay(value) {
  return Math.min(MAX_DELAY, Math.max(MIN_DELAY, Math.round(value / 10) * 10));
}

function snapshot() {
  return { frames: frames.map((frame) => frame.slice()), current };
}

function remember() {
  undoStack.push(snapshot());
  if (undoStack.length > MAX_UNDO) undoStack.shift();
  redoStack = [];
}

function restore(from, to) {
  const state = from.pop();
  if (!state) return;
  stop();
  to.push(snapshot());
  ({ frames, current } = state);
  changed(true);
}

// edits always apply to the frame being edited, so playback stops first
function edit(change, structure = false) {
  stop();
  remember();
  change();
  changed(structure);
}

function changed(structure = false) {
  shown = null;
  save();
  if (structure) renderFrames();
  else drawThumb(current);
  update();
  queueRender();
}

// badge model

function buildStage() {
  const svg = $('#draw-leds');
  svg.setAttribute('viewBox', `${VIEW.x} ${VIEW.y} ${VIEW.width} ${VIEW.height}`);
  $('#draw-stage').style.aspectRatio = `${VIEW.width} / ${VIEW.height}`;

  // the badge's outline: a stroke can start anywhere on it, around it the page scrolls
  const area = document.createElementNS(SVG_NS, 'path');
  area.setAttribute('class', 'badge-area');
  area.setAttribute('d', OUTER);
  svg.append(area);

  LEDS.forEach(([x, y], i) => {
    const cell = document.createElementNS(SVG_NS, 'rect');
    cell.setAttribute('class', 'led-cell');
    cell.setAttribute('x', x - CELL / 2);
    cell.setAttribute('y', y - CELL / 2);
    cell.setAttribute('width', CELL);
    cell.setAttribute('height', CELL);
    cell.setAttribute('rx', 0.6);
    cell.setAttribute('role', 'button');
    cell.setAttribute('tabindex', i === 0 ? '0' : '-1');
    cell.dataset.index = i;
    svg.append(cell);
    cells.push(cell);
  });

  // Touches on the badge draw, touches beside it scroll the page. touch-action on the badge
  // says the same, but not every browser honours it on SVG shapes.
  svg.addEventListener('touchstart', (event) => {
    if (onBadge(event.target)) event.preventDefault();
  }, { passive: false });

  svg.addEventListener('pointerdown', (event) => {
    if (!onBadge(event.target) || event.button !== 0) return;
    const index = cellAt(event.clientX, event.clientY);
    event.preventDefault();
    if (index !== null) focusCell(index);
    if (event.altKey) {
      if (index !== null) pickColor(index);
      return;
    }
    stop();
    remember();
    painting = true;
    strokeChanged = false;
    svg.setPointerCapture(event.pointerId);
    if (index !== null) paint(index);
  });
  svg.addEventListener('pointermove', (event) => {
    if (!painting) return;
    // coalesced events catch LEDs a fast swipe would skip
    for (const point of event.getCoalescedEvents?.() ?? [event]) {
      const index = cellAt(point.clientX, point.clientY);
      if (index !== null) paint(index);
    }
  });
  const end = () => {
    if (!painting) return;
    painting = false;
    if (strokeChanged) changed();
    else undoStack.pop(); // the stroke changed nothing
  };
  svg.addEventListener('pointerup', end);
  svg.addEventListener('pointercancel', end);
  svg.addEventListener('lostpointercapture', end);

  svg.addEventListener('keydown', (event) => {
    const index = Number(event.target.dataset?.index);
    if (!Number.isInteger(index)) return;
    const step = { ArrowRight: 1, ArrowDown: 1, ArrowLeft: -1, ArrowUp: -1 }[event.key];
    if (step) {
      event.preventDefault();
      focusCell((index + step + LED_COUNT) % LED_COUNT, true);
    } else if (event.key === 'Enter' || event.key === ' ') {
      event.preventDefault();
      edit(() => { frames[current][index] = color; });
    }
  });

  new ResizeObserver(queueRender).observe($('#draw-stage'));
}

const onBadge = (target) => Boolean(target?.closest?.('.led-cell, .badge-area'));

function cellAt(x, y) {
  const target = document.elementFromPoint(x, y);
  const index = Number(target?.dataset?.index);
  return target?.classList?.contains('led-cell') && Number.isInteger(index) ? index : null;
}

function focusCell(index, move = false) {
  cells.forEach((cell, i) => cell.setAttribute('tabindex', i === index ? '0' : '-1'));
  if (move) cells[index].focus();
}

function paint(index) {
  if (frames[current][index] === color) return;
  frames[current][index] = color;
  strokeChanged = true;
  queueRender();
}

function pickColor(index) {
  setColor(frames[shown ?? current][index]);
}

function queueRender() {
  syncLive();
  if (renderQueued) return;
  renderQueued = true;
  requestAnimationFrame(() => {
    renderQueued = false;
    renderStage();
  });
}

// The case is frosted plastic: each LED is a soft patch of light on it,
// brightest in the middle, where a bright LED washes out towards white.
function renderStage() {
  const canvas = $('#draw-glow');
  const box = canvas.getBoundingClientRect();
  if (!box.width || !box.height) return;
  const ratio = window.devicePixelRatio || 1;
  canvas.width = Math.round(box.width * ratio);
  canvas.height = Math.round(box.height * ratio);

  const ctx = canvas.getContext('2d');
  // the same fit as the SVG's xMidYMid meet, so the rectangles line up
  const scale = Math.min(canvas.width / VIEW.width, canvas.height / VIEW.height);
  const offsetX = (canvas.width - VIEW.width * scale) / 2 - VIEW.x * scale;
  const offsetY = (canvas.height - VIEW.height * scale) / 2 - VIEW.y * scale;
  ctx.setTransform(scale, 0, 0, scale, offsetX, offsetY);

  const frame = frames[shown ?? current];

  ctx.fillStyle = '#25282e';
  for (const [x, y] of BUTTONS) ctx.fillRect(x - 1.6, y - 3, 3.2, 6);

  // the lines of the S are everything between the outer edge and the inside
  ctx.fillStyle = LINES;
  ctx.fill(outer);
  ctx.fillStyle = CASE;
  ctx.fill(inner);

  ctx.save();
  ctx.clip(inner);
  // unlit LEDs show through faintly
  ctx.fillStyle = 'rgb(40 44 52 / 14%)';
  for (const [x, y] of LEDS) ctx.fillRect(x - 1.75, y - 1.75, 3.5, 3.5);

  glow.width = canvas.width;
  glow.height = canvas.height;
  const light = glow.getContext('2d');
  light.setTransform(scale, 0, 0, scale, offsetX, offsetY);
  light.globalCompositeOperation = 'lighter';
  LEDS.forEach(([x, y], i) => {
    const c = frame[i];
    const peak = Math.max(c >> 16, (c >> 8) & 0xff, c & 0xff);
    if (!peak) return;
    // light adds to the case: a dim colour is the full colour, fainter
    const strength = peak / 255;
    const r = Math.round(((c >> 16) * 255) / peak);
    const g = Math.round((((c >> 8) & 0xff) * 255) / peak);
    const b = Math.round(((c & 0xff) * 255) / peak);
    const gradient = light.createRadialGradient(x, y, 0, x, y, 7.5);
    gradient.addColorStop(0, `rgb(${r} ${g} ${b} / ${strength})`);
    gradient.addColorStop(0.3, `rgb(${r} ${g} ${b} / ${0.75 * strength})`);
    gradient.addColorStop(1, `rgb(${r} ${g} ${b} / 0)`);
    light.fillStyle = gradient;
    light.fillRect(x - 7.5, y - 7.5, 15, 15);

    const core = light.createRadialGradient(x, y, 0, x, y, 2.6);
    core.addColorStop(0, `rgb(255 255 255 / ${0.55 * strength})`);
    core.addColorStop(1, 'rgb(255 255 255 / 0)');
    light.fillStyle = core;
    light.fillRect(x - 2.6, y - 2.6, 5.2, 5.2);
  });

  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.filter = `blur(${Math.max(1, 0.6 * scale)}px)`;
  ctx.drawImage(glow, 0, 0);
  ctx.filter = 'none';
  ctx.restore();

  frame.forEach((c, i) => {
    cells[i].setAttribute('aria-label', `LED ${i + 1}, ${c ? hex(c) : 'off'}`);
  });
}

// live preview

function liveAvailable() {
  return app.ready && !app.busy;
}

// Sends the pixels that differ from what the badge shows, one at a time, and
// always towards the newest state, so fast painting or playback never queues up.
function syncLive() {
  if (!live || liveSending || badgeHold) return;
  liveSending = true;
  liveDone = sendLive();
}

async function sendLive() {
  try {
    while (live && !badgeHold && liveAvailable()) {
      const target = frames[shown ?? current];
      const index = target.findIndex((c, i) => liveSent[i] !== c);
      if (index < 0) break;
      const c = target[index];
      await app.badge.showPixel(index, c);
      liveSent[index] = c;
    }
  } catch (error) {
    if (app.smp.connected) {
      notify(`Live preview stopped: ${error.message}`, 'error');
      setLive(false);
    }
  } finally {
    liveSending = false;
  }
}

function setLive(on) {
  live = on;
  liveSent = [];
  $('#draw-live').checked = on;
  syncLive();
}

// frames

function drawThumb(index) {
  const canvas = thumbs[index];
  if (!canvas) return;
  const ctx = canvas.getContext('2d');
  const scale = canvas.width / VIEW.width;
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  ctx.setTransform(scale, 0, 0, scale, -VIEW.x * scale, -VIEW.y * scale);
  ctx.fillStyle = LINES;
  ctx.fill(outer);
  ctx.fillStyle = CASE;
  ctx.fill(inner);
  LEDS.forEach(([x, y], i) => {
    const c = frames[index][i];
    ctx.fillStyle = c ? hex(c) : 'rgb(40 44 52 / 18%)';
    ctx.beginPath();
    ctx.arc(x, y, c ? 3.2 : 1.6, 0, Math.PI * 2);
    ctx.fill();
  });
}

function renderFrames() {
  thumbs.length = 0;
  const items = frames.map((_, i) => {
    const canvas = el('canvas', { width: 36, height: 93 });
    thumbs.push(canvas);
    drawThumb(i);
    const button = el('button', { type: 'button', 'aria-label': `Frame ${i + 1}` }, canvas, el('span', {}, String(i + 1)));
    button.addEventListener('click', () => {
      stop();
      current = i;
      changed(true);
    });
    return el('li', {}, button);
  });
  $('#draw-frames').replaceChildren(...items);
  markFrames();
}

function markFrames() {
  const active = shown ?? current;
  [...$('#draw-frames').children].forEach((item, i) => {
    const button = item.firstElementChild;
    if (i === current) button.setAttribute('aria-current', 'true');
    else button.removeAttribute('aria-current');
    button.classList.toggle('showing', playing !== null && i === active);
  });
  // scroll the strip only; scrollIntoView would also move the page while playing
  const list = $('#draw-frames');
  const selected = list.children[playing !== null ? active : current];
  if (!selected) return;
  const left = selected.offsetLeft;
  const right = left + selected.offsetWidth;
  if (left < list.scrollLeft) list.scrollLeft = left;
  else if (right > list.scrollLeft + list.clientWidth) list.scrollLeft = right - list.clientWidth;
}

// Sending each frame's pixels is too slow for animations, so the badge gets the GIF.
function startBadgePreview() {
  const data = encodeGif(frames, delay / 10);
  const ended = badgeEnded;
  badgeHold = true;
  badgeSession = (async () => {
    // the previous preview's end and any pixels on their way go first
    await ended;
    await liveDone;
    if (data.length > PREVIEW_MAX) {
      throw new Error(
        `This animation (${formatBytes(data.length)}) is too long to preview on the badge, ` +
        `which holds ${formatBytes(PREVIEW_MAX)}. It only plays here.`,
      );
    }
    await app.badge.previewGif(data);
    return true;
  })().catch((error) => {
    if (app.smp.connected) notify(error.message, 'error');
    return false;
  });
}

function endBadgePreview() {
  const session = badgeSession;
  if (!session) return;
  badgeSession = null;
  badgeEnded = session
    .then((started) => (started && app.smp.connected ? app.badge.endPreview() : null))
    .catch(() => {})
    .then(() => {
      // unless Play was pressed again meanwhile, go back to sending the edited frame
      if (badgeSession) return;
      badgeHold = false;
      liveSent = [];
      syncLive();
    });
}

function play() {
  if (playing !== null) {
    stop();
    return;
  }
  if (live && liveAvailable()) startBadgePreview();
  shown = current;
  const step = () => {
    shown = (shown + 1) % frames.length;
    markFrames();
    queueRender();
    playing = setTimeout(step, delay);
  };
  playing = setTimeout(step, delay);
  update();
}

function stop() {
  if (playing === null) return;
  endBadgePreview();
  clearTimeout(playing);
  playing = null;
  shown = null;
  markFrames();
  queueRender();
  update();
}

// colour

function setColor(value) {
  color = value;
  $('#draw-color').value = hex(color);
  for (const swatch of $('#draw-swatches').children) {
    swatch.setAttribute('aria-pressed', String(Number(swatch.dataset.color) === color));
  }
  $('#draw-stage').style.setProperty('--paint', hex(color));
  save();
}

function buildSwatches() {
  const swatches = SWATCHES.map(([label, value]) => {
    const swatch = el('button', {
      type: 'button', className: 'swatch', title: label, 'aria-label': label, 'aria-pressed': 'false',
    });
    swatch.dataset.color = value;
    swatch.style.setProperty('--swatch', hex(value));
    swatch.addEventListener('click', () => setColor(value));
    return swatch;
  });
  $('#draw-swatches').replaceChildren(...swatches);
}

// output

function fileFor() {
  const animated = frames.length > 1;
  return {
    animated,
    folder: animated ? 'gif' : 'png',
    filename: `${name.trim().replace(/\.(png|gif)$/i, '') || 'drawing'}.${animated ? 'gif' : 'png'}`,
  };
}

async function encode() {
  const { animated, folder, filename } = fileFor();
  const data = animated ? encodeGif(frames, delay / 10) : await encodePng(frames[0]);
  return { folder, filename, data };
}

async function upload() {
  stop();
  const { folder, filename, data } = await encode();
  uploading = true;
  update();
  try {
    await uploadFiles([new File([data], filename)], { folder, progress: showProgress });
  } finally {
    uploading = false;
    update();
  }
}

async function download() {
  const { folder, filename, data } = await encode();
  const url = URL.createObjectURL(new Blob([data], { type: `image/${folder}` }));
  const link = el('a', { href: url, download: filename });
  document.body.append(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function showProgress(text, value = 0) {
  $('#draw-progress').hidden = text === null;
  if (text === null) return;
  $('#draw-progress-label').textContent = text;
  $('#draw-progress-bar').value = value;
}

function update() {
  const { animated, folder } = fileFor();
  $('#draw-format').textContent = animated
    ? `${frames.length} frames: saves as an animation in /gif.`
    : 'One frame: saves as a picture in /png. Add frames to make an animation.';
  $('#draw-upload').textContent = `Upload ${animated ? 'GIF' : 'PNG'} to badge`;
  $('#draw-upload').disabled = uploading || !canUpload();
  $('#draw-upload').title = app.ready ? '' : 'Connect a badge first';
  $('#draw-download').textContent = `Download .${folder}`;

  $('#draw-live').disabled = !app.ready;
  $('#draw-live-hint').textContent = !app.ready
    ? 'Connect a badge to see the drawing on it as you paint.'
    : live
      ? 'The badge shows the frame you are editing, and Play runs the animation on it without saving it. ' +
        'It keeps the last picture until you press one of its buttons.'
      : 'Show the frame you are editing on the badge. Nothing is saved on it.';
  syncLive();

  $('#draw-play').textContent = playing !== null ? 'Stop' : 'Play';
  $('#draw-play').disabled = frames.length < 2;
  $('#draw-undo').disabled = !undoStack.length;
  $('#draw-redo').disabled = !redoStack.length;
  $('#frame-add').disabled = frames.length >= MAX_FRAMES;
  $('#frame-copy').disabled = frames.length >= MAX_FRAMES;
  $('#frame-earlier').disabled = current === 0;
  $('#frame-later').disabled = current === frames.length - 1;
  $('#frame-delete').disabled = frames.length < 2;
  $('#draw-frames-count').textContent = `${current + 1} of ${frames.length}`;
}

export function setupDraw() {
  load();
  buildStage();
  buildSwatches();
  setColor(color);
  renderFrames();

  $('#draw-color').addEventListener('input', ({ target }) => setColor(parseInt(target.value.slice(1), 16)));
  $('#draw-fill').addEventListener('click', () => edit(() => frames[current].fill(color)));
  $('#draw-clear').addEventListener('click', () => edit(() => frames[current].fill(0)));
  $('#draw-undo').addEventListener('click', () => restore(undoStack, redoStack));
  $('#draw-redo').addEventListener('click', () => restore(redoStack, undoStack));

  $('#frame-add').addEventListener('click', () => edit(() => {
    frames.splice(current + 1, 0, blank());
    current++;
  }, true));
  $('#frame-copy').addEventListener('click', () => edit(() => {
    frames.splice(current + 1, 0, frames[current].slice());
    current++;
  }, true));
  $('#frame-delete').addEventListener('click', () => edit(() => {
    frames.splice(current, 1);
    current = Math.min(current, frames.length - 1);
  }, true));
  const move = (step) => edit(() => {
    const [frame] = frames.splice(current, 1);
    current += step;
    frames.splice(current, 0, frame);
  }, true);
  $('#frame-earlier').addEventListener('click', () => move(-1));
  $('#frame-later').addEventListener('click', () => move(1));

  const delayInput = $('#draw-delay');
  delayInput.value = delay;
  delayInput.addEventListener('change', () => {
    const value = Number(delayInput.value);
    if (Number.isFinite(value) && value > 0) delay = clampDelay(value);
    delayInput.value = delay;
    save();
    // the badge has the old timing, so start over
    if (playing !== null) {
      stop();
      play();
    }
  });
  $('#draw-play').addEventListener('click', play);

  const nameInput = $('#draw-name');
  nameInput.value = name;
  nameInput.addEventListener('input', () => {
    name = nameInput.value;
    save();
  });
  $('#draw-upload').addEventListener('click', () => upload().catch((error) => notify(error.message, 'error')));
  $('#draw-download').addEventListener('click', () => download().catch((error) => notify(error.message, 'error')));

  document.addEventListener('keydown', (event) => {
    if ($('#panel-draw').hidden || !(event.ctrlKey || event.metaKey) || event.altKey) return;
    if (event.target.matches?.('input, textarea')) return;
    const key = event.key.toLowerCase();
    if (key === 'z' && !event.shiftKey) {
      event.preventDefault();
      restore(undoStack, redoStack);
    } else if (key === 'y' || (key === 'z' && event.shiftKey)) {
      event.preventDefault();
      restore(redoStack, undoStack);
    }
  });

  $('#draw-live').addEventListener('change', ({ target }) => setLive(target.checked));
  // after a reconnect the badge shows its own files again
  app.smp.addEventListener('disconnect', () => { liveSent = []; });

  app.addEventListener('change', update);
  update();

  return () => {
    // the tab was hidden, so the canvas had no size
    queueRender();
  };
}

export function leaveDraw() {
  stop();
}
