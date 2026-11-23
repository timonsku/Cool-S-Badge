import { app } from './app.js';
import { $, notify, setupTabs } from './dom.js';
import { setupMedia, refreshMedia, resetMedia } from './media.js';
import { setupDraw, leaveDraw } from './draw.js';
import { setupCode } from './code.js';
import { setupUpdate, isUpdating, stopWaiting, loadFirmwareState, checkUpdateResult } from './update.js';

let connecting = false;
let disconnecting = false;

async function loadMedia() {
  try {
    await refreshMedia();
  } catch (error) {
    // a broken file system must not block a firmware update
    notify(`Could not read the media storage: ${error.message}`, 'error');
  }
}

const updateMedia = setupMedia();
const showDraw = setupDraw();
const showCode = setupCode();
setupUpdate(loadMedia);

const selectMainTab = setupTabs($('#main-tabs'), (tab) => {
  if (tab.id === 'tab-media') updateMedia();
  // the editor polls the badge in the background, once the previews have stopped
  else app.stopBackground().then(() => { if (tab.id === 'tab-code') showCode(); });
  if (tab.id === 'tab-draw') showDraw();
  else leaveDraw();
});
$('#go-firmware').addEventListener('click', () => {
  selectMainTab($('#tab-firmware'));
  $('#tab-firmware').focus();
});

async function connect() {
  try {
    await app.smp.choose();
  } catch (error) {
    // NotFoundError is also what closing the device picker gives
    if (error.name !== 'NotFoundError') notify(error.message, 'error');
    return;
  }

  await app.run(async () => {
    connecting = true;
    app.changed();
    try {
      resetMedia();
      await app.smp.connect();
      await loadFirmwareState();
      await loadMedia();
      if (!app.smp.connected) return;
      app.ready = true;
      await checkUpdateResult();
    } catch (error) {
      app.smp.disconnect();
      throw error;
    } finally {
      connecting = false;
    }
  });
}

$('#connect').addEventListener('click', () => {
  if (isUpdating()) {
    stopWaiting();
  } else if (app.ready) {
    disconnecting = true;
    app.smp.disconnect();
    disconnecting = false;
  } else {
    connect();
  }
});

app.smp.addEventListener('disconnect', () => {
  if (!disconnecting && !connecting && !app.restarting && !isUpdating()) notify('The badge disconnected.', 'error');
  resetMedia();
});

app.addEventListener('change', () => {
  const reconnecting = isUpdating() && !app.ready;
  let label = 'Not connected';
  if (connecting && !app.ready) label = 'Connecting…';
  else if (reconnecting) label = 'Reconnecting…';
  else if (app.ready) label = app.smp.device.name ?? 'Connected';

  $('#connection-label').textContent = label;
  $('#connection').classList.toggle('online', app.ready);

  const button = $('#connect');
  button.textContent = reconnecting ? 'Stop waiting' : app.ready ? 'Disconnect' : 'Connect';
  button.disabled = !navigator.bluetooth || (app.busy && !reconnecting);
});

window.addEventListener('beforeunload', (event) => {
  if (app.busy) event.preventDefault();
});
// a file dropped next to a drop zone would otherwise replace the page
for (const type of ['dragover', 'drop']) {
  window.addEventListener(type, (event) => event.preventDefault());
}

async function checkBrowser() {
  let problem = '';
  if (!window.isSecureContext) {
    problem = 'Bluetooth needs a secure page. Open this site over HTTPS or from localhost.';
  } else if (!navigator.bluetooth) {
    problem = 'This browser has no Web Bluetooth. Use Chrome or Edge on Windows, macOS, Android or ChromeOS. ' +
      'On Linux, enable it in chrome://flags. On iPhone and iPad, use a Web Bluetooth browser such as Bluefy.';
  } else if ((await navigator.bluetooth.getAvailability?.()) === false) {
    problem = 'Bluetooth is switched off or not available on this device.';
  }
  $('#browser-warning').hidden = !problem;
  $('#browser-warning').textContent = problem;
}

app.changed();
checkBrowser();
