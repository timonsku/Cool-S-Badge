import { app } from './app.js';
import { $, el, notify, confirmDialog, setupTabs, touchScreen } from './dom.js';
import { FORMATS, blocksFor, deviceName, uniqueName, formatBytes } from './formats.js';
import { createZip, readZip } from './zip.js';
import { editScript } from './code.js';

// Left free for littlefs copy-on-write and the settings the badge saves itself.
const RESERVED_BLOCKS = 2;
// A tiny file can declare a huge canvas and hang the browser.
const MAX_PREVIEW_SIDE = 2048;
// The files the badge shows when nothing in a folder is selected, in the littlefs root.
const DEFAULTS = Object.keys(FORMATS).map((folder) => `default.${folder}`);
const SETTINGS = 'settings.json';

let dir = 'gif';
let storage; // undefined: not loaded, null: firmware without media management
// by folder; null for a folder the firmware does not have, like lua before Lua scripts
const noFiles = () => Object.fromEntries(Object.keys(FORMATS).map((folder) => [folder, []]));
let files = noFiles();
const filesIn = (folder) => files[folder] ?? [];
let previewsOn = false;
const previews = new Map();
const cards = new Map();

const keyOf = (folder, name) => `${folder}/${name}`;

function forgetPreview(key) {
  const preview = previews.get(key);
  if (preview?.url) URL.revokeObjectURL(preview.url);
  previews.delete(key);
}

function clearPreviews() {
  for (const key of [...previews.keys()]) forgetPreview(key);
}

export function resetMedia() {
  clearPreviews();
  storage = undefined;
  files = noFiles();
  render();
}

export async function refreshMedia() {
  const info = await app.badge.storage();
  const found = noFiles();
  if (info) {
    for (const folder of Object.keys(found)) found[folder] = await app.badge.list(folder);
  }
  storage = info;
  files = found;
  render();
}

function renderStorage() {
  const meter = $('#storage-meter');
  if (!storage) {
    meter.replaceChildren();
    meter.removeAttribute('aria-valuenow');
    meter.removeAttribute('aria-valuetext');
    $('#storage-free').textContent = '–';
    $('#storage-detail').textContent =
      storage === null ? 'Needs newer firmware' : 'Connect a badge to see its storage';
    return;
  }

  const { block, blocks, free } = storage;
  const used = blocks - free;
  meter.replaceChildren(...Array.from({ length: Math.min(blocks, 256) }, (_, i) => {
    let kind = 'free';
    if (i < used) kind = 'used';
    else if (i >= blocks - RESERVED_BLOCKS) kind = 'reserved';
    return el('span', { className: kind });
  }));

  const summary = `${formatBytes(free * block)} free of ${formatBytes(blocks * block)}`;
  meter.setAttribute('aria-valuenow', String(Math.round((used / blocks) * 100)));
  meter.setAttribute('aria-valuetext', summary);
  $('#storage-free').textContent = summary;
  $('#storage-detail').textContent =
    `${formatBytes(Math.max(0, free - RESERVED_BLOCKS) * block)} available for uploads. ` +
    `${formatBytes(RESERVED_BLOCKS * block)} is kept free for the badge, ` +
    `and files take up whole ${formatBytes(block)} blocks.`;
}

function updatePreview(key) {
  const card = cards.get(key);
  if (!card) return;
  const preview = previews.get(key);
  card.dimensions.textContent = preview?.width ? `${preview.width} × ${preview.height}`
    : preview?.lines ? `${preview.lines} ${preview.lines === 1 ? 'line' : 'lines'}` : '';

  if (preview?.text !== undefined) {
    // the start of a script, so it can be told apart from the others
    card.thumb.replaceChildren(el('pre', { className: 'code' }, preview.text.split(/\r?\n/).slice(0, 12).join('\n')));
  } else if (preview?.url) {
    const image = el('img', { src: preview.url, alt: '' });
    image.addEventListener('error', () => image.replaceWith(el('span', {}, 'Cannot display')));
    card.thumb.replaceChildren(image);
  } else {
    card.thumb.replaceChildren(el('span', {}, preview?.error ?? preview?.status ?? 'Waiting…'));
  }
}

