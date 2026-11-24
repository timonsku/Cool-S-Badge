// The badge's console output: its log, printk and Lua's print, over the Nordic UART
// Service (cool-s-firmware/src/nus_console.c). Shown in the Console tab and under
// the Code tab's editor.

import { app } from './app.js';
import { $, el, notify } from './dom.js';
import { NUS_SERVICE } from './smp.js';

const NUS_TX = '6e400003-b5a3-f393-e0a9-e50e24dcca9e';
// kept in the page; older output is dropped a line at a time
const MAX_TEXT = 256 * 1024;
// the Code tab shows only the end
const CODE_TAIL = 8 * 1024;

let text = '';
let decoder = null;
// null: not connected, false: the firmware has no console, true: receiving
let receiving = null;
let characteristic = null;
let queued = false;

const views = [];

function onNotification({ target }) {
  append(decoder.decode(target.value, { stream: true }));
}

function append(more) {
  text += more;
  if (text.length > MAX_TEXT) {
    const cut = text.indexOf('\n', text.length - MAX_TEXT);
    text = text.slice(cut < 0 ? text.length - MAX_TEXT : cut + 1);
  }
  render();
}

// once per frame, however many notifications arrive
function render() {
  if (queued) return;
  queued = true;
  requestAnimationFrame(() => {
    queued = false;
    update();
    for (const { pre, tail } of views) {
      if (pre.offsetParent === null) continue; // hidden, filled in when shown
      const atEnd = pre.scrollHeight - pre.scrollTop - pre.clientHeight < 24;
      pre.textContent = tail ? text.slice(-tail) : text;
      if (atEnd) pre.scrollTop = pre.scrollHeight;
    }
  });
}

function status() {
  if (receiving === true) return 'Showing what the badge prints: its log and the output of Lua scripts.';
  if (receiving === false) {
    return 'The firmware on this badge has no console. Update it to see its output here. ' +
      'If you just did, disconnect and connect again.';
  }
  return app.ready ? '' : 'Connect a badge to see what it prints.';
}

function update() {
  for (const node of document.querySelectorAll('.console-status')) node.textContent = status();
  $('#console-download').disabled = !text;
  $('#console-clear').disabled = !text;
}

// During connecting, after the other services, as Android takes one GATT operation at a time.
// Never fails the connection: the badge works without its console.
export async function connectConsole() {
  try {
    await subscribe();
  } catch (error) {
    if (error.name === 'NotFoundError') {
      receiving = false;
    } else if (app.smp.connected) {
      notify(`Could not show the badge's console: ${error.message}`, 'error');
    }
    update();
  }
}

async function subscribe() {
  const service = await app.smp.device.gatt.getPrimaryService(NUS_SERVICE);
  characteristic = await service.getCharacteristic(NUS_TX);
  decoder = new TextDecoder();
  // the same object can come back on reconnect, so do not stack listeners
  characteristic.removeEventListener('characteristicvaluechanged', onNotification);
  characteristic.addEventListener('characteristicvaluechanged', onNotification);
  // the badge sends what it kept from before first, so mark where this connection starts
  if (text && !text.endsWith('\n')) append('\n');
  append(`--- connected ${new Date().toLocaleTimeString()} ---\n`);
  await characteristic.startNotifications();
  receiving = true;
  update();
}

function disconnected() {
  receiving = null;
  if (decoder) append(decoder.decode());
  decoder = null;
  update();
}

function download() {
  const url = URL.createObjectURL(new Blob([text], { type: 'text/plain' }));
  const link = el('a', { href: url, download: `cool-s-console-${new Date().toISOString().slice(0, 19).replace(/:/g, '-')}.txt` });
  document.body.append(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function clear() {
  text = '';
  for (const { pre } of views) pre.textContent = '';
  update();
}

export function setupConsole() {
  views.push({ pre: $('#console-output') }, { pre: $('#code-console'), tail: CODE_TAIL });
  $('#console-clear').addEventListener('click', clear);
  $('#code-console-clear').addEventListener('click', clear);
  $('#console-download').addEventListener('click', download);
  // the Code tab's output fills in when it is opened
  $('#code-console-box').addEventListener('toggle', render);
  app.smp.addEventListener('disconnect', disconnected);
  app.addEventListener('change', update);
  update();

  // called when the tab is shown, as hidden output is not kept up to date
  return () => {
    render();
    // start at the newest line
    requestAnimationFrame(() => {
      for (const { pre } of views) pre.scrollTop = pre.scrollHeight;
    });
  };
}
