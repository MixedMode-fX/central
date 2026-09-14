// A labelled control, in a grid of them. The label stays above its control,
// as every other field on the card does: beside it in a fixed column, a phone
// leaves the select about eight characters.

import { el } from '../dom.js';

export function Field({ label, hint = null }, ...controls) {
  return el('div', { class: 'field' },
    label ? el('span', { class: 'field-name' }, label) : null,
    ...controls,
    hint ? el('span', { class: 'hint' }, hint) : null);
}

export const Fields = (...fields) => el('div', { class: 'fields' }, ...fields);

// A word before a control on one line, for a row of a few short controls.
export const Labelled = (label, ...controls) => [el('span', { class: 'field-name' }, label), ...controls];