function renderFiles() {
  cards.clear();
  const folder = dir;
  const items = filesIn(folder).map((file) => {
    const remove = el('button', {
      type: 'button', className: 'delete', 'aria-label': `Delete ${file.name}`, textContent: 'Delete',
    });
    remove.addEventListener('click', () => deleteFile(folder, file.name));
    // scripts open in the Code tab
    const open = FORMATS[folder].script
      ? el('button', { type: 'button', 'aria-label': `Edit ${file.name}`, textContent: 'Edit' })
      : null;
    open?.addEventListener('click', () => editScript(file.name));

    const thumb = el('div', { className: 'thumb', hidden: !previewsOn });
    const dimensions = el('span', { className: 'dimensions' });
    const key = keyOf(folder, file.name);
    cards.set(key, { thumb, dimensions, remove, open });
    if (previewsOn) updatePreview(key);

    const onFlash = blocksFor(file.size, storage.block) * storage.block;
    return el('li', { className: 'file' },
      thumb,
      el('div', { className: 'file-info' },
        el('strong', { title: file.name }, file.name),
        el('span', { title: `Takes ${formatBytes(onFlash)} of flash` }, formatBytes(file.size)),
        dimensions),
      ...(open ? [el('div', { className: 'file-actions' }, open, remove)] : [remove]));
  });

  $('#files').replaceChildren(...items);
  $('#files').classList.toggle('with-previews', previewsOn);
}

function render() {
  renderStorage();
  renderFiles();
  for (const [folder, { label }] of Object.entries(FORMATS)) {
    $(`#tab-${folder}`).textContent = storage && files[folder] ? `${label} (${files[folder].length})` : label;
  }
  $('#folder-path').textContent = `/${dir}`;
  $('#file-input').accept = `.${dir},${FORMATS[dir].mime}`;
  $('#drop-text').textContent = touchScreen
    ? `Tap to choose ${FORMATS[dir].label} files`
    : `Drop ${FORMATS[dir].label} files here, or click to choose`;
  $('#lua-help').hidden = dir !== 'lua';
  update();
}

// cheap enough to run on every state change
function update() {
  const usable = app.ready && Boolean(storage);
  const locked = !usable || app.busy;
  // the backup buttons work for every folder the firmware has
  const folderLocked = locked || files[dir] === null;

  $('#media-unsupported').hidden = !(app.ready && storage === null);
  $('#file-input').disabled = folderLocked;
  $('#backup-download').disabled = locked;
  $('#backup-restore').disabled = locked;
  $('#storage-erase').disabled = locked;
  $('#dropzone').classList.toggle('disabled', folderLocked);
  $('#refresh').disabled = !app.ready || app.busy;
  for (const { remove, open } of cards.values()) {
    remove.disabled = locked;
    if (open) open.disabled = locked;
  }

  let emptyText = 'Not connected.';
  if (usable && files[dir] === null) {
    emptyText = `The firmware on this badge cannot run ${FORMATS[dir].label} ${FORMATS[dir].noun}s. Update it first.`;
  } else if (usable) emptyText = `No ${FORMATS[dir].label} files on the badge yet.`;
  else if (app.ready && storage === null) emptyText = 'Media management is unavailable.';
  else if (app.ready) emptyText = 'The storage could not be read. Press Refresh to retry.';
  $('#files-empty').textContent = emptyText;
  $('#files-empty').hidden = filesIn(dir).length > 0;

  if (usable && previewsOn && !$('#panel-media').hidden) app.background(loadPreviews);
}

