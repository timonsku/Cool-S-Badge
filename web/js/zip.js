// Just enough ZIP for media backups: writes stored entries, reads stored and deflated ones,
// so a backup that was unpacked and zipped again with the operating system's tools still works.

const LOCAL = 0x04034b50;
const CENTRAL = 0x02014b50;
const END = 0x06054b50;
const UTF8_NAMES = 0x0800;
const ENCRYPTED = 0x0001;
const STORED = 0;
const DEFLATED = 8;

const CRC_TABLE = Array.from({ length: 256 }, (_, n) => {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});

function crc32(data) {
  let crc = 0xffffffff;
  for (const byte of data) crc = CRC_TABLE[(crc ^ byte) & 0xff] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}

function dosDateTime(date) {
  const time = (date.getHours() << 11) | (date.getMinutes() << 5) | (date.getSeconds() >> 1);
  const day = ((Math.max(date.getFullYear(), 1980) - 1980) << 9) | ((date.getMonth() + 1) << 5) | date.getDate();
  return { time, day };
}

// entries: [{ path, data }], folders are implied by the paths
export function createZip(entries, date = new Date()) {
  const encoder = new TextEncoder();
  const { time, day } = dosDateTime(date);
  const parts = [];
  const central = [];
  let offset = 0;

  for (const { path, data } of entries) {
    const name = encoder.encode(path);
    const crc = crc32(data);

    const local = new DataView(new ArrayBuffer(30));
    local.setUint32(0, LOCAL, true);
    local.setUint16(4, 20, true); // version needed
    local.setUint16(6, UTF8_NAMES, true);
    local.setUint16(8, STORED, true);
    local.setUint16(10, time, true);
    local.setUint16(12, day, true);
    local.setUint32(14, crc, true);
    local.setUint32(18, data.length, true);
    local.setUint32(22, data.length, true);
    local.setUint16(26, name.length, true);
    parts.push(local, name, data);

    const header = new DataView(new ArrayBuffer(46));
    header.setUint32(0, CENTRAL, true);
    header.setUint16(4, 20, true); // made by
    header.setUint16(6, 20, true);
    header.setUint16(8, UTF8_NAMES, true);
    header.setUint16(10, STORED, true);
    header.setUint16(12, time, true);
    header.setUint16(14, day, true);
    header.setUint32(16, crc, true);
    header.setUint32(20, data.length, true);
    header.setUint32(24, data.length, true);
    header.setUint16(28, name.length, true);
    header.setUint32(42, offset, true);
    central.push(header, name);

    offset += 30 + name.length + data.length;
  }

  const centralSize = central.reduce((sum, part) => sum + part.byteLength, 0);
  const end = new DataView(new ArrayBuffer(22));
  end.setUint32(0, END, true);
  end.setUint16(8, entries.length, true);
  end.setUint16(10, entries.length, true);
  end.setUint32(12, centralSize, true);
  end.setUint32(16, offset, true);

  return new Blob([...parts, ...central, end], { type: 'application/zip' });
}

async function inflate(data) {
  const stream = new Blob([data]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

// Returns [{ path, data }] for the files, leaving out folders. Entries larger than maxSize throw.
export async function readZip(buffer, maxSize) {
  const bytes = new Uint8Array(buffer);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const invalid = () => new Error('This is not a valid ZIP file.');

  // the end record sits at the very end, behind a comment of up to 64 KB
  let end = -1;
  for (let i = bytes.length - 22; i >= Math.max(0, bytes.length - 22 - 0xffff); i--) {
    if (view.getUint32(i, true) === END) {
      end = i;
      break;
    }
  }
  if (end < 0) throw invalid();

  const count = view.getUint16(end + 10, true);
  let pos = view.getUint32(end + 16, true);
  if (count === 0xffff || pos === 0xffffffff) throw new Error('ZIP64 files are not supported.');

  const decoder = new TextDecoder();
  const found = [];
  for (let n = 0; n < count; n++) {
    if (pos + 46 > bytes.length || view.getUint32(pos, true) !== CENTRAL) throw invalid();
    const flags = view.getUint16(pos + 8, true);
    const method = view.getUint16(pos + 10, true);
    const crc = view.getUint32(pos + 16, true);
    const packed = view.getUint32(pos + 20, true);
    const size = view.getUint32(pos + 24, true);
    const nameLength = view.getUint16(pos + 28, true);
    const extraLength = view.getUint16(pos + 30, true);
    const commentLength = view.getUint16(pos + 32, true);
    const localOffset = view.getUint32(pos + 42, true);
    // old tools without the UTF-8 flag mostly still wrote ASCII names
    const path = decoder.decode(bytes.subarray(pos + 46, pos + 46 + nameLength));
    pos += 46 + nameLength + extraLength + commentLength;

    if (path.endsWith('/')) continue;
    if (flags & ENCRYPTED) throw new Error(`${path} is encrypted.`);
    if (method !== STORED && method !== DEFLATED) throw new Error(`${path} uses an unsupported compression method.`);
    if (size > maxSize) throw new Error(`${path} is too large for the badge.`);

    if (localOffset + 30 > bytes.length || view.getUint32(localOffset, true) !== LOCAL) throw invalid();
    const start = localOffset + 30 + view.getUint16(localOffset + 26, true) + view.getUint16(localOffset + 28, true);
    if (start + packed > bytes.length) throw invalid();
    const raw = bytes.subarray(start, start + packed);

    const data = method === STORED ? raw.slice() : await inflate(raw);
    if (data.length !== size || crc32(data) !== crc) throw new Error(`${path} is damaged.`);
    found.push({ path, data });
  }
  return found;
}
