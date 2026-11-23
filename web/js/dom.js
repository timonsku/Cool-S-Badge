export const $ = (selector) => document.querySelector(selector);

// Strings become text nodes, so names coming from the badge are never parsed as HTML.
export function el(tag, props = {}, ...children) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(props)) {
    if (key in node) node[key] = value;
    else node.setAttribute(key, value);
  }
  node.append(...children);
  return node;
}

export function notify(message, kind = 'info') {
  const close = el('button', { type: 'button', className: 'close', 'aria-label': 'Dismiss', textContent: '×' });
  const note = el('div', { className: `note ${kind}` }, el('span', {}, message), close);
  close.addEventListener('click', () => note.remove());

  const notes = $('#notes');
  notes.append(note);
  while (notes.children.length > 4) notes.firstElementChild.remove();
  if (kind !== 'error') setTimeout(() => note.remove(), 6000);
}

export function confirmDialog(title, message, action) {
  const dialog = $('#confirm');
  $('#confirm-title').textContent = title;
  $('#confirm-message').textContent = message;
  $('#confirm-action').textContent = action;
  dialog.returnValue = '';
  dialog.showModal();
  return new Promise((resolve) => {
    dialog.addEventListener('close', () => resolve(dialog.returnValue === 'ok'), { once: true });
  });
}

// Tabs may share a panel, as GIF and PNG do.
export function setupTabs(tablist, onSelect) {
  const tabs = [...tablist.querySelectorAll('[role="tab"]')];

  const select = (tab) => {
    const panel = document.getElementById(tab.getAttribute('aria-controls'));
    for (const other of tabs) {
      other.setAttribute('aria-selected', String(other === tab));
      other.tabIndex = other === tab ? 0 : -1;
      const otherPanel = document.getElementById(other.getAttribute('aria-controls'));
      otherPanel.hidden = otherPanel !== panel;
    }
    panel.setAttribute('aria-labelledby', tab.id);
    onSelect(tab);
  };

  tabs.forEach((tab, i) => {
    tab.addEventListener('click', () => select(tab));
    tab.addEventListener('keydown', (event) => {
      const step = { ArrowRight: 1, ArrowLeft: -1 }[event.key];
      if (!step) return;
      const next = tabs[(i + step + tabs.length) % tabs.length];
      next.focus();
      select(next);
    });
  });

  return select;
}