async function loadPreviews(signal) {
  while (!signal.aborted && app.ready && previewsOn && !$('#panel-media').hidden) {
    // looked up every round, so switching tabs changes what loads next
    const folder = dir;
    const { label, noun, mime, inspect, script } = FORMATS[folder];
    const file = filesIn(folder).find(({ name }) => !previews.has(keyOf(folder, name)));
    if (!file) return;

    const key = keyOf(folder, file.name);
    previews.set(key, { status: 'Loading…' });
    updatePreview(key);
    try {
      const data = await app.badge.download(folder, file.name, signal);
      const info = inspect(data);
      if (!info) {
        previews.set(key, { error: `Not a valid ${label} ${noun}` });
      } else if (script) {
        previews.set(key, info);
      } else if (Math.max(info.width, info.height) > MAX_PREVIEW_SIDE) {
        previews.set(key, { ...info, error: 'Too large to preview' });
      } else {
        previews.set(key, { ...info, url: URL.createObjectURL(new Blob([data], { type: mime })) });
      }
    } catch (error) {
      if (signal.aborted || !app.smp.connected) {
        previews.delete(key); // retry later
        updatePreview(key);
        return;
      }
      previews.set(key, { error: error.message });
    }
    updatePreview(key);
  }
}

function progressIn(prefix) {
  return (text, value = 0) => {
    $(`#${prefix}-progress`).hidden = text === null;
    if (text === null) return;
    $(`#${prefix}-label`).textContent = text;
    $(`#${prefix}-bar`).value = value;
  };
}

const showProgress = progressIn('upload');
const showBackupProgress = progressIn('backup');

// Whether files can be uploaded right now.
export function canUpload() {
  return app.ready && Boolean(storage) && !app.busy;
}

// Also used by the drawing tab, which brings its own folder and progress display.
// The files the badge has in a folder, null if its firmware does not have the folder.
export function filesOn(folder) {
  return files[folder];
}

// replace: overwrite files of the same name without asking, for the code editor
export function uploadFiles(selected, { folder = dir, progress = showProgress, replace = false } = {}) {
  if (!selected.length) return Promise.resolve(0);
  const { label, noun, extension, inspect } = FORMATS[folder];
  let uploaded = 0;

  return app.run(async () => {
    const queue = [];
    const names = new Set();
    const capacity = storage.blocks * storage.block;

    for (const file of selected) {
      if (!extension.test(file.name)) {
        notify(`${file.name} is not a .${folder} file. Each tab only takes its own type.`, 'error');
        continue;
      }
      if (file.size > capacity) {
        notify(`${file.name} (${formatBytes(file.size)}) is larger than the badge's whole storage.`, 'error');
        continue;
      }
      const data = new Uint8Array(await file.arrayBuffer());
      if (!inspect(data)) {
        const hint = folder === 'lua' ? ' Scripts have to be Lua source text of at most 12 KB.' : '';
        notify(`${file.name} does not contain a valid ${label} ${noun}.${hint}`, 'error');
        continue;
      }
      const name = uniqueName(deviceName(file.name, folder), names);
      names.add(name);
      queue.push({ name, data });
    }
    if (!queue.length) return;

    const existing = new Set(filesIn(folder).map(({ name }) => name));
    const replaced = queue.filter(({ name }) => existing.has(name));
    if (replaced.length && !replace) {
      const list = replaced.map(({ name }) => name).join(', ');
      if (!(await confirmDialog('Replace existing files?', list, 'Replace'))) return;
    }

    try {
      for (const [i, { name, data }] of queue.entries()) {
        const title = `Uploading ${name}${queue.length > 1 ? ` (${i + 1} of ${queue.length})` : ''}`;
        progress(title);

        await app.badge.prepareUpload(folder);
        const info = await app.badge.storage();
        // even when replacing, the old file keeps its blocks until the new one is complete
        const available = Math.max(0, info.free - RESERVED_BLOCKS);
        const needed = blocksFor(data.length, info.block);
        if (needed > available) {
          const hint = existing.has(name)
            ? ' Replacing needs room for both copies, so delete the old file first.'
            : '';
          notify(
            `Not enough space for ${name}: it needs ${formatBytes(needed * info.block)} and ` +
            `${formatBytes(available * info.block)} is available.${hint}`,
            'error',
          );
          continue;
        }

        await app.badge.upload(folder, name, data, (value) => progress(title, value));
        forgetPreview(keyOf(folder, name));
        uploaded++;
      }
    } finally {
      progress(null);
      if (app.smp.connected) await refreshMedia();
    }
    if (uploaded) notify(`Uploaded ${uploaded} ${label} ${uploaded === 1 ? 'file' : 'files'}.`, 'success');
  }).then(() => uploaded);
}

