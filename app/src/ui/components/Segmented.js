// A row of chips, one of which is on: a choice small enough that every option
// can be on screen at once, which is what a two- or three-way setting should
// look like. Every setting that reads as a switch is one of these.

import { el, classes } from '../dom.js';
import './Segmented.css';

export function Segmented({ options, value, onChange, label }) {
  return el('div', { class: 'segmented', role: 'group', 'aria-label': label },
    options.map((option) => el('button', {
      type: 'button', class: classes('chip', option.value === value && 'on'),
      'aria-pressed': option.value === value ? 'true' : 'false',
      title: option.hint ?? null,
      onclick: () => { if (option.value !== value) onChange(option.value); },
    }, option.label)));
}
