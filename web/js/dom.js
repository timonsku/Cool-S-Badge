export const $ = (selector) => document.querySelector(selector);

// phones and tablets, which cannot drop files and tap instead of clicking
export const touchScreen = matchMedia('(pointer: coarse)').matches;

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

// Asks for a line of text. check(value) returns why it cannot be used, or '' if it can.
// Resolves to the text, or null if cancelled.
export function promptDialog(title, label, value, action, check = () => '') {
  const dialog = $('#prompt');
  const input = $('#prompt-input');
  const error = $('#prompt-error');
  $('#prompt-title').textContent = title;
  $('#prompt-label').textContent = label;
  $('#prompt-action').textContent = action;
  input.value = value;
  error.textContent = '';
  input.removeAttribute('aria-invalid');

  const validate = () => {
    const problem = input.value.trim() ? check(input.value.trim()) : '';
    error.textContent = problem;
    input.toggleAttribute('aria-invalid', Boolean(problem));
    $('#prompt-action').disabled = !input.value.trim() || Boolean(problem);
    return !problem;
  };
  const onSubmit = (event) => {
    // only the action button is checked, Cancel always closes
    if (event.submitter?.value === 'ok' && !validate()) event.preventDefault();
  };
  // Enter would submit with the form's first button, which is Cancel
  const onKey = (event) => {
    if (event.key !== 'Enter') return;
    event.preventDefault();
    if (validate() && input.value.trim()) $('#prompt-action').click();
  };
  input.addEventListener('input', validate);
  input.addEventListener('keydown', onKey);
  dialog.querySelector('form').addEventListener('submit', onSubmit);
  validate();
  dialog.returnValue = '';
  dialog.showModal();
  // the name without its extension, which is usually what is changed
  const dot = value.lastIndexOf('.');
  input.setSelectionRange(0, dot > 0 ? dot : value.length);
  return new Promise((resolve) => {
    dialog.addEventListener('close', () => {
      input.removeEventListener('input', validate);
      input.removeEventListener('keydown', onKey);
      dialog.querySelector('form').removeEventListener('submit', onSubmit);
      resolve(dialog.returnValue === 'ok' ? input.value.trim() : null);
    }, { once: true });
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