// What firmware without walk() can still back up: the files the badge knows by name.
const KNOWN_ROOT_FILES = [...DEFAULTS, SETTINGS];
// '/lfs1/' and the path have to fit CONFIG_MCUMGR_GRP_FS_PATH_LEN.
const MAX_PATH = 64 - '/lfs1/'.length;
const utf8 = new TextEncoder();

// [{ path, size }] relative to the root; size is undefined for a file that may not exist.
// complete is false when the firmware cannot list the whole storage.
async function listBadge() {
  const all = await app.badge.walk();
  if (all) return { complete: true, found: all };
  return {
    complete: false,
    found: [
      ...KNOWN_ROOT_FILES.map((path) => ({ path })),
      ...Object.keys(FORMATS).flatMap((folder) =>
        filesIn(folder).map(({ name, size }) => ({ path: `${folder}/${name}`, size }))),
    ],
  };
}

// A backup holds every file on the badge, laid out as on its storage: settings.json,
// default.gif, default.png, default.lua, gif/*.gif, png/*.png, lua/*.lua and anything else there is.
// Hidden files are left out, they are unfinished uploads and temporary copies.
// Throws if any file could not be read, so nothing relies on a partial backup.
async function saveBackup() {
  try {
    await refreshMedia();
    const { complete, found } = await listBadge();
    const entries = [];
    for (const [i, { path, size }] of found.entries()) {
      showBackupProgress(`Reading ${path}`, i / found.length);
      const data = await app.badge.readFile(path);
      if (data) entries.push({ path, data });
      else if (size !== undefined) throw new Error(`${path} disappeared from the badge. Please try again.`);
    }
    if (!entries.length) {
      notify('There are no files on the badge to back up.');
      return;
    }

    const url = URL.createObjectURL(createZip(entries));
    const link = el('a', { href: url, download: `cool-s-backup-${new Date().toISOString().slice(0, 10)}.zip` });
    document.body.append(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 60000);
    notify(`Saved ${entries.length} ${entries.length === 1 ? 'file' : 'files'} in the backup.`, 'success');
    if (!complete) {
      notify('This firmware only lets the backup include the settings, the default images and the gif ' +
        'and png folders. Update it to also back up any other files.');
    }
  } finally {
    showBackupProgress(null);
  }
}

function downloadBackup() {
  return app.run(saveBackup);
}

function chooseErase() {
  const dialog = $('#erase-dialog');
  dialog.returnValue = '';
  dialog.showModal();
  return new Promise((resolve) => {
    dialog.addEventListener('close', () => resolve(dialog.returnValue), { once: true });
  });
}

// Formats the storage: every file and the settings are gone, as on a new badge.
function eraseStorage() {
  return app.run(async () => {
    // the restart would undo an update that is still on trial, and the badge refuses
    const active = (await app.badge.images()).find((image) => image.active);
    if (active && !active.confirmed) {
      throw new Error('The badge is still testing its new firmware. Try again in a minute.');
    }

    const choice = await chooseErase();
    if (choice !== 'backup' && choice !== 'erase') return;
    if (choice === 'backup') await saveBackup();

    await app.badge.formatStorage();
    clearPreviews();
    app.restarting = true;
    try {
      app.smp.disconnect();
    } finally {
      app.restarting = false;
    }
    notify('The badge restarts and erases its storage, which takes a few seconds. Connect again afterwards.',
      'success');
  });
}

