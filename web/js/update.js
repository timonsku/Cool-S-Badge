import { app } from './app.js';
import { $, notify, confirmDialog } from './dom.js';
import { readFirmware, toHex } from './firmware-file.js';
import { formatBytes } from './formats.js';

const RESTART_TIMEOUT = 180000;
const RETRY_DELAY = 3000;
// Firmware up to v0.6 erases a used slot for about 11 seconds and often loses the link during
// it (supervision timeout, mostly when connection parameters change mid-erase).
// The badge finishes the erase regardless, so reconnect and resume.
const RESUME_TIMEOUT = 90000;
const MAX_DROPS_WITHOUT_PROGRESS = 5;
// New firmware confirms itself after 30 healthy seconds and reverts after 90 bad ones.
// Releases up to v0.6 confirm at boot.
const PROVING_TIMEOUT = 120000;
const PRESS_PROMPT = 'Press each of the four buttons on the badge twice to allow the update. ' +
  'It breathes yellow while it waits, for up to 30 seconds.';

let selected = null;
let selection = 0;
// hash of the image we restarted into, until the result is known
let expected = null;
let updating = null;
let afterReconnect;

const hexHash = (image) => (image?.hash ? toHex(image.hash) : '');
const activeImage = () => app.images.find((image) => image.active);
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

export const isUpdating = () => updating !== null;
export const stopWaiting = () => updating?.abort();

function setStatus(text, { kind = '', progress } = {}) {
  $('#update-status').textContent = text;
  $('#update-status').className = `status ${kind}`;
  const bar = $('#update-bar');
  bar.hidden = progress === undefined;
  if (progress === null) bar.removeAttribute('value');
  else if (progress !== undefined) bar.value = progress;
}

function update() {
  const active = app.ready ? activeImage() : null;
  $('#fw-version').textContent = app.ready ? (app.version ? `v${app.version}` : 'Unknown') : '–';
  $('#fw-hash').textContent = active ? hexHash(active).slice(0, 16) : '–';
  $('#fw-state').textContent = active ? (active.confirmed ? 'Confirmed' : 'On trial') : '–';

  const installed = Boolean(selected && active && hexHash(active) === toHex(selected.hash));
  $('#firmware-installed').hidden = !installed;
  $('#firmware-input').disabled = app.busy;
  $('#install').disabled = !app.ready || app.busy || !selected || installed;
}

async function selectFile(file) {
  if (!file) return;
  const id = ++selection;
  selected = null;
  update();
  $('#firmware-summary').hidden = true;
  $('#firmware-error').hidden = true;
  setStatus('');

  try {
    const firmware = await readFirmware(file);
    if (id !== selection) return;
    selected = firmware;
    $('#file-fw-name').textContent = firmware.fileName;
    $('#file-fw-size').textContent = formatBytes(firmware.data.length);
    $('#file-fw-hash').textContent = toHex(firmware.hash).slice(0, 16);
    $('#firmware-summary').hidden = false;
  } catch (error) {
    if (id !== selection) return;
    $('#firmware-error').textContent = error.message;
    $('#firmware-error').hidden = false;
  }
  update();
}

async function install() {
  const firmware = selected;
  const confirmed = await confirmDialog(
    'Update the firmware?',
    'This takes a few minutes. Keep the badge powered and nearby, and keep this page open.',
    'Update',
  );
  if (!confirmed) return;

  await app.run(async () => {
    // an earlier update's result is settled by now
    expected = null;
    // set for the whole update, in which losing the link is expected
    updating = new AbortController();
    const { signal } = updating;
    const target = toHex(firmware.hash);
    const inOtherSlot = (image) => !image.active && hexHash(image) === target;
    let uploadedNow = false;
    try {
      setStatus('Checking the badge…', { progress: null });
      await loadFirmwareState();
      if (activeImage()?.confirmed === false) {
        // until it is confirmed, the other slot holds the fallback and refuses uploads
        const proven = await waitUntilConfirmed('Waiting for the running firmware to prove that it works…', signal);
        if (!proven) throw new Error('The running firmware is not confirmed. Restart the badge and try again.');
      }
      let images = await app.badge.images();

      if (!images.some(inOtherSlot)) {
        await uploadResumable(firmware, signal);
        uploadedNow = true;
        setStatus('Checking the uploaded image…', { progress: null });
        images = await app.badge.images();
      }

      const uploaded = images.find(inOtherSlot);
      if (!uploaded) throw new Error('The badge does not report the uploaded image. Please try again.');
      if (!uploaded.pending) {
        // straight after an upload the presses were already given
        if (app.needsPresses && !uploadedNow) setStatus(PRESS_PROMPT, { kind: 'waiting' });
        await app.badge.testImage(firmware.hash);
      }

      await restartInto(target, signal);
    } catch (error) {
      const reason = app.needsPresses && app.badge.isNotApproved(error)
        ? 'It was not allowed on the badge. Press Update firmware to try again.'
        : error.message;
      setStatus(`Update failed: ${reason}`, { kind: 'error' });
      // a reconnect along the way cleared the media list
      if (app.smp.connected) await afterReconnect();
    } finally {
      updating = null;
    }
  });
}

