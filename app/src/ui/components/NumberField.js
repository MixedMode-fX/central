// A number field that clamps what is typed into it and reports a number.
// Numeric keyboard on a phone, and a fallback for a field left empty.

import { el, classes } from '../dom.js';

export function NumberField({ value, min, max, step = 1, onChange, fallback = min, wide = false,
                              class: klass = '', ...attrs }) {
  const clamp = (v) => Math.max(min, Math.min(max, Number.isFinite(v) ? v : fallback));
  return el('input', {
    type: 'number', class: classes('number', wide && 'wide', klass),
    min: String(min), max: String(max), step: String(step), value: String(value),
    inputmode: Number.isInteger(step) ? 'numeric' : 'decimal',
    ...attrs,
    onchange: (e) => {
      const chosen = clamp(e.target.value === '' ? NaN : Number(e.target.value));
      e.target.value = String(chosen);
      onChange(chosen);
    },
  });
}
