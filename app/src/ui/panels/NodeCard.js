// One node: what it reads, what it writes, and the buses they are on; the
// signals on those buses, live; its parameters; its grid, if its algorithm
// has one. Everything here is driven
// by what the device reported - port names and domains, parameter ranges and
// kinds, enum options - so an algorithm added to the firmware gets a working
// card for free.

import { el } from '../dom.js';
import { IconButton } from '../components/IconButton.js';
import { KeyBadge } from '../components/KeyBadge.js';
import { BusNeighbours } from '../controls/BusNeighbours.js';
import { ParamSections } from '../controls/ParamControl.js';
import { ModRoutes } from '../controls/ModRoute.js';
import { inletName, outletName } from '../../core/patch.js';
import { domainName } from '../../core/validate.js';
import { algorithmGrid } from './algorithms.js';
import { BlockSignalsPanel } from '../scope/ScopePanels.js';
import { BlockKind } from '../../core/graph.js';
import './cards.css';

// One port of one node: what it is called, and what it is wired to.
//
// **There is no bus selector here.** Routing is the canvas's, and a dropdown
// per port was the rigid half of it: it could only ever say *one* bus, so
// summing two sources into an inlet meant moving their sources onto one bus
// and taking whatever else that merged with it. A port is a set of buses now
// (src/bus/domain.h), and a set is made by dragging.
function Port(app, index, isOutlet, i) {
  const node = app.state.patch.nodes[index];
  const d = app.device.byId.get(node.algorithmId);
  const domain = isOutlet ? d.outDomain[i] : d.inDomain[i];
  const buses = (isOutlet ? node.outBuses[i] : node.inBuses[i]) ?? [];
  const name = isOutlet ? outletName(d, i) : inletName(d, i);
  const optional = isOutlet || i >= d.minIn;

  return el('div', { class: `port dom-${domainName(domain)}` },
    el('div', { class: 'port-head' },
      el('span', { class: 'port-name' }, name, optional ? null : el('span', { class: 'required' }, '*'))),
    BusNeighbours(app, { domain, buses, self: `node:${index}#${isOutlet ? 'out' : 'in'}${i}`,
                         writes: isOutlet,
                         unused: optional ? 'not connected' : 'must be connected' }));
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
    // Which key this node is playing in, which is the one musical decision it
    // does not carry itself (src/midi/global_key.h) - and, where a cable has
    // rooted it, what that cable is playing instead.
    d.readsKey ? el('div', { class: 'node-key' }, KeyBadge(app, { index })) : null,
    d.summary ? el('p', { class: 'hint summary' }, d.summary) : null,
    el('div', { class: 'ports' },
      ports(false, d.nIn, 'reads', 'reads nothing'),
      ports(true, d.nOut, 'writes', 'writes nothing')),
    // Right under the ports, because it is the same list drawn over time:
    // what the node read on each, and what it wrote.
    BlockSignalsPanel(app, { kind: BlockKind.Node, index }),
    ParamSections(app, index),
    ModRoutes(app, index),
    grid ? grid(app, index) : null);
}