async function reconnect(signal, timeout) {
  const deadline = Date.now() + timeout;
  while (!signal.aborted && Date.now() < deadline) {
    await sleep(RETRY_DELAY);
    if (signal.aborted) break;
    try {
      await app.smp.connect();
      await loadFirmwareState();
    } catch {
      app.smp.disconnect();
      continue;
    }
    app.ready = true;
    app.changed();
    return true;
  }
  return false;
}

async function uploadResumable(firmware, signal) {
  let furthest = 0;
  let drops = 0;
  const onProgress = (value) => {
    if (value > furthest) {
      furthest = value;
      drops = 0;
    }
    setStatus(`Uploading firmware… ${Math.floor(value * 100)}%`, { progress: value });
  };

  for (;;) {
    if (furthest === 0) {
      if (app.needsPresses) setStatus(PRESS_PROMPT, { kind: 'waiting' });
      else setStatus('Preparing flash. This can take up to a minute…', { progress: null });
    }
    try {
      await app.badge.uploadImage(firmware.data, firmware.sha, onProgress);
      return;
    } catch (error) {
      // still connected means the badge refused, which trying again will not change
      if (app.smp.connected || ++drops > MAX_DROPS_WITHOUT_PROGRESS) throw error;
    }
    setStatus('The connection dropped. Reconnecting to resume the upload…', { progress: null });
    if (!(await reconnect(signal, RESUME_TIMEOUT))) {
      throw new Error('Could not reconnect. Press Connect, then Update firmware to resume.');
    }
  }
}

async function restartInto(target, signal) {
  try {
    await app.badge.reset();
  } catch (error) {
    // the link can drop before the response to the reset arrives
    if (app.smp.connected) throw error;
  }
  expected = target;
  app.ready = false;
  app.changed();
  setStatus('Installing. The badge restarts, which takes 30 to 60 seconds…', { progress: null });

  // do not reconnect to the old firmware in the moment before it resets
  await sleep(RETRY_DELAY);
  app.smp.disconnect();

  if (await reconnect(signal, RESTART_TIMEOUT)) {
    await checkUpdateResult(signal);
    if (app.smp.connected) await afterReconnect();
  } else {
    setStatus('Not reconnected. Once the badge is back, press Connect to check the update.', { kind: 'error' });
  }
}

// Never confirm from here: that would keep an image that cannot run on its own.
// signal is only given during an update, where a restart (a revert) is followed.
async function waitUntilConfirmed(text, signal) {
  const start = hexHash(activeImage());
  const deadline = Date.now() + PROVING_TIMEOUT;
  while (Date.now() < deadline) {
    const active = activeImage();
    if (hexHash(active) !== start) return false;
    if (active?.confirmed) return true;

    setStatus(text, { progress: null });
    await sleep(RETRY_DELAY);
    try {
      await loadFirmwareState();
      app.changed();
    } catch (error) {
      if (app.smp.connected || !signal) throw error;
      if (!(await reconnect(signal, RESTART_TIMEOUT))) throw error;
    }
  }
  return false;
}

export async function loadFirmwareState() {
  app.images = await app.badge.images();
  Object.assign(app, await app.badge.firmwareInfo());
}

export async function checkUpdateResult(signal) {
  if (!expected) return;

  if (hexHash(activeImage()) === expected) {
    await waitUntilConfirmed('The new firmware is running. Waiting for it to prove that it works…', signal);
  }
  const active = activeImage();

  if (hexHash(active) === expected) {
    if (!active.confirmed) {
      setStatus(
        'The new firmware runs but has not confirmed itself. The badge returns to the previous one when it restarts.',
        { kind: 'error' },
      );
      expected = null;
      app.changed();
      return;
    }
    setStatus('Update complete. The badge is running the new firmware.', { kind: 'success' });
    notify('Firmware updated.', 'success');
  } else if (app.images.some((image) => image.pending && hexHash(image) === expected)) {
    setStatus('The update is ready but the badge has not restarted. Turn it off and on again.', { kind: 'error' });
  } else {
    setStatus(
      'The badge is still running its previous firmware. The new image was rejected or failed to start.',
      { kind: 'error' },
    );
  }
  expected = null;
  app.changed();
}

export function setupUpdate(onReconnect) {
  afterReconnect = onReconnect;

  const input = $('#firmware-input');
  input.addEventListener('change', () => {
    selectFile(input.files[0]);
    input.value = '';
  });

  const zone = $('#firmware-drop');
  zone.addEventListener('dragover', (event) => {
    event.preventDefault();
    if (!input.disabled) zone.classList.add('over');
  });
  zone.addEventListener('dragleave', () => zone.classList.remove('over'));
  zone.addEventListener('drop', (event) => {
    event.preventDefault();
    zone.classList.remove('over');
    if (!input.disabled) selectFile(event.dataTransfer.files[0]);
  });

  $('#install').addEventListener('click', install);
  app.addEventListener('change', update);
  update();
}
