import { app } from './app.js';
import { $, el, notify, setupTabs } from './dom.js';
import { setupMedia, refreshMedia, resetMedia } from './media.js';
import { setupDraw, leaveDraw } from './draw.js';
import { setupCode } from './code.js';
import { setupSettings, loadSettings, resetSettings } from './settings.js';
import { setupBattery } from './battery.js';
import { setupConsole, connectConsole } from './console.js';
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
const showSettings = setupSettings();
setupBattery();
const showConsole = setupConsole();
// after an update the badge restarts, and comes back with the new firmware's features
setupUpdate(async () => {
  await loadMedia();
  await loadSettings();
  await connectConsole();
});

const selectMainTab = setupTabs($('#main-tabs'), (tab) => {
  $('#main-tabs-menu').value = tab.id;
  if (tab.id === 'tab-media') updateMedia();
  // the editor polls the badge in the background, once the previews have stopped
  else {
    app.stopBackground().then(() => {
      if (tab.id === 'tab-code') showCode();
      if (tab.id === 'tab-settings') showSettings();
      if (tab.id === 'tab-console') showConsole();
    });
  }
  if (tab.id === 'tab-draw') showDraw();
  else leaveDraw();
});
// A menu instead of the tabs when they do not fit side by side. The tabs keep their
// place in the layout, hidden, so their width still tells when they fit again.
function setupTabsMenu(tablist, menu) {
  for (const tab of tablist.querySelectorAll('[role="tab"]')) {
    menu.append(el('option', { value: tab.id, textContent: tab.textContent }));
    if (tab.getAttribute('aria-selected') === 'true') menu.value = tab.id;
  }
  menu.addEventListener('change', () => selectMainTab(document.getElementById(menu.value)));

  const fit = () => {
    const narrow = tablist.scrollWidth > tablist.clientWidth + 1;
    tablist.classList.toggle('collapsed', narrow);
    tablist.inert = narrow;
    menu.hidden = !narrow;
  };
  new ResizeObserver(fit).observe(tablist);
}
setupTabsMenu($('#main-tabs'), $('#main-tabs-menu'));

$('#go-firmware').addEventListener('click', () => {
  selectMainTab($('#tab-firmware'));
  $('#tab-firmware').focus();
});

async function connect() {
  try {
    await app.smp.choose();
  } catch (error) {
    // NotFoundError is also what closing the device picker gives
    if (error?.name === 'NotFoundError') return;
    // Bluefy on iOS can reject with a plain value instead of an Error
    const reason = error?.message || (typeof error === 'string' ? error : JSON.stringify(error));
    notify(`Could not open the Bluetooth device picker: ${reason ?? 'no reason given'}`, 'error');
    return;
  }

  await app.run(async () => {
    connecting = true;
    app.changed();
    try {
      resetMedia();
      resetSettings();
      await app.smp.connect();
      await loadFirmwareState();
      await loadMedia();
      await loadSettings();
      await connectConsole();
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

const onConnectClick = () => {
  if (isUpdating()) {
    stopWaiting();
  } else if (app.ready) {
    disconnecting = true;
    app.smp.disconnect();
    disconnecting = false;
  } else {
    connect();
  }
};
for (const button of document.querySelectorAll('.connect-button')) button.addEventListener('click', onConnectClick);

// the bar appears once the header's controls are out of sight
const topbar = $('#topbar');
new IntersectionObserver(([entry]) => {
  const shown = !entry.isIntersecting && entry.boundingClientRect.top < 0;
  topbar.classList.toggle('shown', shown);
  topbar.inert = !shown;
}).observe($('#connection'));

app.smp.addEventListener('disconnect', () => {
  if (!disconnecting && !connecting && !app.restarting && !isUpdating()) notify('The badge disconnected.', 'error');
  resetMedia();
  resetSettings();
});

app.addEventListener('change', () => {
  const reconnecting = isUpdating() && !app.ready;
  let label = 'Not connected';
  if (connecting && !app.ready) label = 'Connecting…';
  else if (reconnecting) label = 'Reconnecting…';
  else if (app.ready) label = app.smp.device.name ?? 'Connected';

  $('#connection-label').textContent = label;
  for (const other of document.querySelectorAll('.connection-label')) other.textContent = label;
  for (const box of document.querySelectorAll('.connection')) box.classList.toggle('online', app.ready);

  for (const button of document.querySelectorAll('.connect-button')) {
    button.textContent = reconnecting ? 'Stop waiting' : app.ready ? 'Disconnect' : 'Connect';
    button.disabled = !navigator.bluetooth || (app.busy && !reconnecting);
  }
});

window.addEventListener('beforeunload', (event) => {
  if (app.busy) event.preventDefault();
});
// a file dropped next to a drop zone would otherwise replace the page
for (const type of ['dragover', 'drop']) {
  window.addEventListener(type, (event) => event.preventDefault());
}

const BLUEFY_URL = 'https://apps.apple.com/app/bluefy-web-ble-browser/id1492822055';
// iPhones, and iPads in mobile mode
const iOS = /iPad|iPhone|iPod/.test(navigator.userAgent);
const macOrIPad = !iOS && /Macintosh/.test(navigator.userAgent);

// Every browser on iOS uses Safaris engine, which has no Web Bluetooth. Bluefy adds it, so
// an iOS browser that has it is Bluefy or one like it, and needs no hint.
function suggestBluefy(maybeMac) {
  const copy = el('button', { type: 'button', className: 'link', textContent: 'copy the address of this page' });
  copy.addEventListener('click', async () => {
    try {
      await navigator.clipboard.writeText(location.href);
      notify('Copied. Paste it into Bluefy\'s address bar.', 'success');
    } catch {
      notify(`Open ${location.href} in Bluefy.`);
    }
  });
  const warning = $('#browser-warning');
  const store = el('a', {
    href: BLUEFY_URL, target: '_blank', rel: 'noopener noreferrer', textContent: 'Bluefy browser from the App Store',
  });
  warning.replaceChildren(...(maybeMac
    ? ['This browser has no Web Bluetooth. On a Mac, use Chrome or Edge. On an iPad, install the free ', store,
      ', then ', copy, ' and open it there.']
    : ['Safari and the other browsers on iPhone and iPad cannot use Bluetooth. Install the free ', store,
      ', then ', copy, ' and open it there.']));
  warning.hidden = false;
}

async function checkBrowser() {
  let problem = '';
  if ((iOS || macOrIPad) && window.isSecureContext && !navigator.bluetooth) {
    suggestBluefy(macOrIPad);
    return;
  }
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
