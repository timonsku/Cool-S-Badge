// The badge's battery, next to its name in the connection controls.
// See the battery command in cool-s-firmware/src/media_mgmt.c.

import { app } from './app.js';

// the badge itself reads the battery every 30 seconds
const POLL_MS = 30000;

let timer = null;
let reading = false;

function show(battery) {
  let text = '';
  let level = '';
  let title = '';
  if (battery?.usb) {
    text = 'USB';
    title = 'On USB power, charging. The badge can read its battery once it is unplugged.';
  } else if (battery?.percent !== undefined) {
    text = `${battery.percent} %`;
    level = battery.percent >= 60 ? 'good' : battery.percent >= 30 ? 'low' : 'empty';
    title = `Battery about ${battery.percent} % (${(battery.mv / 1000).toFixed(2)} V)`;
  }
  for (const badge of document.querySelectorAll('.battery')) {
    badge.hidden = !text;
    badge.textContent = text;
    badge.title = title;
    badge.className = `battery ${level}`.trim();
    badge.setAttribute('aria-label', title || text);
  }
}

async function read() {
  // stays out of the way of a firmware update or a backup
  if (reading || !app.ready || app.busy) return;
  reading = true;
  try {
    const battery = await app.badge.battery();
    // firmware without the command: nothing to show, so stop asking
    if (battery === null) stopBattery();
    if (app.ready) show(battery);
  } catch {
    // a missed reading is not worth a message; the next one comes soon
  } finally {
    reading = false;
  }
}

function stopBattery() {
  clearInterval(timer);
  timer = null;
  show(null);
}

// reads whenever a badge is connected, including after it restarted with new firmware
export function setupBattery() {
  let wasReady = false;
  app.addEventListener('change', () => {
    if (app.ready === wasReady) return;
    wasReady = app.ready;
    stopBattery();
    if (app.ready) {
      timer = setInterval(read, POLL_MS);
      // after the connect that made it ready has finished
      setTimeout(read);
    }
  });
}
