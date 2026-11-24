// The Settings tab: brightness, the mode the badge starts in, and its Bluetooth name.
// See the settings commands in cool-s-firmware/src/media_mgmt.c.

import { app } from './app.js';
import { $, notify } from './dom.js';

const PREFIX = 'LED_BLE_';
// cool-s-firmware/src/badge_settings.h: printable ASCII without quotes or backslashes
const NAME_CHARS = /^[\x20-\x21\x23-\x5b\x5d-\x7e]*$/;
// what firmware without the settings commands starts with
const LEGACY_BRIGHTNESS = 30;

// undefined: not read yet, null: the firmware has no settings commands
let settings;
let loading = false;

// the slider sends its newest value once the one before has arrived
let brightnessWanted = null;
let brightnessSending = false;

const legacy = () => settings === null;
const nameMax = () => settings?.max ?? 18;

function nameProblem(name) {
  if (name.length > nameMax()) return `At most ${nameMax()} characters.`;
  if (!NAME_CHARS.test(name)) return 'Use letters, digits, spaces and plain punctuation, without " or \\.';
  return '';
}

async function sendBrightness() {
  if (brightnessSending) return;
  brightnessSending = true;
  try {
    while (brightnessWanted !== null && app.ready) {
      const value = brightnessWanted;
      brightnessWanted = null;
      await app.badge.setBrightness(value, legacy());
      if (settings) settings.brightness = value;
    }
  } catch (error) {
    if (app.smp.connected) notify(`Could not change the brightness: ${error.message}`, 'error');
  } finally {
    brightnessSending = false;
    brightnessWanted = null;
  }
}

export async function loadSettings() {
  if (!app.smp.connected || loading) return;
  loading = true;
  try {
    settings = await app.badge.settings();
  } catch (error) {
    notify(`Could not read the settings: ${error.message}`, 'error');
  } finally {
    loading = false;
    show();
  }
}

export function resetSettings() {
  settings = undefined;
  show();
}

// fills the controls from what the badge reported
function show() {
  const known = Boolean(settings);
  const brightness = known ? settings.brightness : legacy() ? LEGACY_BRIGHTNESS : null;
  if (brightness !== null && !brightnessSending) $('#settings-brightness').value = brightness;
  for (const radio of document.querySelectorAll('input[name="settings-boot"]')) {
    radio.checked = known && radio.value === settings.boot;
  }
  $('#settings-name').value = known ? settings.name : '';
  $('#settings-name').placeholder = known ? settings.default : '';
  $('#settings-name').maxLength = nameMax();
  update();
}

function update() {
  const connected = app.ready && settings !== undefined;
  const known = Boolean(settings);

  $('#settings-unsupported').hidden = !(app.ready && legacy());
  $('#settings-disconnected').hidden = app.ready;

  const slider = $('#settings-brightness');
  slider.disabled = !connected;
  $('#settings-brightness-value').textContent = connected ? `${slider.value} %` : '–';
  $('#settings-brightness-hint').textContent = legacy()
    ? 'This firmware cannot report its brightness, so the slider starts at the default.'
    : 'The badge changes right away and saves the brightness a few seconds after you stop.';

  for (const radio of document.querySelectorAll('input[name="settings-boot"]')) {
    radio.disabled = !known || !app.ready || app.busy;
  }

  const input = $('#settings-name');
  const name = input.value.trim();
  const problem = nameProblem(name);
  input.disabled = !known || !app.ready || app.busy;
  input.setAttribute('aria-invalid', String(Boolean(problem)));
  $('#settings-name-save').disabled = input.disabled || Boolean(problem) || name === settings?.name;
  $('#settings-name-reset').disabled = input.disabled || settings?.name === '';
  $('#settings-name-hint').textContent = problem || (known
    ? `The badge is found as ${PREFIX}${settings.name || settings.default}. ` +
      'A new name shows from the next time you connect; ' +
      'a phone or computer that knows the badge may keep showing the old name for a while.'
    : '');
  $('#settings-name-hint').classList.toggle('error', Boolean(problem));
}

async function saveName(name) {
  await app.run(async () => {
    await app.badge.setSettings({ name });
    settings.name = name;
    notify(`The badge will be called ${PREFIX}${name || settings.default} once you disconnect.`);
  });
  show();
}

export function setupSettings() {
  const slider = $('#settings-brightness');
  slider.addEventListener('input', () => {
    $('#settings-brightness-value').textContent = `${slider.value} %`;
    brightnessWanted = Number(slider.value);
    sendBrightness();
  });

  for (const radio of document.querySelectorAll('input[name="settings-boot"]')) {
    radio.addEventListener('change', async () => {
      if (!radio.checked) return;
      const before = settings.boot;
      await app.run(async () => {
        await app.badge.setSettings({ boot: radio.value });
        settings.boot = radio.value;
      });
      // put the choice back if it did not arrive
      if (settings && settings.boot === before) show();
    });
  }

  const input = $('#settings-name');
  input.addEventListener('input', update);
  $('#settings-name-form').addEventListener('submit', (event) => {
    event.preventDefault();
    const name = input.value.trim();
    if (!$('#settings-name-save').disabled) saveName(name);
  });
  $('#settings-name-reset').addEventListener('click', () => saveName(''));
  $('#settings-go-firmware').addEventListener('click', () => {
    $('#tab-firmware').click();
    $('#tab-firmware').focus();
  });

  app.addEventListener('change', update);
  show();

  // read again whenever the tab is opened, in case something else changed them since connecting
  return () => {
    if (app.ready && !app.busy) loadSettings();
  };
}
