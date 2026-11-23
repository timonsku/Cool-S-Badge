// SMP (Simple Management Protocol) over Web Bluetooth.

import { encode, decode } from './cbor.js';

const SERVICE = '8d53dc1d-1db7-4cd3-868b-8a527460aa84';
const CHARACTERISTIC = 'da2e7828-fbce-4e01-ae9e-261174997c48';
const NAME_PREFIX = 'LED_BLE_';
// the iPixel app's service, which the Draw tab's live preview uses (cool-s-firmware/src/main.c)
export const IPIXEL_SERVICE = 0x00fa;

const HEADER_SIZE = 8;
const VERSION_2 = 1 << 3;
export const READ = 0;
export const WRITE = 2;

// 20 bytes always fits. 512 is the largest GATT value Web Bluetooth will write.
const MIN_FRAGMENT = 20;
const MAX_FRAGMENT = 512;
const CONNECT_TIMEOUT = 20000;
const REQUEST_TIMEOUT = 10000;

export const RC = { NO_MEMORY: 2, INVALID: 3, NOT_FOUND: 5, BAD_STATE: 6, NOT_SUPPORTED: 8, BUSY: 10, ACCESS_DENIED: 11 };

const RC_TEXT = [
  'OK', 'Unknown error', 'Out of memory or space', 'Invalid request', 'Timed out', 'Not found',
  'Not possible in the current state', 'Response too large', 'Not supported by this firmware',
  'Corrupt request', 'Busy', 'Access denied',
];

const GROUP_TEXT = {
  1: {
    9: 'No free firmware slot. Restart the badge and try again',
    12: 'Writing to flash failed',
    13: 'Erasing flash failed',
    23: 'The file is not a firmware image',
    28: 'Another update is already pending. Restart the badge and try again',
    29: 'The firmware image is not bootable on this badge',
    30: 'The firmware image is too large',
  },
  8: {
    2: 'Invalid file name',
    3: 'File not found',
    7: 'Reading the file failed',
    10: 'Writing the file failed. The storage may be full',
    14: 'The storage is not available',
    15: 'The storage is read-only',
  },
};

export class SmpError extends Error {
  // group is null for the generic MCUmgr result codes
  constructor(rc, group = null) {
    const text = group === null ? RC_TEXT[rc] : GROUP_TEXT[group]?.[rc];
    super(`${text ?? `The badge reported error ${group ?? 'mcumgr'}/${rc}`}.`);
    this.name = 'SmpError';
    this.rc = rc;
    this.group = group;
  }
}

function withTimeout(promise, ms, message, onTimeout) {
  let timer;
  const timeout = new Promise((_, reject) => {
    timer = setTimeout(() => {
      reject(new Error(message));
      onTimeout();
    }, ms);
  });
  return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}

export class SmpClient extends EventTarget {
  device = null;
  connected = false;
  bufferSize = 256;
  fragmentSize = MIN_FRAGMENT;

  #characteristic = null;
  #queue = Promise.resolve();
  #sequence = 0;
  #pending = null;
  #received = new Uint8Array(0);
  #firstNotification = 0;

