// A titled box, which is what every tab is a column of.

import { el, classes } from '../dom.js';

export function Panel(title, ...children) {
  return el('section', { class: 'panel' }, title ? el('h2', {}, title) : null, ...children);
}

export const Row = (...children) => el('div', { class: 'row' }, ...children);

export const Hint = (text, klass = '') => el('p', { class: classes('hint', klass) }, text);

// A box of things gone wrong, or worth knowing: a list under a heading, or a
// sentence that is dismissed by pressing it.
export function Notice({ kind, title = null, items = [], text = null, onClick = null }) {
  return el('div', { class: classes('notice', kind), onclick: onClick },
    title ? el('h4', {}, title) : null,
    text,
    items.length ? el('ul', {}, items.map((item) => el('li', {}, item))) : null);
}
