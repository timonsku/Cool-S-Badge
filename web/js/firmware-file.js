// Reads and checks a dfu_application.zip or signed .bin before anything is sent.

// cool-s-firmware/pm_static.yml
const SLOT_START = 0x12000;
const SLOT_SIZE = 0x67000;
// swap-using-move needs a sector for the trailer and one to move through
const MAX_IMAGE_SIZE = SLOT_SIZE - 0x2000;
const BOARD = 'cool_s';
const MAX_FILE_SIZE = 4 * 1024 * 1024;

const IMAGE_MAGIC = 0x96f3b83d;
const TLV_INFO_MAGIC = 0x6907;
const TLV_SHA256 = 0x10;
const FLAGS_UNUSABLE = 0x04 | 0x08 | 0x10; // encrypted or non-bootable

export const toHex = (bytes) => Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');

const sha256 = async (bytes) => new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));

export async function readFirmware(file) {
  if (file.size > MAX_FILE_SIZE) throw new Error('This file is far too large to be badge firmware.');
  try {
    let data = new Uint8Array(await file.arrayBuffer());
    if (data[0] === 0x50 && data[1] === 0x4b) data = await imageFromPackage(data);
    return { fileName: file.name, data, sha: await sha256(data), hash: await checkImage(data) };
  } catch (error) {
    // out of range reads and inflate failures both mean a broken file
    if (error instanceof RangeError || error instanceof TypeError) {
      throw new Error('The file is damaged or incomplete.');
    }
    throw error;
  }
}

async function imageFromPackage(zip) {
  const entries = zipEntries(zip);
  if (!entries.has('manifest.json')) {
    throw new Error('This zip has no manifest.json, so it is not an update package.');
  }

  let manifest;
  try {
    manifest = JSON.parse(new TextDecoder().decode(await unzip(zip, entries.get('manifest.json'))));
  } catch {
    throw new Error('The manifest in this package cannot be read.');
  }

  const files = Array.isArray(manifest?.files) ? manifest.files : [];
  if (files.length !== 1 || Number(files[0].image_index ?? 0) !== 0) {
    throw new Error('Expected a package with exactly one application image.');
  }
  const { board, file } = files[0];
  if (board && String(board).split('/')[0] !== BOARD) {
    throw new Error(`This package is for "${board}", not for the Cool-S badge.`);
  }
  if (!entries.has(file)) throw new Error('The package is missing its firmware image.');
  return unzip(zip, entries.get(file));
}

function zipEntries(zip) {
  const view = new DataView(zip.buffer, zip.byteOffset, zip.byteLength);
  let end = zip.length - 22;
  while (end >= 0 && view.getUint32(end, true) !== 0x06054b50) end--;
  if (end < 0) throw new Error('The zip file is damaged.');

  const entries = new Map();
  let at = view.getUint32(end + 16, true);
  for (let i = view.getUint16(end + 10, true); i > 0; i--) {
    if (view.getUint32(at, true) !== 0x02014b50) throw new Error('The zip file is damaged.');
    const nameLength = view.getUint16(at + 28, true);
    entries.set(new TextDecoder().decode(zip.subarray(at + 46, at + 46 + nameLength)), {
      method: view.getUint16(at + 10, true),
      packed: view.getUint32(at + 20, true),
      size: view.getUint32(at + 24, true),
      offset: view.getUint32(at + 42, true),
    });
    at += 46 + nameLength + view.getUint16(at + 30, true) + view.getUint16(at + 32, true);
  }
  return entries;
}

async function unzip(zip, { method, packed, size, offset }) {
  if (size > MAX_FILE_SIZE) throw new Error('The zip file is damaged.');
  const view = new DataView(zip.buffer, zip.byteOffset + offset, 30);
  const start = offset + 30 + view.getUint16(26, true) + view.getUint16(28, true);
  const stored = zip.slice(start, start + packed);

  if (method === 0 && stored.length === size) return stored;
  if (method !== 8) throw new Error('The zip file is damaged or uses an unsupported compression.');

  const reader = new Blob([stored]).stream().pipeThrough(new DecompressionStream('deflate-raw')).getReader();
  const data = new Uint8Array(size);
  let at = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    data.set(value, at); // throws once the output outgrows the declared size
    at += value.length;
  }
  if (at !== size) throw new Error('The zip file is damaged.');
  return data;
}

async function checkImage(data) {
  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  if (data.length < 32 || view.getUint32(0, true) !== IMAGE_MAGIC) {
    throw new Error('This is not a signed firmware image. Use the release .zip or a .signed.bin file.');
  }
  if (data.length > MAX_IMAGE_SIZE) {
    throw new Error(`This image is ${data.length - MAX_IMAGE_SIZE} bytes too large for the update slot.`);
  }
  if (view.getUint32(16, true) & FLAGS_UNUSABLE) throw new Error('This image cannot boot on the badge.');

  const headerSize = view.getUint16(8, true);
  const hashedEnd = headerSize + view.getUint32(12, true) + view.getUint16(10, true);
  if (headerSize < 32 || headerSize + 8 > hashedEnd || hashedEnd + 4 > data.length) {
    throw new Error('The firmware image is incomplete.');
  }

  // the Cortex-M reset vector has to point into the badge's application slot
  const entry = view.getUint32(headerSize + 4, true);
  if (entry < SLOT_START + headerSize || entry >= SLOT_START + SLOT_SIZE) {
    throw new Error('This image was built for a different device or flash layout.');
  }

  const tlvEnd = hashedEnd + view.getUint16(hashedEnd + 2, true);
  if (view.getUint16(hashedEnd, true) !== TLV_INFO_MAGIC || tlvEnd > data.length) {
    throw new Error('The firmware image is incomplete.');
  }

  for (let at = hashedEnd + 4; at + 4 <= tlvEnd;) {
    const type = view.getUint16(at, true);
    const length = view.getUint16(at + 2, true);
    if (type === TLV_SHA256) {
      const hash = data.slice(at + 4, at + 4 + length);
      if (toHex(hash) !== toHex(await sha256(data.subarray(0, hashedEnd)))) {
        throw new Error('The firmware image is corrupted: its checksum does not match.');
      }
      return hash;
    }
    at += 4 + length;
  }
  throw new Error('The firmware image has no checksum.');
}
