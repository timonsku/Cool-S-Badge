// A colour picker of our own: the browsers' built-in one is a short list of fixed colours on some devices.

import { $ } from './dom.js';

const hex = (color) => `#${color.toString(16).padStart(6, '0')}`;

export function toRgb(h, s, v) {
  const f = (n) => {
    const k = (n + h / 60) % 6;
    return Math.round(255 * (v - v * s * Math.max(0, Math.min(k, 4 - k, 1))));
  };
  return (f(5) << 16) | (f(3) << 8) | f(1);
}

export function toHsv(color) {
  const r = (color >> 16) / 255;
  const g = ((color >> 8) & 0xff) / 255;
  const b = (color & 0xff) / 255;
  const max = Math.max(r, g, b);
  const d = max - Math.min(r, g, b);
  let h = 0;
  if (d) {
    if (max === r) h = ((g - b) / d + 6) % 6;
    else if (max === g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
  }
  return { h: h * 60, s: max ? d / max : 0, v: max };
}

let state = { h: 0, s: 1, v: 1 };
let onInput = null; // told about every change while the dialog is open

function show(fromHex = false) {
  const color = toRgb(state.h, state.s, state.v);
  const area = $('#color-area');
  area.style.setProperty('--hue', `hsl(${state.h} 100% 50%)`);
  area.setAttribute('aria-valuenow', String(Math.round(state.v * 100)));
  area.setAttribute('aria-valuetext',
    `saturation ${Math.round(state.s * 100)} %, brightness ${Math.round(state.v * 100)} %`);
  $('#color-knob').style.left = `${state.s * 100}%`;
  $('#color-knob').style.top = `${(1 - state.v) * 100}%`;
  $('#color-hue').value = String(Math.round(state.h));
  $('#color-new').style.background = hex(color);
  if (!fromHex) {
    $('#color-hex').value = hex(color);
    $('#color-hex').removeAttribute('aria-invalid');
  }
  $('#color-ok').disabled = false;
  onInput?.(color);
}

function setup() {
  const area = $('#color-area');
  const pointAt = (event) => {
    const box = area.getBoundingClientRect();
    state.s = Math.min(1, Math.max(0, (event.clientX - box.left) / box.width));
    state.v = 1 - Math.min(1, Math.max(0, (event.clientY - box.top) / box.height));
    show();
  };
  area.addEventListener('pointerdown', (event) => {
    area.setPointerCapture(event.pointerId);
    area.focus();
    pointAt(event);
  });
  area.addEventListener('pointermove', (event) => {
    if (area.hasPointerCapture(event.pointerId)) pointAt(event);
  });
  area.addEventListener('keydown', (event) => {
    const step = event.shiftKey ? 0.1 : 0.01;
    const moves = { ArrowLeft: [-step, 0], ArrowRight: [step, 0], ArrowUp: [0, step], ArrowDown: [0, -step] };
    const move = moves[event.key];
    if (!move) return;
    event.preventDefault();
    state.s = Math.min(1, Math.max(0, state.s + move[0]));
    state.v = Math.min(1, Math.max(0, state.v + move[1]));
    show();
  });
  $('#color-hue').addEventListener('input', ({ target }) => {
    state.h = Number(target.value);
    show();
  });
  $('#color-hex').addEventListener('input', ({ target }) => {
    const match = /^#?([0-9a-f]{6}|[0-9a-f]{3})$/i.exec(target.value.trim());
    target.toggleAttribute('aria-invalid', !match);
    if (!match) {
      $('#color-ok').disabled = true;
      return;
    }
    const digits = match[1].length === 3 ? [...match[1]].map((c) => c + c).join('') : match[1];
    const hue = state.h;
    state = toHsv(parseInt(digits, 16));
    // grey has no hue of its own, keep the slider where it was
    if (!state.s) state.h = hue;
    show(true);
  });
  // Enter would submit with the form's first button
  $('#color-hex').addEventListener('keydown', (event) => {
    if (event.key !== 'Enter') return;
    event.preventDefault();
    if (!$('#color-ok').disabled) $('#color-ok').click();
  });
}

let ready = false;

// Resolves with the chosen colour, 'delete' when the slot is to be removed, or null. `input` sees every change
// as it happens, and the initial colour again when the dialog is cancelled.
export function pickColor(initial, title, { removable = false, input = null } = {}) {
  if (!ready) {
    setup();
    ready = true;
  }
  const dialog = $('#color-dialog');
  $('#color-title').textContent = title;
  $('#color-delete').hidden = !removable;
  $('#color-old').style.background = hex(initial);
  state = toHsv(initial);
  onInput = null;
  show();
  onInput = input;
  dialog.returnValue = '';
  dialog.showModal();
  $('#color-area').focus();
  // The buttons answer at once. The close event alone can come late, after a next showModal().
  return new Promise((resolve) => {
    const form = dialog.querySelector('form');
    const settle = (value) => {
      form.removeEventListener('submit', onSubmit);
      dialog.removeEventListener('close', onClose);
      if (value !== 'ok') onInput?.(initial);
      onInput = null;
      if (value === 'ok') resolve(toRgb(state.h, state.s, state.v));
      else resolve(value === 'delete' ? 'delete' : null);
    };
    const onSubmit = (event) => settle(event.submitter?.value);
    // Escape; a late close from the previous opening finds the dialog open again
    const onClose = () => {
      if (!dialog.open) settle(dialog.returnValue);
    };
    form.addEventListener('submit', onSubmit);
    dialog.addEventListener('close', onClose);
  });
}
