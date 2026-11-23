// Encoders for one-row images, the shape the badge firmware reads.
// Pixels are 0xRRGGBB numbers.

const BLACK = 0;

class ByteWriter {
  bytes = [];

  u8(...values) {
    for (const value of values) this.bytes.push(value & 0xff);
  }

  u16le(value) {
    this.u8(value, value >> 8);
  }

  u32be(value) {
    this.u8(value >>> 24, value >>> 16, value >>> 8, value);
  }

  ascii(text) {
    for (const char of text) this.u8(char.charCodeAt(0));
  }

  rgb(color) {
    this.u8(color >> 16, color >> 8, color);
  }

  append(data) {
    for (const value of data) this.bytes.push(value);
  }

  result() {
    return Uint8Array.from(this.bytes);
  }
}

// PNG

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

async function zlib(data) {
  const stream = new Blob([data]).stream().pipeThrough(new CompressionStream('deflate'));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

function chunk(out, type, data) {
  const body = new ByteWriter();
  body.ascii(type);
  body.append(data);
  const bytes = body.result();
  out.u32be(data.length);
  out.append(bytes);
  out.u32be(crc32(bytes));
}

// 8-bit RGB, one row
export async function encodePng(pixels) {
  const header = new ByteWriter();
  header.u32be(pixels.length);
  header.u32be(1);
  header.u8(8, 2, 0, 0, 0); // bit depth, truecolour, deflate, adaptive filtering, no interlace

  const row = new ByteWriter();
  row.u8(0); // no filter
  for (const color of pixels) row.rgb(color);

  const out = new ByteWriter();
  out.u8(0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a);
  chunk(out, 'IHDR', header.result());
  chunk(out, 'IDAT', await zlib(row.result()));
  chunk(out, 'IEND', []);
  return out.result();
}

// GIF

function lzw(indices, minCodeSize) {
  const clear = 1 << minCodeSize;
  const end = clear + 1;
  const out = [];
  let codeSize = minCodeSize + 1;
  let next = end + 1;
  let dictionary = new Map();
  let buffer = 0;
  let bits = 0;

  const emit = (code) => {
    buffer |= code << bits;
    bits += codeSize;
    while (bits >= 8) {
      out.push(buffer & 0xff);
      buffer >>>= 8;
      bits -= 8;
    }
  };

  emit(clear);
  let prefix = indices[0];
  for (let i = 1; i < indices.length; i++) {
    const key = (prefix << 8) | indices[i];
    const code = dictionary.get(key);
    if (code !== undefined) {
      prefix = code;
      continue;
    }
    emit(prefix);
    if (next === 4096) {
      emit(clear);
      dictionary = new Map();
      codeSize = minCodeSize + 1;
      next = end + 1;
    } else {
      if (next >= 1 << codeSize) codeSize++;
      dictionary.set(key, next++);
    }
    prefix = indices[i];
  }
  emit(prefix);
  emit(end);
  if (bits > 0) out.push(buffer & 0xff);
  return out;
}

function subBlocks(out, data) {
  for (let i = 0; i < data.length; i += 255) {
    const part = data.slice(i, i + 255);
    out.u8(part.length);
    out.append(part);
  }
  out.u8(0);
}

// Black comes first: the firmware blanks pixels that match the background
// colour (index 0), which must not change any other colour.
function palette(colors) {
  const list = [BLACK, ...[...new Set(colors)].filter((color) => color !== BLACK)];
  let bits = 1;
  while (1 << bits < list.length) bits++;
  return { list, bits, index: new Map(list.map((color, i) => [color, i])) };
}

function writeTable(out, { list, bits }) {
  for (let i = 0; i < 1 << bits; i++) out.rgb(list[i] ?? BLACK);
}

// frames: arrays of pixels, all the same length. delay: centiseconds per frame.
export function encodeGif(frames, delay) {
  const width = frames[0].length;
  const all = palette(frames.flat());
  // more than 256 colours: every frame brings its own table instead
  const global = all.list.length <= 256 ? all : null;

  const out = new ByteWriter();
  out.ascii('GIF89a');
  out.u16le(width);
  out.u16le(1);
  out.u8(global ? 0x80 | ((global.bits - 1) << 4) | (global.bits - 1) : 0, 0, 0);
  if (global) writeTable(out, global);

  // loop forever (the badge loops anyway, browsers need this)
  out.u8(0x21, 0xff, 11);
  out.ascii('NETSCAPE2.0');
  out.u8(3, 1, 0, 0, 0);

  for (const frame of frames) {
    out.u8(0x21, 0xf9, 4, 1 << 2); // graphic control: leave the frame in place, no transparency
    out.u16le(delay);
    out.u8(0, 0);

    const table = global ?? palette(frame);
    out.u8(0x2c);
    out.u16le(0);
    out.u16le(0);
    out.u16le(width);
    out.u16le(1);
    out.u8(global ? 0 : 0x80 | (table.bits - 1));
    if (!global) writeTable(out, table);

    const minCodeSize = Math.max(2, table.bits);
    out.u8(minCodeSize);
    subBlocks(out, lzw(frame.map((color) => table.index.get(color)), minCodeSize));
  }

  out.u8(0x3b);
  return out.result();
}