function validPath(path) {
  return utf8.encode(path).length <= MAX_PATH && path.split('/').every((part) => part && !part.includes('\0'));
}

// Turns the zip into what to write where. Media is checked, since the badge tries to show it.
async function readBackup(file, complete) {
  const capacity = storage.blocks * storage.block;
  const entries = (await readZip(await file.arrayBuffer(), capacity))
    .map(({ path, data }) => ({ path: path.replace(/\\/g, '/').replace(/^\/+/, ''), data }))
    // hidden files and what macOS adds when zipping a folder by hand
    .filter(({ path }) => !path.split('/').some((part) => part.startsWith('.') || part === '__MACOSX'));

  // a zipped backup folder puts everything one level down
  const top = entries[0]?.path.split('/')[0];
  if (entries.length && !Object.hasOwn(FORMATS, top.toLowerCase()) &&
      entries.every(({ path }) => path.startsWith(`${top}/`))) {
    for (const entry of entries) entry.path = entry.path.slice(top.length + 1);
  }

  const restore = [];
  const skipped = [];
  const taken = new Set();
  const names = Object.fromEntries(Object.keys(FORMATS).map((folder) => [folder, new Set()]));
  for (const entry of entries) {
    const parts = entry.path.split('/');
    let { path } = entry;
    let folder = null;
    let reason = null;

    if (parts.length === 1 && DEFAULTS.includes(path.toLowerCase())) {
      path = path.toLowerCase();
      const format = FORMATS[path.slice(-3)];
      if (!format.inspect(entry.data)) reason = `not a valid ${format.noun}`;
    } else if (parts.length === 2 && Object.hasOwn(FORMATS, parts[0].toLowerCase())) {
      folder = parts[0].toLowerCase();
      if (files[folder] === null) {
        reason = 'needs newer firmware';
      } else if (!FORMATS[folder].extension.test(parts[1]) || !FORMATS[folder].inspect(entry.data)) {
        reason = `not a ${FORMATS[folder].label}`;
      } else {
        // names the web UI gave stay as they are
        const name = uniqueName(deviceName(parts[1], folder), names[folder]);
        names[folder].add(name);
        path = `${folder}/${name}`;
      }
    } else if (!validPath(path)) {
      reason = 'name too long';
    } else if (!complete && parts.length > 1) {
      reason = 'needs newer firmware';
    }
    if (!reason && taken.has(path)) reason = 'twice in the backup';

    if (reason) {
      skipped.push(`${entry.path} (${reason})`);
      continue;
    }
    taken.add(path);
    restore.push({ path, folder, data: entry.data });
  }

  // what the badge shows and its settings first, then the folders
  const rank = ({ path }) => (KNOWN_ROOT_FILES.includes(path) ? 0 : path.includes('/') ? 2 : 1);
  restore.sort((a, b) => rank(a) - rank(b));
  return { restore, skipped };
}

