// A <details> whose open state outlives a re-render. The page is rebuilt
// under it on every edit, so what it remembers by itself is lost: a binding
// editor that folded shut the moment its CC number was set was not usable.

import { el, classes } from '../dom.js';

export function Disclosure({ summary, open, onToggle, class: klass = '' }, ...children) {
  const node = el('details', { class: klass }, el('summary', {}, summary), ...children);
  node.open = open;
  node.addEventListener('toggle', () => onToggle(node.open));
  return node;
}

// The open state of a disclosure, kept in the app's `ui.opened` under a key.
export const remembered = (ui, key) => ({
  open: ui.opened.has(key),
  onToggle: (open) => { if (open) ui.opened.add(key); else ui.opened.delete(key); },
});

