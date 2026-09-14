// A checkbox with its word beside it.

import { el } from '../dom.js';

export function Switch({ checked, label, hint = null, onChange }) {
  const box = el('input', {
    type: 'checkbox', class: 'switch',
    onchange: (e) => onChange(e.target.checked),
  });
  box.checked = Boolean(checked);
  return el('label', { class: 'bool', title: hint }, box, label ? el('span', {}, label) : null);
}
