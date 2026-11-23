// Media file checks and littlefs size accounting.

// Has to match LUA_SCRIPT_MAX_SIZE in cool-s-firmware/src/lua_anim.h
export const SCRIPT_MAX_SIZE = 12 * 1024;

export const FORMATS = {
  gif: {
    label: 'GIF',
    noun: 'image',
    mime: 'image/gif',
    extension: /\.gif$/i,
    inspect(data) {
      if (data.length < 14 || !['GIF87a', 'GIF89a'].includes(String.fromCharCode(...data.subarray(0, 6)))) {
        return null;
      }
      const view = new DataView(data.buffer, data.byteOffset);
      return { width: view.getUint16(6, true), height: view.getUint16(8, true) };
    },
  },
  png: {
    label: 'PNG',
    noun: 'image',
    mime: 'image/png',
    extension: /\.png$/i,
    inspect(data) {
      const signature = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];
      if (data.length < 33 || signature.some((b, i) => data[i] !== b)) return null;
      const view = new DataView(data.buffer, data.byteOffset);
      return { width: view.getUint32(16), height: view.getUint32(20) };
    },
  },
  lua: {
    label: 'Lua',
    noun: 'script',
    mime: 'text/plain',
    extension: /\.lua$/i,
    script: true,
    // Source text only: the badge refuses precompiled bytecode.
    inspect(data) {
      if (!data.length || data.length > SCRIPT_MAX_SIZE || data[0] === 0x1b || data.includes(0)) return null;
      try {
        const text = new TextDecoder('utf-8', { fatal: true }).decode(data);
        return { text, lines: text.split(/\r?\n/).length };
      } catch {
        return null;
      }
    },
  },
};

// The firmware allows 64 bytes for the whole path; shorter names also keep the list readable.
const MAX_NAME = 32;

export function deviceName(original, dir) {
  const suffix = `.${dir}`;
  const stem = original
    .replace(FORMATS[dir].extension, '')
    .normalize('NFKD')
    .replace(/\p{M}/gu, '')
    .replace(/[^A-Za-z0-9_-]+/g, '_')
    .replace(/^_+|_+$/g, '')
    .slice(0, MAX_NAME - suffix.length);
  return `${stem || 'image'}${suffix}`;
}

export function uniqueName(name, taken) {
  const dot = name.lastIndexOf('.');
  let result = name;
  for (let n = 2; taken.has(result); n++) {
    const suffix = `-${n}${name.slice(dot)}`;
    result = name.slice(0, Math.min(dot, MAX_NAME - suffix.length)) + suffix;
  }
  return result;
}

// Files up to CONFIG_FS_LITTLEFS_CACHE_SIZE live inside the directory's metadata.
// Larger ones are a list of whole blocks, where block n also holds ctz(n) + 1 pointers.
const INLINE_MAX = 64;

export function blocksFor(size, blockSize) {
  if (size <= INLINE_MAX) return 0;
  let blocks = 1;
  let capacity = blockSize;
  while (capacity < size) {
    const trailingZeros = 31 - Math.clz32(blocks & -blocks);
    capacity += blockSize - 4 * (trailingZeros + 1);
    blocks++;
  }
  return blocks;
}

export function formatBytes(bytes) {
  if (bytes < 1024) return `${bytes} B`;
  return `${Number((bytes / 1024).toFixed(1))} KB`;
}
