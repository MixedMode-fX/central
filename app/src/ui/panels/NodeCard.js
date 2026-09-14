// One node: what it reads, what it writes, and the buses they are on; its
// parameters; its grid, if its algorithm has one. Everything here is driven
// by what the device reported - port names and domains, parameter ranges and
// kinds, enum options - so an algorithm added to the firmware gets a working
// card for free.

import { el } from '../dom.js';
import { IconButton } from '../components/IconButton.js';
import { BusSelect } from '../controls/BusSelect.js';
import { BusNeighbours } from '../controls/BusNeighbours.js';
import { ParamSections } from '../controls/ParamControl.js';
import { ModRoutes } from '../controls/ModRoute.js';
import { inletName, outletName } from '../../core/patch.js';
import { domainName } from '../../core/validate.js';
import { algorithmGrid } from './algorithms.js';
import './cards.css';

function Port(app, index, isOutlet, i) {
  const node = app.state.patch.nodes[index];
  const d = app.device.byId.get(node.algorithmId);
  const domain = isOutlet ? d.outDomain[i] : d.inDomain[i];
  const bus = isOutlet ? node.outBus[i] : node.inBus[i];
  const name = isOutlet ? outletName(d, i) : inletName(d, i);
  const optional = isOutlet || i >= d.minIn;

  return el('div', { class: `port dom-${domainName(domain)}` },
    el('label', { class: 'port-head' },
      el('span', { class: 'port-name' }, name, optional ? null : el('span', { class: 'required' }, '*')),
      BusSelect({
        caps: app.device.capabilities, domain, value: bus,
        none: optional ? 'not connected' : '— must be connected',
        onChange: (chosen) => app.editor.setConnection(index, isOutlet, i, chosen),
      })),
    BusNeighbours(app, { domain, bus, self: `node:${index}#${isOutlet ? 'out' : 'in'}${i}`, writes: isOutlet }));
}

// The header is the details panel's when the card is the details panel: the
// name, the index and the remove button sit in the panel's own bar.
export function NodeCard(app, index, { header = true } = {}) {
  const node = app.state.patch.nodes[index];
  const d = app.device.byId.get(node.algorithmId);
  if (!d) return el('section', { class: 'node bad' }, `node ${index}: unknown algorithm ${node.algorithmId}`);

  const ports = (isOutlet, n, title, none) => el('div', { class: 'port-group' },
    el('h4', {}, n ? title : none),
    Array.from({ length: n }, (_, i) => Port(app, index, isOutlet, i)));
  const grid = algorithmGrid(d.name);

  return el('section', { class: 'node', id: `node-${index}` },
    header ? el('header', {},
      el('span', { class: 'index' }, index),
      el('h3', {}, d.name),
      d.wantsTick ? el('span', { class: 'tag' }, 'clocked') : null,
      IconButton({ icon: 'trash', label: `remove ${d.name} ${index}`, class: 'ghost danger',
                   onclick: () => app.editor.removeNode(index) })) : null,
    d.summary ? el('p', { class: 'hint summary' }, d.summary) : null,
    el('div', { class: 'ports' },
      ports(false, d.nIn, 'reads', 'reads nothing'),
      ports(true, d.nOut, 'writes', 'writes nothing')),
    ParamSections(app, index),
    ModRoutes(app, index),
    grid ? grid(app, index) : null);
}