  async choose() {
    const device = await navigator.bluetooth.requestDevice({
      filters: [{ namePrefix: NAME_PREFIX }],
      optionalServices: [SERVICE, IPIXEL_SERVICE],
    });

    if (device !== this.device) {
      if (this.device) {
        this.disconnect();
        this.device.removeEventListener('gattserverdisconnected', this.#onDisconnect);
      }
      this.device = device;
      device.addEventListener('gattserverdisconnected', this.#onDisconnect);
    }
  }

  async connect() {
    const { gatt } = this.device;
    try {
      await withTimeout(
        this.#open(gatt), CONNECT_TIMEOUT, 'Connecting to the badge timed out.', () => gatt.disconnect(),
      );
      await this.#negotiate();
      if (!this.#characteristic) throw new Error('The connection was lost while connecting.');
    } catch (error) {
      this.disconnect();
      if (error.name === 'NotFoundError') {
        throw new Error('This device has no firmware management service, so it is probably not a Cool-S badge.');
      }
      throw error;
    }
    this.connected = true;
  }

  async #open(gatt) {
    const server = await gatt.connect();
    const service = await server.getPrimaryService(SERVICE);
    const characteristic = await service.getCharacteristic(CHARACTERISTIC);
    // the same object can come back on reconnect, so do not stack listeners
    characteristic.removeEventListener('characteristicvaluechanged', this.#onNotification);
    characteristic.addEventListener('characteristicvaluechanged', this.#onNotification);
    await characteristic.startNotifications();
    this.#characteristic = characteristic;
  }

  // Web Bluetooth does not expose the MTU, and some systems silently cut writes that
  // are too long. The badge splits long responses at MTU - 3, so ask it for a long
  // echo and look at the size of the first piece.
  async #negotiate() {
    this.bufferSize = 256;
    this.fragmentSize = MIN_FRAGMENT;
    try {
      const params = await this.request(READ, 0, 6);
      if (params.buf_size) this.bufferSize = params.buf_size;
    } catch (error) {
      if (!(error instanceof SmpError)) throw error;
    }

    if (this.bufferSize < 700) return; // no room for the probe
    await this.request(WRITE, 0, 0, { d: '-'.repeat(600) });
    this.fragmentSize = Math.min(MAX_FRAGMENT, Math.max(MIN_FRAGMENT, this.#firstNotification));
  }

  disconnect() {
    if (this.device?.gatt.connected) this.device.gatt.disconnect();
    this.#onDisconnect();
  }

  #onDisconnect = () => {
    const wasConnected = this.connected;
    this.connected = false;
    this.#characteristic = null;
    this.#pending?.reject(new Error('The connection to the badge was lost.'));
    if (wasConnected) this.dispatchEvent(new Event('disconnect'));
  };

  // One request at a time: the badge has a single reassembly buffer.
  request(op, group, id, payload = {}, timeout = REQUEST_TIMEOUT) {
    const result = this.#queue.then(() => this.#exchange(op, group, id, payload, timeout));
    this.#queue = result.catch(() => {});
    return result;
  }

  async #exchange(op, group, id, payload, timeout) {
    const characteristic = this.#characteristic;
    if (!characteristic) throw new Error('The badge is not connected.');

    const body = encode(payload);
    const packet = new Uint8Array(HEADER_SIZE + body.length);
    if (packet.length > this.bufferSize) throw new Error('The request is too large for the badge.');

    const sequence = (this.#sequence = (this.#sequence + 1) & 0xff);
    const view = new DataView(packet.buffer);
    view.setUint8(0, VERSION_2 | op);
    view.setUint16(2, body.length);
    view.setUint16(4, group);
    view.setUint8(6, sequence);
    view.setUint8(7, id);
    packet.set(body, HEADER_SIZE);

    this.#received = new Uint8Array(0);
    this.#firstNotification = 0;
    const response = new Promise((resolve, reject) => {
      this.#pending = { sequence, group, id, resolve, reject };
    });

    const send = async () => {
      try {
        for (let at = 0; at < packet.length; at += this.fragmentSize) {
          await characteristic.writeValueWithoutResponse(packet.slice(at, at + this.fragmentSize));
        }
      } catch (error) {
        this.disconnect();
        throw error;
      }
    };

    try {
      // A lost fragment leaves the badge waiting for the rest of the packet, and it
      // only forgets that on disconnect. So a failure here ends the connection.
      const [, result] = await withTimeout(
        Promise.all([send(), response]), timeout, 'The badge stopped responding. Connect again to retry.',
        () => this.disconnect(),
      );
      if (result.err) throw new SmpError(result.err.rc, result.err.group);
      if (result.rc) throw new SmpError(result.rc);
      return result;
    } finally {
      this.#pending = null;
    }
  }

  #onNotification = ({ target }) => {
    if (!this.#pending) return;
    const { buffer, byteOffset, byteLength } = target.value;
    const chunk = new Uint8Array(buffer, byteOffset, byteLength);
    this.#firstNotification ||= byteLength;

    const data = new Uint8Array(this.#received.length + byteLength);
    data.set(this.#received);
    data.set(chunk, this.#received.length);
    this.#received = data;
    if (data.length < HEADER_SIZE) return;

    const view = new DataView(data.buffer);
    const end = HEADER_SIZE + view.getUint16(2);
    if (data.length < end) return;
    this.#received = new Uint8Array(0);

    const { sequence, group, id, resolve, reject } = this.#pending;
    if (view.getUint16(4) !== group || data[6] !== sequence || data[7] !== id) return;
    try {
      resolve(decode(data.subarray(HEADER_SIZE, end)));
    } catch (error) {
      reject(error);
    }
  };
}
