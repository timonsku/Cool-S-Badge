import { app } from './app.js';
import { $, el, notify, confirmDialog } from './dom.js';
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
// how long the badge waits for the presses (cool-s-firmware/src/update_approval.c)
const APPROVAL_SECONDS = 30;

let selected = null;
let selection = 0;
// hash of the image we restarted into, until the result is known
let expected = null;
let updating = null;
let afterReconnect;
// firmware/manifest.json, published with the site: the newest build of main and the latest release
let manifest = null;
let downloading = false;
let announced = '';
let pressTimer = null;

const hexHash = (image) => (image?.hash ? toHex(image.hash) : '');
const activeImage = () => app.images.find((image) => image.active);
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

export const isUpdating = () => updating !== null;
export const stopWaiting = () => updating?.abort();

// Any later status means the badge answered, so the prompt goes away.
function setStatus(text, { kind = '', progress } = {}) {
  clearInterval(pressTimer);
  $('#press-prompt').hidden = true;
  $('#update-status').textContent = text;
  $('#update-status').className = `status ${kind}`;
  const bar = $('#update-bar');
  bar.hidden = progress === undefined;
  if (progress === null) bar.removeAttribute('value');
  else if (progress !== undefined) bar.value = progress;
}

function askForPresses() {
  setStatus('To confirm the update, press all 4 buttons on the badge twice, the order does not matter.', { progress: null });
  const deadline = Date.now() + APPROVAL_SECONDS * 1000;
  const tick = () => {
    const left = Math.ceil((deadline - Date.now()) / 1000);
    $('#press-countdown').textContent = left > 0
      ? `${left} seconds left`
      : 'Time is up. If the badge no longer breathes yellow, start the update again.';
    if (left <= 0) clearInterval(pressTimer);
  };
  tick();
  pressTimer = setInterval(tick, 1000);
  const prompt = $('#press-prompt');
  prompt.hidden = false;
  prompt.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
}

// '0.10' is newer than '0.9'
function compareVersions(a, b) {
  const pa = a.split('.').map(Number);
  const pb = b.split('.').map(Number);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const diff = (pa[i] ?? 0) - (pb[i] ?? 0);
    if (diff) return Math.sign(diff);
  }
  return 0;
}

const formatDate = (iso) => new Date(iso).toLocaleDateString(undefined, { dateStyle: 'medium' });

function updateRemote() {
  const release = manifest?.release;
  // the button stays, also to reinstall the release over a development build of the same version
  const current = Boolean(app.ready && release && app.version === release.version);
  const newer = Boolean(app.ready && release && !current &&
    (!app.version || compareVersions(release.version, app.version) > 0));
  $('#fw-upgrade').hidden = !release;
  if (release) {
    const running = app.version ? `This badge runs v${app.version}.` : 'This badge runs older firmware.';
    let text = `The latest release is v${release.version}, released ${formatDate(release.date)}.`;
    if (current) text = `This badge runs the latest release, v${release.version}.`;
    else if (newer) text = `Firmware v${release.version} is available, released ${formatDate(release.date)}. ${running}`;
    else if (app.ready) text = `The latest release is v${release.version}. ${running}`;
    $('#fw-upgrade-text').textContent = text;
    $('#fw-upgrade-install').textContent = newer ? `Upgrade to v${release.version}` : `Install v${release.version}`;
    $('#fw-upgrade-install').classList.toggle('primary', newer);
    $('#fw-upgrade-notes').href = release.url;
  }
  $('#fw-upgrade-install').disabled = !app.ready || app.busy || downloading;

  // once per badge and version, as an update reconnects several times
  const key = newer ? `${app.smp.device?.id}:${app.version}` : '';
  if (key && key !== announced) notify(`Firmware v${release.version} is available in the Firmware Update tab.`);
  if (app.ready) announced = key;

  const latest = manifest?.latest;
  $('#fw-dev').hidden = !latest;
  if (latest) {
    const link = el('a', { href: latest.url, target: '_blank', rel: 'noopener noreferrer', className: 'mono' },
      latest.commit.slice(0, 7));
    $('#fw-dev-text').replaceChildren(
      'The newest build of the main branch, commit ', link,
      ` from ${formatDate(latest.date)}, reports itself as v${latest.version}.`,
    );
  }
  $('#fw-dev-install').disabled = !app.ready || app.busy || downloading;
}

