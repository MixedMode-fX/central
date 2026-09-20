// One gate jack, in full: which way it faces, and what it is wired to.
//
// **A direction is a setting, not a kind of jack.** Which way it faces is the
// one control here; where it is patched is the canvas's to say, and this only
// reports it (see `NodeCard`).

import * as P from '../../protocol/generated.js';
import { el, classes } from '../dom.js';
import { Segmented } from '../components/Segmented.js';
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
      onChange: (direction) => app.editor.setJack(index, direction),
    }),
    used ? el('div', { class: 'port dom-gate' },
      el('div', { class: 'port-head' },
        el('span', { class: 'port-name' }, writes ? 'writes' : 'reads')),
      BusNeighbours(app, { domain: Domain.Gate, buses: port.buses, self: `jack:${index}`, writes,
                           unused: 'not connected — patch it on the canvas' })) : null);
}
