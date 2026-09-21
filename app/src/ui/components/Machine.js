// Which machine is being played, and on which of its cables.
//
// Hitting a pad and hearing nothing because you are driving the module in the
// browser while looking at a rack is the failure this exists to prevent, so
// it is a statement rather than an inference, and it is on screen wherever
// something can be played from.
//
// The cable is part of it because it is part of the patch: `CcMapping`
// filters by source port, so a binding learned while the surface claims one
// cable does not fire when it claims another (src/control/cc_mapper.h).

import { el, classes } from '../dom.js';
import { Select } from './Select.js';
import { MUSICAL_PORTS } from '../../protocol/names.js';
import './Machine.css';

// `port` names the cable the badge is about - the surface's lead, say, rather
// than the keyboard's. Left out, it is whatever the keyboard's own bar is set
// to (`ui.play.port`), which is the one cable the page plays on.
export function MachineBadge(app, { compact = false, port = undefined } = {}) {
  const machine = app.play.machine(port);
  return el('div', { class: classes('machine', machine.ok ? 'ok' : 'warn', compact && 'compact') },
    el('span', { class: 'machine-dot' }),
    el('span', { class: 'machine-name' }, machine.kind === 'module' ? 'built-in module' : machine.name),
    compact ? null : el('span', { class: 'hint' }, machine.reaches));
}

// Which of the module's logical inputs something plays into. Never the
// control cable: MUSICAL_PORTS leaves it out, because a page that could send
// notes down the protocol's cable could stall a patch transfer with a chord.
//
// The keyboard's cable is the default; the surface passes its own, which is
// one lead for all twenty-four of its controls (services/surface.js).
export function PortSelect(app, {
  label = 'into',
  said = 'the module input the keys play into',
  value = app.play.port,
  onChange = (port) => app.play.setPort(port),
} = {}) {
  return el('label', { class: 'machine-port' },
    el('span', { class: 'field-name' }, label),
    Select({ options: MUSICAL_PORTS, value, 'aria-label': said, onChange }));
}
