// The commands this UI needs, on top of SmpClient.

import { encode } from './cbor.js';
import { READ, WRITE, RC, SmpError, IPIXEL_SERVICE } from './smp.js';

const GROUP = { OS: 0, IMAGE: 1, STAT: 2, FS: 8, MEDIA: 64 };
const OS = { RESET: 5 };
const IMAGE = { STATE: 0, UPLOAD: 1 };
const FS = { FILE: 0, CLOSE: 4 };
const MEDIA = { INFO: 0, LIST: 1, PREPARE: 2, COMMIT: 3, DELETE: 4, PREVIEW: 5, WALK: 6, PUT: 7, RUN: 8, SCRIPT: 9, FORMAT: 10 }; // cool-s-firmware/src/media_mgmt.c

const MOUNT = '/lfs1';

const FS_ERR_NOT_FOUND = 3;
const isNotFound = (error) => error instanceof SmpError &&
  (error.group === null ? error.rc === RC.NOT_FOUND : error.group === GROUP.FS && error.rc === FS_ERR_NOT_FOUND);
const UPLOAD_PATH = `${MOUNT}/.upload`;

// the first firmware chunk waits for the whole slot to be erased
const ERASE_TIMEOUT = 120000;
const FLASH_TIMEOUT = 20000;
// activating an image that was not just uploaded waits up to 30 seconds for button presses
const ACTIVATE_TIMEOUT = 45000;
// what boot_set_next's refusal in update_approval.c turns into
const IMAGE_ERR_UNKNOWN = 1;
const MAX_DOWNLOAD = 1024 * 1024;
const IPIXEL_WRITE = 0xfa02;
// cool-s-firmware/src/preview.h
export const PREVIEW_MAX = 16 * 1024;

export class Badge {
  #pixelWriter = null;