// Writes a backup onto the badge, for example after its storage was erased.
// Files already on the badge stay unless the backup has one with the same path.
function restoreBackup(file) {
  return app.run(async () => {
    const { complete, found } = await listBadge();
    const { restore, skipped } = await readBackup(file, complete);
    if (skipped.length) notify(`Not restored: ${skipped.join(', ')}`);
    if (!restore.length) throw new Error(`${file.name} has nothing to restore.`);

    const sizes = new Map(found.map(({ path, size }) => [path, size]));
    const replaced = restore.filter(({ path }) => sizes.get(path) !== undefined);
    const needed = restore.reduce((sum, { path, data }) =>
      sum + blocksFor(data.length, storage.block) - blocksFor(sizes.get(path) ?? 0, storage.block), 0);
    const available = Math.max(0, storage.free - RESERVED_BLOCKS);
    const settings = restore.some(({ path }) => path === SETTINGS);

    const lines = [`${restore.length} ${restore.length === 1 ? 'file' : 'files'} will be written to the badge.`];
    if (replaced.length) lines.push(`Replaced: ${replaced.map(({ path }) => path).join(', ')}.`);
    if (restore.some(({ path }) => DEFAULTS.includes(path))) lines.push('The default images are overwritten.');
    if (settings) lines.push('The settings are overwritten.');
    if (needed > available) {
      lines.push(`The backup needs about ${formatBytes(needed * storage.block)}, but only ` +
        `${formatBytes(available * storage.block)} is free, so some files will be left out.`);
    }
    if (!(await confirmDialog('Restore this backup?', lines.join(' '), 'Restore'))) return;

    let restored = 0;
    const left = [];
    try {
      for (const [i, { path, folder, data }] of restore.entries()) {
        const title = `Restoring ${path} (${i + 1} of ${restore.length})`;
        const onProgress = (value) => showBackupProgress(title, value);
        showBackupProgress(title);

        if (complete || folder) {
          // the old copy keeps its blocks until the new one is complete
          if (folder) await app.badge.prepareUpload(folder);
          const info = await app.badge.storage();
          if (blocksFor(data.length, info.block) > Math.max(0, info.free - RESERVED_BLOCKS)) {
            left.push(path);
            continue;
          }
          if (complete) await app.badge.put(path, data, onProgress);
          else await app.badge.upload(folder, path.slice(folder.length + 1), data, onProgress);
          if (folder) forgetPreview(path);
        } else {
          // older firmware: written in place, the badge removes the old file first
          await app.badge.putInRoot(path, data, onProgress);
        }
        restored++;
      }
    } finally {
      showBackupProgress(null);
      if (app.smp.connected) await refreshMedia();
    }
    if (left.length) notify(`Not enough space for: ${left.join(', ')}`, 'error');
    if (restored) notify(`Restored ${restored} ${restored === 1 ? 'file' : 'files'}.`, 'success');
    if (settings && !left.includes(SETTINGS)) {
      notify('Restart the badge to load the restored settings. Until then, changing its brightness ' +
        'would save over them.');
    }
  });
}

async function deleteFile(folder, name) {
  if (!(await confirmDialog('Delete this file?', `${name} will be removed from the badge.`, 'Delete'))) return;
  await app.run(async () => {
    await app.badge.remove(folder, name);
    forgetPreview(keyOf(folder, name));
    await refreshMedia();
  });
}

export function setupMedia() {
  setupTabs($('#media-tabs'), (tab) => {
    dir = tab.dataset.dir;
    render();
  });

  $('#preview-toggle').addEventListener('change', async ({ target }) => {
    previewsOn = target.checked;
    render();
    if (!previewsOn) await app.stopBackground();
  });

  $('#refresh').addEventListener('click', () => app.run(async () => {
    clearPreviews();
    await refreshMedia();
  }));

  const input = $('#file-input');
  input.addEventListener('change', () => {
    uploadFiles([...input.files]);
    input.value = '';
  });

  $('#backup-download').addEventListener('click', downloadBackup);
  $('#storage-erase').addEventListener('click', eraseStorage);
  const backupInput = $('#backup-input');
  $('#backup-restore').addEventListener('click', () => backupInput.click());
  backupInput.addEventListener('change', () => {
    const [file] = backupInput.files;
    backupInput.value = '';
    if (file) restoreBackup(file);
  });

  const zone = $('#dropzone');
  zone.addEventListener('dragover', (event) => {
    event.preventDefault();
    if (!input.disabled) zone.classList.add('over');
  });
  zone.addEventListener('dragleave', () => zone.classList.remove('over'));
  zone.addEventListener('drop', (event) => {
    event.preventDefault();
    zone.classList.remove('over');
    if (!input.disabled) uploadFiles([...event.dataTransfer.files]);
  });

  app.addEventListener('change', update);
  render();
  return update;
}
