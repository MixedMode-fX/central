// A native select over a list of options. Values are compared as strings,
// because that is what a <select> holds; `onChange` gets the option's own
// value back, in whatever type it was given.

import { el } from '../dom.js';

export function Select({ options, value, onChange, ...attrs }) {
  const byValue = new Map(options.map((o) => [String(o.value), o.value]));
  const select = el('select', {
    ...attrs,
    onchange: (e) => onChange(byValue.get(e.target.value)),
  }, options.map((o) => {
    const option = el('option', { value: String(o.value), title: o.hint }, o.label);
    if (String(o.value) === String(value)) option.selected = true;
    return option;
  }));
  return select;
}

// The options of a numbered range: `0..n-1` named by `label(i)`.
export const range = (n, label, from = 0) =>
  Array.from({ length: n - from }, (_, i) => ({ value: from + i, label: label(from + i) }));
