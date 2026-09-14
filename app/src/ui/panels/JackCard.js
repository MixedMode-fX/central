// One gate jack, in full: which way it faces, and the gate bus it is on.
//
// **A direction is a setting, not a kind of jack.** Which way and onto what
// are two controls, and the bus survives the turn.

import * as P from '../../protocol/generated.js';
import { el, classes } from '../dom.js';
import { Segmented } from '../components/Segmented.js';
import { BusSelect } from '../controls/BusSelect.js';
import { BusNeighbours } from '../controls/BusNeighbours.js';
import { GATE_DIRECTIONS } from '../../protocol/names.js';
import { Domain } from '../../core/validate.js';
import './cards.css';

export function JackCard(app, index) {
  const port = app.state.patch.gatePorts[index];
  const used = port.direction !== P.GatePortDirection.GATE_PORT_UNUSED;
  const writes = port.direction === P.GatePortDirection.GATE_PORT_IN;
  const title = `jack ${index + 1}`;

  return el('div', { class: classes('jack-card', !used && 'unused') },
    el('div', { class: 'jack-head' },
      el('h4', {}, title),
      el('span', { class: 'hint' }, GATE_DIRECTIONS.find((d) => d.value === port.direction)?.hint ?? '')),
    Segmented({
      options: GATE_DIRECTIONS, value: port.direction, label: `${title} direction`,
      onChange: (direction) => app.editor.setJack(index, direction, port.bus),
    }),
    used ? el('div', { class: 'port dom-gate' },
      el('label', { class: 'port-head' },
        el('span', { class: 'port-name' }, writes ? 'writes' : 'reads'),
        BusSelect({
          caps: app.device.capabilities, domain: Domain.Gate, value: port.bus, none: null,
          onChange: (bus) => app.editor.setJack(index, port.direction, bus),
        })),
      BusNeighbours(app, { domain: Domain.Gate, bus: port.bus, self: `jack:${index}`, writes })) : null);
}