function update() {
  updateRemote();
  const active = app.ready ? activeImage() : null;
  $('#fw-version').textContent = app.ready ? (app.version ? `v${app.version}` : 'Unknown') : '–';
  $('#fw-hash').textContent = active ? hexHash(active).slice(0, 16) : '–';
  $('#fw-state').textContent = active ? (active.confirmed ? 'Confirmed' : 'On trial') : '–';

  const installed = Boolean(selected && active && hexHash(active) === toHex(selected.hash));
  $('#firmware-installed').hidden = !installed;
  $('#firmware-input').disabled = app.busy || downloading;
  $('#install').disabled = !app.ready || app.busy || downloading || !selected || installed;
}

function showSelected(firmware) {
  selected = firmware;
  $('#file-fw-name').textContent = firmware.fileName;
  $('#file-fw-size').textContent = formatBytes(firmware.data.length);
  $('#file-fw-hash').textContent = toHex(firmware.hash).slice(0, 16);
  $('#firmware-summary').hidden = false;
  $('#firmware-error').hidden = true;
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
    showSelected(firmware);
  } catch (error) {
    if (id !== selection) return;
    $('#firmware-error').textContent = error.message;
    $('#firmware-error').hidden = false;
  }
  update();
}

const KEEP_NEARBY = 'This takes a few minutes. Keep the badge powered and nearby, and keep this page open.';

async function install(firmware, title = 'Update the firmware?', message = KEEP_NEARBY) {
  const confirmed = await confirmDialog(title, message, 'Update');
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
        if (app.needsPresses && !uploadedNow) askForPresses();
        await app.badge.testImage(firmware.hash);
      }

      await restartInto(target, signal);
    } catch (error) {
      const reason = app.needsPresses && app.badge.isNotApproved(error)
        ? 'It was not confirmed on the badge, which needs all 4 buttons pressed twice. Start the update again.'
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
      if (app.needsPresses) askForPresses();
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

// Downloads a package published with the site, then installs it like a selected file.
async function installPublished(entry, title, message) {
  if (!entry) return;
  ++selection;
  downloading = true;
  update();
  let firmware;
  try {
    setStatus('Downloading the firmware…', { progress: null });
    const response = await fetch(entry.file, { cache: 'no-cache' });
    if (!response.ok) throw new Error(`The download failed (HTTP ${response.status}).`);
    const name = entry.file.split('/').pop();
    firmware = await readFirmware(new File([await response.blob()], name));
    setStatus('');
  } catch (error) {
    setStatus(`Could not get the firmware: ${error.message}`, { kind: 'error' });
    return;
  } finally {
    downloading = false;
    update();
  }

  showSelected(firmware);
  update();
  const active = activeImage();
  if (active && hexHash(active) === toHex(firmware.hash)) return;
  await install(firmware, title, message);
}

async function loadManifest() {
  try {
    const response = await fetch('firmware/manifest.json', { cache: 'no-cache' });
    if (!response.ok) return;
    manifest = await response.json();
  } catch {
    // opened locally or offline: only files can be installed
    return;
  }
  update();
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

  $('#install').addEventListener('click', () => install(selected));
  $('#fw-upgrade-install').addEventListener('click', () => {
    const { release } = manifest;
    installPublished(release, `Install firmware v${release.version}?`, KEEP_NEARBY);
  });
  $('#fw-dev-install').addEventListener('click', () => {
    const { latest } = manifest;
    installPublished(
      latest,
      'Flash the latest commit?',
      `This installs commit ${latest.commit.slice(0, 7)}, a development build that has not been tested like a release. ${KEEP_NEARBY}`,
    );
  });
  app.addEventListener('change', update);
  update();
  loadManifest();
}
