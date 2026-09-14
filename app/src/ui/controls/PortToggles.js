// A port mask, as a row of toggles. The control cable is not offered: it is
// reserved for the protocol, and a patch that could route music onto it - or
// take it away from the app - is a patch that could lock the module out.

import { el, classes } from '../dom.js';
import { MUSICAL_PORTS, ALL_MUSICAL } from '../../protocol/names.js';

export function PortToggles({ mask, onChange, label }) {
  return el('div', { class: 'segmented', role: 'group', 'aria-label': label },
    MUSICAL_PORTS.map((p) => {
      const on = (mask & p.value) !== 0;
      return el('button', {
        type: 'button', class: classes('chip', on && 'on'),
        onclick: () => onChange(on ? (mask & ~p.value) : (mask | p.value)),
      }, p.label);
    }),
    el('button', {
      type: 'button', class: 'chip ghost',
      onclick: () => onChange(mask === ALL_MUSICAL ? 0 : ALL_MUSICAL),
    }, 'all'));
}
