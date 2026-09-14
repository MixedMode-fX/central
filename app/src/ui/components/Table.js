// A table that scrolls in its own box on a phone rather than widening the
// page.

import { el } from '../dom.js';

export function Table({ head, rows }) {
  return el('div', { class: 'scroll-x' }, el('table', { class: 'list' },
    el('thead', {}, el('tr', {}, head.map((cell) => el('th', {}, cell)))),
    el('tbody', {}, rows.map((row) => el('tr', {}, row.map((cell) => el('td', {}, cell)))))));
}
