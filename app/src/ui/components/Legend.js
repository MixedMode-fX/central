// One chip per trace, in the trace's colour, and pressing one hides it: a
// legend is a list of what is on the screen, and the fastest way to look at
// three traces out of nine is to put the other six away for a moment.

import { el, classes } from '../dom.js';
import './Legend.css';

export function Legend({ items, hidden, onToggle }) {
  return el('div', { class: 'legend', role: 'group', 'aria-label': 'traces' },
    items.map((item) => el('button', {
      type: 'button', class: classes('legend-chip', hidden.has(item.key) && 'off'),
      style: `--key:${item.colour}`,
      'aria-pressed': hidden.has(item.key) ? 'false' : 'true',
      title: hidden.has(item.key) ? `show ${item.label}` : `hide ${item.label}`,
      onclick: () => onToggle(item.key),
    }, item.label)));
}

export function toggleIn(set, key) {
  if (set.has(key)) set.delete(key); else set.add(key);
}