  constructor(smp) {
    this.smp = smp;
    smp.addEventListener('disconnect', () => { this.#pixelWriter = null; });
  }

  // The iPixel app's live drawing: shows one pixel without saving anything.
  // Packet: length, 0, command 5, type 1, 0, R, G, B, x, y. The badge takes x as the LED index.
  // Its "stop drawing" packet would save the LEDs as default.png, so it is never sent.
  async showPixel(index, color) {
    if (!this.#pixelWriter) {
      const service = await this.smp.device.gatt.getPrimaryService(IPIXEL_SERVICE);
      this.#pixelWriter = await service.getCharacteristic(IPIXEL_WRITE);
    }
    const packet = Uint8Array.of(10, 0, 5, 1, 0, color >> 16, color >> 8, color, index, 0);
    // with a response, so nothing is dropped and writes cannot pile up
    await this.#pixelWriter.writeValueWithResponse(packet);
  }

  async firmwareInfo() {
    let fields;
    try {
      ({ fields } = await this.smp.request(READ, GROUP.STAT, 0, { name: 'smp_svr_stats' }));
    } catch (error) {
      if (!(error instanceof SmpError)) throw error;
    }
    return {
      version: fields?.version_major === undefined ? null : `${fields.version_major}.${fields.version_minor}`,
      // newer firmware wants button presses before it accepts an image
      needsPresses: fields?.upload_approval === 1,
    };
  }

  // null means the firmware predates media management
  async storage() {
    try {
      return await this.smp.request(READ, GROUP.MEDIA, MEDIA.INFO);
    } catch (error) {
      if (error instanceof SmpError && error.group === null && error.rc === RC.NOT_SUPPORTED) return null;
      throw error;
    }
  }

  // null when the firmware does not know the folder, as firmware before Lua scripts and "lua"
  async list(dir) {
    const found = [];
    let skip = 0;
    for (;;) {
      let response;
      try {
        response = await this.smp.request(READ, GROUP.MEDIA, MEDIA.LIST, { dir, skip });
      } catch (error) {
        if (skip === 0 && error instanceof SmpError && error.group === null && error.rc === RC.INVALID) return null;
        throw error;
      }
      const { files, next } = response;
      for (let i = 0; i < files.length; i += 2) found.push({ name: files[i], size: files[i + 1] });
      if (!(next > skip)) break;
      skip = next;
    }
    return found.sort((a, b) => a.name.localeCompare(b.name, undefined, { numeric: true }));
  }

  // The stock group keeps the last file open, and mixes up offsets if a new
  // transfer to the same path starts before that is closed.
  closeFile() {
    return this.smp.request(WRITE, GROUP.FS, FS.CLOSE, {}, FLASH_TIMEOUT);
  }

  async prepareUpload(dir) {
    await this.closeFile();
    await this.smp.request(WRITE, GROUP.MEDIA, MEDIA.PREPARE, { dir }, FLASH_TIMEOUT);
  }

  // Stock file upload, straight to path. Only the media folders go through
  // prepareUpload() and upload(), which never leave a partial file behind.
  async #writeFile(path, data, onProgress) {
    let off = 0;
    while (off < data.length) {
      const request = { name: path, off, ...(off === 0 && { len: data.length }) };
      const end = off + this.#room(request, data.length - off);
      const response = await this.smp.request(
        WRITE, GROUP.FS, FS.FILE, { ...request, data: data.subarray(off, end) }, FLASH_TIMEOUT,
      );
      if (response.off !== end) throw new Error('The badge lost track of the upload. Please try again.');
      off = end;
      onProgress(off / data.length);
    }
    await this.closeFile();
  }

  // call prepareUpload() first
  async upload(dir, name, data, onProgress) {
    try {
      await this.#writeFile(UPLOAD_PATH, data, onProgress);
      await this.smp.request(WRITE, GROUP.MEDIA, MEDIA.COMMIT, { dir, name, size: data.length }, FLASH_TIMEOUT);
    } catch (error) {
      // give the space back; if the link is gone, the next upload does this instead
      if (this.smp.connected) await this.prepareUpload(dir).catch(() => {});
      if (error instanceof SmpError && error.group === null && [RC.BAD_STATE, RC.NO_MEMORY].includes(error.rc)) {
        throw new Error('The badge could not store the whole file. The storage is probably full.');
      }
      throw error;
    }
  }

  async download(dir, name, signal) {
    const data = await this.readFile(`${dir}/${name}`, signal);
    if (!data) throw new Error('File not found.');
    return data;
  }

  // Every file and folder below path ('' is the root), hidden ones left out.
  // null means the firmware cannot list outside the media folders.
  async walk(path = '') {
    const found = [];
    let skip = 0;
    for (;;) {
      let response;
      try {
        response = await this.smp.request(READ, GROUP.MEDIA, MEDIA.WALK, { path: `${MOUNT}${path && `/${path}`}`, skip });
      } catch (error) {
        if (skip === 0 && error instanceof SmpError && error.group === null && error.rc === RC.NOT_SUPPORTED) return null;
        throw error;
      }
      const { entries, next } = response;
      for (let i = 0; i < entries.length; i += 3) {
        const [name, isDir, size] = entries.slice(i, i + 3);
        const child = path ? `${path}/${name}` : name;
        if (isDir) found.push(...(await this.walk(child)));
        else found.push({ path: child, size });
      }
      if (!(next > skip)) break;
      skip = next;
    }
    return found;
  }

  // Any file, path relative to the root. The badge only moves it into place once
  // it is complete, making the folders on the way. Needs firmware with walk().
  async put(path, data, onProgress) {
    await this.closeFile();
    try {
      await this.#writeFile(UPLOAD_PATH, data, onProgress);
      await this.smp.request(WRITE, GROUP.MEDIA, MEDIA.PUT, { path: `${MOUNT}/${path}`, size: data.length }, FLASH_TIMEOUT);
    } catch (error) {
      if (this.smp.connected) await this.closeFile().catch(() => {});
      if (error instanceof SmpError && error.group === null && [RC.BAD_STATE, RC.NO_MEMORY].includes(error.rc)) {
        throw new Error('The badge could not store the whole file. The storage is probably full.');
      }
      throw error;
    }
  }

  // For firmware without put(): a file in the root, written in place. Not atomic,
  // a dropped link leaves it cut short until it is written again.
  async putInRoot(name, data, onProgress) {
    await this.closeFile();
    try {
      await this.#writeFile(`${MOUNT}/${name}`, data, onProgress);
    } catch (error) {
      if (this.smp.connected) await this.closeFile().catch(() => {});
      throw error;
    }
  }

  // path is relative to the file system root; resolves to null if there is no such file
  async readFile(path, signal) {
    await this.closeFile();
    try {
      let data = null;
      let off = 0;
      do {
        signal?.throwIfAborted();
        let part;
        try {
          part = await this.smp.request(READ, GROUP.FS, FS.FILE, { name: `${MOUNT}/${path}`, off });
        } catch (error) {
          if (off === 0 && isNotFound(error)) return null;
          throw error;
        }
        if (data === null) {
          if (!(part.len <= MAX_DOWNLOAD)) throw new Error('The badge reported an invalid file size.');
          data = new Uint8Array(part.len);
        }
        if (off < data.length && !part.data?.length) throw new Error('The file ended early.');
        data.set(part.data, off);
        off += part.data.length;
      } while (off < data.length);
      return data;
    } finally {
      if (this.smp.connected) await this.closeFile().catch(() => {});
    }
  }

  // Plays a GIF on the badge from its RAM; nothing is written to flash.
  async previewGif(data) {
    try {
      let off = 0;
      while (off < data.length) {
        const request = { off, ...(off === 0 && { len: data.length }) };
        const end = off + this.#room(request, data.length - off);
        const response = await this.smp.request(
          WRITE, GROUP.MEDIA, MEDIA.PREVIEW, { ...request, data: data.subarray(off, end) },
        );
        if (response.off !== end) throw new Error('The badge lost track of the preview.');
        off = end;
      }
    } catch (error) {
      if (error instanceof SmpError && error.group === null && error.rc === RC.NOT_SUPPORTED) {
        throw new Error('The firmware on this badge cannot play animation previews. Update it to see them on the badge.');
      }
      throw error;
    }
  }

  // stops the preview; its frame stays up until the next drawing or button press
  endPreview() {
    return this.smp.request(WRITE, GROUP.MEDIA, MEDIA.PREVIEW, { len: 0 });
  }

  // Shows the script lua/<name> on the badge. False if the firmware cannot.
  async runScript(name) {
    try {
      await this.smp.request(WRITE, GROUP.MEDIA, MEDIA.RUN, { dir: 'lua', name });
      return true;
    } catch (error) {
      if (error instanceof SmpError && error.group === null && error.rc === RC.NOT_SUPPORTED) return false;
      throw error;
    }
  }

  // { running, path, error } of the last script the badge started, null if the firmware cannot tell.
  async scriptStatus() {
    try {
      return await this.smp.request(READ, GROUP.MEDIA, MEDIA.SCRIPT, {});
    } catch (error) {
      if (error instanceof SmpError && error.group === null && error.rc === RC.NOT_SUPPORTED) return null;
      throw error;
    }
  }

  async remove(dir, name) {
    await this.closeFile();
    await this.smp.request(WRITE, GROUP.MEDIA, MEDIA.DELETE, { dir, name }, FLASH_TIMEOUT);
  }

  async images() {
    const { images } = await this.smp.request(READ, GROUP.IMAGE, IMAGE.STATE);
    return images ?? [];
  }

  async uploadImage(data, sha, onProgress) {
    let off = 0;
    let stalls = 0;
    while (off < data.length) {
      const request = { off, ...(off === 0 && { image: 0, len: data.length, sha }) };
      let size = this.#room(request, data.length - off);
      if (off + size < data.length) size -= size % 4; // flash word aligned

      const response = await this.smp.request(
        WRITE, GROUP.IMAGE, IMAGE.UPLOAD, { ...request, data: data.subarray(off, off + size) },
        off === 0 ? ERASE_TIMEOUT : FLASH_TIMEOUT,
      );
      // the badge names the next offset, which is how an interrupted upload resumes
      const next = response.off;
      if (!Number.isInteger(next) || next > data.length) {
        throw new Error('The badge sent an invalid upload offset.');
      }
      stalls = next > off ? 0 : stalls + 1;
      if (stalls > 3) throw new Error('The firmware upload is not making progress.');
      off = next;
      onProgress(off / data.length);
    }
  }

  // Boots once. Unless the new image then confirms itself, MCUboot goes back to the previous one.
  testImage(hash) {
    return this.smp.request(WRITE, GROUP.IMAGE, IMAGE.STATE, { hash, confirm: false }, ACTIVATE_TIMEOUT);
  }

  isNotApproved(error) {
    if (!(error instanceof SmpError)) return false;
    if (error.group === null) return error.rc === RC.ACCESS_DENIED;
    return error.group === GROUP.IMAGE && error.rc === IMAGE_ERR_UNKNOWN;
  }

  reset() {
    return this.smp.request(WRITE, GROUP.OS, OS.RESET);
  }

  // The badge restarts right after answering and erases its whole storage while starting.
  async formatStorage() {
    await this.closeFile();
    try {
      await this.smp.request(WRITE, GROUP.MEDIA, MEDIA.FORMAT);
    } catch (error) {
      if (error instanceof SmpError && error.group === null && error.rc === RC.NOT_SUPPORTED) {
        throw new Error('The firmware on this badge cannot erase its storage. Update it to v0.7 or newer.');
      }
      if (error instanceof SmpError && error.group === null && error.rc === RC.BUSY) {
        throw new Error('The badge is still testing its new firmware. Try again in a minute.');
      }
      throw error;
    }
  }

  // bytes of data that still fit in one SMP packet next to the other fields
  #room(request, remaining) {
    const overhead = 8 + encode({ ...request, data: new Uint8Array(0) }).length + 2;
    return Math.min(remaining, this.smp.bufferSize - overhead);
  }
}
