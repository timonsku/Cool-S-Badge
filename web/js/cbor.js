// The subset of CBOR (RFC 8949) that MCUmgr uses.

const encoder = new TextEncoder();
const decoder = new TextDecoder();

export function encode(value) {
  const out = [];

  const head = (major, n) => {
    const type = major << 5;
    if (n < 24) out.push(type | n);
    else if (n < 0x100) out.push(type | 24, n);
    else if (n < 0x10000) out.push(type | 25, n >> 8, n & 0xff);
    else out.push(type | 26, n >>> 24, (n >>> 16) & 0xff, (n >>> 8) & 0xff, n & 0xff);
  };

  const write = (v) => {
    if (typeof v === 'boolean') {
      out.push(v ? 0xf5 : 0xf4);
    } else if (Number.isInteger(v) && v >= 0 && v <= 0xffffffff) {
      head(0, v);
    } else if (typeof v === 'string') {
      const bytes = encoder.encode(v);
      head(3, bytes.length);
      for (const b of bytes) out.push(b);
    } else if (v instanceof Uint8Array) {
      head(2, v.length);
      for (const b of v) out.push(b);
    } else if (v?.constructor === Object) {
      const entries = Object.entries(v);
      head(5, entries.length);
      for (const [key, item] of entries) {
        write(key);
        write(item);
      }
    } else {
      throw new TypeError(`Cannot encode ${v} as CBOR`);
    }
  };

  write(value);
  return Uint8Array.from(out);
}

const BREAK = Symbol('break');
const MAX_DEPTH = 16;

export function decode(bytes) {
  let pos = 0;

  const take = (n) => {
    if (pos + n > bytes.length) throw new Error('The badge sent a truncated response.');
    pos += n;
    return bytes.subarray(pos - n, pos);
  };

  const read = (depth) => {
    if (depth > MAX_DEPTH) throw new Error('The badge sent a malformed response.');
    const first = take(1)[0];
    if (first === 0xff) return BREAK;

    const major = first >> 5;
    const info = first & 0x1f;

    if (major === 7) {
      if (info === 20) return false;
      if (info === 21) return true;
      if (info === 22 || info === 23) return null;
      throw new Error('The badge sent an unsupported value.');
    }

    let n = info;
    if (info >= 24 && info <= 27) {
      n = 0;
      for (const b of take(1 << (info - 24))) n = n * 256 + b;
    } else if (info === 31 && (major === 4 || major === 5)) {
      // zcbor writes maps and lists with an indefinite length
      n = Infinity;
    } else if (info > 27) {
      throw new Error('The badge sent a malformed response.');
    }

    switch (major) {
      case 0: return n;
      case 1: return -1 - n;
      case 2: return take(n).slice();
      case 3: return decoder.decode(take(n));
      case 4: {
        const list = [];
        while (list.length < n) {
          const item = read(depth + 1);
          if (item === BREAK) break;
          list.push(item);
        }
        return list;
      }
      case 5: {
        const map = Object.create(null);
        for (let i = 0; i < n; i++) {
          const key = read(depth + 1);
          if (key === BREAK) break;
          map[key] = read(depth + 1);
        }
        return map;
      }
      default:
        throw new Error('The badge sent an unsupported value.');
    }
  };

  return bytes.length ? read(0) : {};
}
