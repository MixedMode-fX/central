// The views. Everything here is driven by what the device reported: inlet and
// outlet names and domains, parameter ranges and kinds, enum option names, and
// the module's real bus counts. Nothing about any specific algorithm is
// hardcoded, so an algorithm added to the firmware gets a working panel for
// free - and, since the registry now carries port names and a summary, a
// *described* one rather than a grid of numbered sockets.

import * as P from './protocol.js';
import { Domain, busCount, domainName } from './validate.js';
import { GATE_DIRECTIONS } from './names.js';
import { icon, midiIcon } from './icons.js';
// The port names live with the patch-shape code, because the canvas needs them
// too and it must not have to reach through the views to get them.
import { inletName, outletName, modParamName, planJackDirection } from './graph.js';
export { inletName, outletName };

const el = (tag, attrs = {}, ...children) => {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(attrs)) {
    if (key === 'class') node.className = value;
    else if (key.startsWith('on')) node.addEventListener(key.slice(2), value);
    else if (value !== null && value !== undefined) node.setAttribute(key, value);
  }
  for (const child of children.flat()) {
    if (child === null || child === undefined) continue;
    node.append(child.nodeType ? child : document.createTextNode(String(child)));
  }
  return node;
};
export { el };

// A button that is an icon. Its word goes in the tooltip and the accessible
// name, and `text` puts it beside the icon too where there is room for it -
// the tabs, and the two buttons at the top of the page.
export function iconButton({ icon: name, label, text = null, class: klass = '', ...attrs }) {
  return el('button', {
    type: 'button', class: `icon-btn ${text ? 'with-text' : ''} ${klass}`.trim(),
    title: label, 'aria-label': label, ...attrs,
  }, icon(name), text ? el('span', { class: 'btn-text' }, text) : null);
}

// The learn button for one parameter: an icon, its meaning in the tooltip and
// the accessible name, and the bound CC in both so neither a pointer nor a
// screen reader has to go looking for it.
export function learnButton(app, index, at, name, binding) {
  const what = binding ? `CC ${binding.cc} is bound to ${name} - learn another` : `learn a controller for ${name}`;
  return el('button', {
    type: 'button', class: `ghost learn ${binding ? 'bound' : ''}`,
    title: what, 'aria-label': what,
    onclick: () => app.learn(index, at),
  }, midiIcon());
}

// The CV button beside it: a control signal onto this parameter, from here.
//
// A route used to be made on the canvas only - drag a control outlet onto a
// block, pick a parameter from a dropdown - which is the right gesture when
// the signal is the thing in hand and the wrong one when the parameter is:
// "modulate *this* from something" started two panels away. So every
// parameter has the route beside its learn button, and pressing it lists the
// CV buses with what writes each one. The route it makes is the same route
// the drag makes (`planBusModulation` and `planModulation` build the same
// object), and once it exists the row below the parameters edits its depth.
export function cvButton(app, index, at, name, route) {
  const what = route
    ? `${name} is modulated from CV bus ${route.bus} - change or remove the route`
    : `modulate ${name} from a CV bus`;
  const button = el('button', {
    type: 'button', class: `ghost learn cv ${route ? 'bound' : ''}`,
    title: what, 'aria-label': what,
  }, icon('cv'));
  button.addEventListener('click', () => openBusMenu(app, button, index, at, name, route));
  return button;
}

// The CV buses, as a menu under the button that asked: the ones something
// writes first, each saying what, then the rest - a bus nothing writes is a
// route to silence, which is still occasionally what somebody wants while the
// source is on its way.
function openBusMenu(app, anchor, index, at, name, route) {
  closeMenu();
  const caps = app.device?.capabilities;
  if (!caps?.modRoutes) { app.say('this firmware has no modulation routes'); return; }
  const buses = [];
  for (let bus = 0; bus < busCount(caps, Domain.CV); bus++) {
    const { writers } = busUsers(app, Domain.CV, bus);
    buses.push({ bus, writers });
  }
  buses.sort((a, b) => Number(Boolean(b.writers.length)) - Number(Boolean(a.writers.length)) || a.bus - b.bus);

  const item = (label, hint, onPick, klass = '') => el('button', {
    type: 'button', class: `param-menu-item ${klass}`, role: 'option',
    onclick: () => { closeMenu(); onPick(); },
  }, el('span', { class: 'param-menu-name' }, label),
     hint ? el('span', { class: 'param-menu-range' }, hint) : null);

  const box = anchor.getBoundingClientRect();
  const menu = el('div', {
    class: 'param-menu', id: 'param-menu', role: 'listbox',
    style: `left:${Math.max(8, Math.min(box.left, (globalThis.innerWidth ?? 9999) - 300))}px; `
         + `top:${box.bottom + 4}px; transform:none`,
  },
    el('div', { class: 'param-menu-head' }, route ? `${name} reads CV bus ${route.bus}` : `modulate ${name} from`),
    el('div', { class: 'param-menu-list' },
      buses.map(({ bus, writers }) => item(
        `CV bus ${bus}`,
        writers.length ? `from ${writers.join(', ')}` : 'nothing writes it',
        () => app.routeParam(index, at, bus),
        route?.bus === bus ? 'chosen' : '')),
      route ? item('remove the route', null, () => app.clearModRoute(route.slot), 'danger') : null));
  document.body.append(menu);
  menu.querySelector('.param-menu-item')?.focus();

  const dismiss = (e) => {
    if (e.type === 'keydown' && e.key !== 'Escape') return;
    if (e.type === 'pointerdown' && menu.contains(e.target)) return;
    closeMenu();
  };
  menu.dismiss = dismiss;
  setTimeout(() => {
    window.addEventListener('pointerdown', dismiss);
    window.addEventListener('keydown', dismiss);
  }, 0);
}

// One menu at a time, whichever button opened it. Shared with the canvas,
// which opens the same kind of menu to finish a modulation drag.
export function closeMenu() {
  const menu = document.getElementById('param-menu');
  if (!menu) return;
  if (menu.dismiss) {
    window.removeEventListener('pointerdown', menu.dismiss);
    window.removeEventListener('keydown', menu.dismiss);
  }
  menu.remove();
}

// A slider a scrolling finger cannot change.
//
// A native range input takes any touch that lands on it: the value jumps to
// where the finger touched down, the page then starts scrolling under it, and
// the gesture ends with a `change` that writes a value nobody chose. On a
// phone, where the whole editor is one long scroll and every parameter has a
// slider across it, that is not an edge case - it is what scrolling the patch
// tab does. The event trace is plain: `pointerdown`, `input` (jumped),
// `pointercancel` (the page took the gesture), `touchend`, `change`.
//
// So a touch has to *claim* the slider before it may move it: either by
// dragging along it - the axis it reads - or by holding still on it for a
// moment, which is a press rather than the start of a swipe. Until then every
// value the input produces is put straight back, and a gesture the browser
// cancels for scrolling puts it back too and commits nothing. A mouse or a
// stylus claims it on contact: neither is trying to scroll the page.
const CLAIM_PX = 8;    // a drag along the slider, far enough not to be a flick
const CLAIM_MS = 250;  // or a press held still, which is not a swipe either

export function slider(attrs, { onInput, onCommit } = {}) {
  const range = el('input', { type: 'range', ...attrs });
  let gesture = null;   // a pointer is down on it: where it started, and whether it has claimed it
  let refuse = false;   // the gesture ended unclaimed, so the `change` it fires is not an edit
  let start = null;     // the value the gesture began from
  const revert = () => { if (start !== null && range.value !== start) range.value = start; };
  const end = () => { if (gesture?.timer) clearTimeout(gesture.timer); gesture = null; };

  range.addEventListener('pointerdown', (e) => {
    start = range.value;
    refuse = false;
    gesture = { x: e.clientX, y: e.clientY, claimed: e.pointerType !== 'touch', timer: 0 };
    if (!gesture.claimed) {
      gesture.timer = setTimeout(() => { if (gesture) gesture.claimed = true; }, CLAIM_MS);
    }
  });
  range.addEventListener('pointermove', (e) => {
    if (!gesture || gesture.claimed) return;
    const dx = Math.abs(e.clientX - gesture.x);
    const dy = Math.abs(e.clientY - gesture.y);
    if (dx >= CLAIM_PX && dx > dy) gesture.claimed = true;
    else if (dy >= CLAIM_PX) revert();          // this is a scroll, not an edit
  });
  // The browser takes the gesture the moment the page scrolls under the
  // finger. Whatever the slider did with it before that is undone.
  range.addEventListener('pointercancel', () => { revert(); refuse = true; end(); });
  range.addEventListener('pointerup', () => { refuse = !gesture?.claimed; end(); });
  range.addEventListener('input', () => {
    if (gesture && !gesture.claimed) { revert(); return; }
    onInput?.(range.value);
  });
  range.addEventListener('change', () => {
    if (refuse) { refuse = false; revert(); return; }
    onCommit?.(range.value);
  });
  return range;
}

// The box a pattern lives in. On a phone its lanes wrap onto as many rows as
// they need and there is nothing here to scroll; on a screen wide enough to
// hold the whole pattern on one line they do that instead, and the leftover -
// a wide grid in a narrow window - scrolls inside this box rather than
// widening the page. It is remembered because the page is rebuilt wholesale on
// every edit: without that, toggling step 20 would scroll the pattern back to
// step 1 and put the next step off the screen.
function scroller(app, key, ...children) {
  return el('div', {
    class: 'lane-scroll', 'data-scroll': key,
    onscroll: (e) => app.scrolled?.set(key, e.target.scrollLeft),
  }, ...children);
}


// A bus selector for one inlet or outlet. The options are only the buses of
// the right domain, because the editor only offers domain-compatible
// connections - the module would refuse anything else.
//
// `none` is what the "on no bus" row says, and `null` leaves it out: a jack's
// bus selector has no such row, because a jack that is on no bus is a jack
// that is *unused*, and that is its direction toggle's word to say. Offering
// it here as well would be a second control for the same thing, and one that
// snapped back - the module refuses a jack in use and on no bus.
function busSelect(caps, domain, value, none, onChange) {
  const select = el('select', { class: `bus bus-${domainName(domain)}`, onchange: (e) => {
    onChange(e.target.value === 'none' ? P.NO_BUS : Number(e.target.value));
  } });
  if (none !== null) {
    const row = el('option', { value: 'none' }, none);
    if (value === P.NO_BUS) row.selected = true;
    select.append(row);
  }
  for (let b = 0; b < busCount(caps, domain); b++) {
    const option = el('option', { value: String(b) }, `${domainName(domain)} bus ${b}`);
    if (b === value) option.selected = true;
    select.append(option);
  }
  return select;
}

// Who is on a bus, in words. A bus *is* the connection, so the thing a patch
// cable would have shown - what this inlet is actually listening to - has to
// be said, or the patch is a list of numbers that happen to match. `self` is
// the port asking, which is left out of its own answer; nothing but a port has
// one, so it is optional.
export function busUsers(app, domain, bus, self = null) {
  const writers = [];
  const readers = [];
  if (bus === P.NO_BUS) return { writers, readers };
  const mine = (isOutlet, index, port) => self && index === self.index
    && Boolean(self.isOutlet) === isOutlet && self.port === port;
  app.patch.nodes.forEach((node, index) => {
    const d = app.device?.byId.get(node.algorithmId);
    if (!d) return;
    for (let i = 0; i < d.nOut && i < P.MAX_OUT; i++) {
      if (node.outBus[i] === bus && d.outDomain[i] === domain && !mine(true, index, i)) {
        writers.push(`${d.name} ${index} ${outletName(d, i)}`);
      }
    }
    for (let i = 0; i < d.nIn && i < P.MAX_IN; i++) {
      if (node.inBus[i] === bus && d.inDomain[i] === domain && !mine(false, index, i)) {
        readers.push(`${d.name} ${index} ${inletName(d, i)}`);
      }
    }
  });
  if (domain === Domain.Gate) {
    app.patch.gatePorts.forEach((port, i) => {
      if (port.bus !== bus) return;
      if (port.direction === P.GatePortDirection.GATE_PORT_IN) writers.push(`jack ${i + 1}`);
      if (port.direction === P.GatePortDirection.GATE_PORT_OUT) readers.push(`jack ${i + 1}`);
    });
  }
  if (domain === Domain.Note) {
    app.patch.midiIn.forEach((port, i) => {
      if (port.sourceMask && port.bus === bus) writers.push(`MIDI in ${i + 1}`);
    });
    app.patch.midiOut.forEach((port, i) => {
      if (port.targetMask && port.bus === bus) readers.push(`MIDI out ${i + 1}`);
    });
  }
  return { writers, readers };
}

function busNeighbours(app, domain, bus, self) {
  if (bus === P.NO_BUS) return null;
  const { writers, readers } = busUsers(app, domain, bus, self);
  const parts = [];
  if (writers.length) parts.push(`from ${writers.join(', ')}`);
  if (readers.length) parts.push(`to ${readers.join(', ')}`);
  if (parts.length) return el('span', { class: 'wire' }, parts.join(' · '));
  // An inlet nobody writes is the warning `advise` raises: the node will read
  // silence. An outlet nobody reads is ordinary - a spare drum lane, an output
  // waiting for a jack - so it is said, not flagged.
  return self.isOutlet
    ? el('span', { class: 'wire' }, 'no reader')
    : el('span', { class: 'wire empty' }, 'no writer');
}

function port(app, index, isOutlet, i) {
  const node = app.patch.nodes[index];
  const d = app.device.byId.get(node.algorithmId);
  const caps = app.device.capabilities;
  const domain = isOutlet ? d.outDomain[i] : d.inDomain[i];
  const bus = isOutlet ? node.outBus[i] : node.inBus[i];
  const name = isOutlet ? outletName(d, i) : inletName(d, i);
  const optional = isOutlet || i >= d.minIn;

  return el('div', { class: `port bus-${domainName(domain)}` },
    el('label', { class: 'port-head' },
      el('span', { class: 'port-name' }, name,
        optional ? null : el('span', { class: 'required' }, '*')),
      busSelect(caps, domain, bus, optional ? 'not connected' : '— must be connected', (chosen) => {
        if (isOutlet) node.outBus[i] = chosen; else node.inBus[i] = chosen;
        app.edit(() => app.device.setConnection(index, isOutlet, i, chosen), 'connection');
        app.render();
      })),
    busNeighbours(app, domain, bus, { index, isOutlet, port: i }));
}

// One node: what it reads, what it writes, and the buses they are on. Buses
// *are* the connections - there is no cable to draw - so each port says what
// it is for and what else is on its bus.
//
// The header is the details panel's when the card is the details panel: the
// name, the index and the remove button sit in the panel's own bar, beside
// the buttons that fold it and put it away, and drawing them twice would put
// two remove buttons on one node.
export function nodeCard(app, index, { header = true } = {}) {
  const patch = app.patch;
  const node = patch.nodes[index];
  const d = app.device.byId.get(node.algorithmId);
  if (!d) return el('section', { class: 'node bad' }, `node ${index}: unknown algorithm ${node.algorithmId}`);

  const inlets = [];
  for (let i = 0; i < d.nIn; i++) inlets.push(port(app, index, false, i));
  const outlets = [];
  for (let i = 0; i < d.nOut; i++) outlets.push(port(app, index, true, i));

  return el('section', { class: 'node', id: `node-${index}` },
    header
      ? el('header', {},
          el('span', { class: 'node-index' }, index),
          el('h3', {}, d.name),
          d.wantsTick ? el('span', { class: 'tag' }, 'clocked') : null,
          iconButton({ icon: 'trash', label: `remove ${d.name} ${index}`, class: 'ghost danger',
                       onclick: () => app.removeNode(index) }))
      : null,
    d.summary ? el('p', { class: 'hint summary' }, d.summary) : null,
    el('div', { class: 'ports' },
      el('div', { class: 'port-group' },
        el('h4', {}, inlets.length ? 'reads' : 'reads nothing'), inlets),
      el('div', { class: 'port-group' },
        el('h4', {}, outlets.length ? 'writes' : 'writes nothing'), outlets)),
    paramPanel(app, index),
    modPanel(app, index),
    gridPanel(app, index));
}

// --- the patch's edges ------------------------------------------------------

// A row of chips, one of which is on: a choice small enough that every option
// can be on screen at once, which is what a two- or three-way setting should
// look like. The MIDI port toggles are the same shape (`portToggles`), so a
// setting that reads as a switch is a switch everywhere in the app.
export function segmented(options, value, onChange, { label }) {
  return el('div', { class: 'ports-row', role: 'group', 'aria-label': label },
    options.map((option) => el('button', {
      type: 'button', class: `chip ${option.value === value ? 'on' : ''}`,
      'aria-pressed': option.value === value ? 'true' : 'false',
      title: option.hint ?? null,
      onclick: () => { if (option.value !== value) onChange(option.value); },
    }, option.label)));
}

// One gate jack, in full: which way it faces, and the gate bus it is on.
//
// **A direction is a setting, not a kind of jack.** It used to be neither: the
// jack had one selector listing every pairing of a direction and a bus ("in →
// gate bus 3"), two rows per bus, so turning a jack round meant finding the
// same bus again in the other half of the list - and the two questions, which
// way and onto what, could not be answered one at a time. They are two
// controls now, and the bus survives the turn.
export function jackCard(app, index) {
  const port = app.patch.gatePorts[index];
  const caps = app.device?.capabilities;
  const used = port.direction !== P.GatePortDirection.GATE_PORT_UNUSED;

  // The toggle and the bus selector are one edit: `planJackDirection` says
  // which bus the jack ends up on, given the one it is being put on, so
  // "turn it round" and "move it" cannot grow two rules that disagree.
  const write = (direction, bus) => {
    const moved = {
      ...app.patch,
      gatePorts: app.patch.gatePorts.map((p, i) => (i === index ? { ...p, bus } : p)),
    };
    const chosen = planJackDirection(moved, caps, index, direction).bus;
    app.patch.gatePorts[index] = { direction, bus: chosen };
    app.edit(() => app.device.setGatePort(index, direction, chosen), 'jack');
    app.render();
  };

  // What else is on its bus. `busUsers` names the jacks too, so this one is
  // taken out of its own answer - the same thing `busNeighbours` does for a
  // node's port, which cannot be reused here because its idea of "me" is a
  // node index and a jack's index is a different number entirely.
  const where = () => {
    if (!used) return null;
    const { writers, readers } = busUsers(app, Domain.Gate, port.bus);
    const me = `jack ${index + 1}`;
    const parts = [];
    const from = writers.filter((w) => w !== me);
    const to = readers.filter((r) => r !== me);
    if (from.length) parts.push(`from ${from.join(', ')}`);
    if (to.length) parts.push(`to ${to.join(', ')}`);
    if (parts.length) return el('span', { class: 'wire' }, parts.join(' · '));
    return port.direction === P.GatePortDirection.GATE_PORT_IN
      ? el('span', { class: 'wire' }, 'no reader')
      : el('span', { class: 'wire empty' }, 'no writer');
  };

  return el('div', { class: `jack-card ${used ? '' : 'unused'}` },
    el('div', { class: 'jack-head' },
      el('h4', {}, `jack ${index + 1}`),
      el('span', { class: 'hint' },
        GATE_DIRECTIONS.find((d) => d.value === port.direction)?.hint ?? '')),
    segmented(GATE_DIRECTIONS, port.direction, (direction) => write(direction, port.bus),
              { label: `jack ${index + 1} direction` }),
    used
      ? el('div', { class: 'port bus-gate' },
          el('label', { class: 'port-head' },
            el('span', { class: 'port-name' },
              port.direction === P.GatePortDirection.GATE_PORT_IN ? 'writes' : 'reads'),
            busSelect(caps, Domain.Gate, port.bus, null, (bus) => write(port.direction, bus))),
          where())
      : null);
}

// What is modulating this node, and how much of it.
//
// A route is made on the canvas - drag a control signal onto a block and pick
// a parameter - but *how* it modulates is not something a drag can say, and
// three of its four fields decide whether the result is a modulation or a
// mess: how deep, around the set point or instead of it, and which way up.
// They live here, beside the parameter they move, rather than in a table of
// routes somewhere else, because "what is happening to this control" is the
// question somebody is asking when they look at it.
//
// Nothing is drawn when nothing is modulating the node: an empty panel on
// every block would be a section heading repeated thirty-two times. A route to
// the clock reaches something that is not a node and so has no block to be
// beside; those live in the modulation table under MIDI control, which lists
// every route whatever it reaches.
function modPanel(app, index) {
  const routes = [];
  (app.patch.modMap ?? []).forEach((route, slot) => {
    if (!route || route.bus === P.NO_BUS) return;
    if (route.targetKind !== P.CcTargetKind.CC_TARGET_NODE || route.targetIndex !== index) return;
    routes.push({ slot, route });
  });
  if (!routes.length) return null;

  return el('div', { class: 'params mod-routes' },
    el('h4', {}, 'modulation'),
    routes.map(({ slot, route }) => modRow(app, slot, route)));
}

function modRow(app, slot, route) {
  const write = (changed) => {
    const next = { ...route, ...changed };
    app.patch.modMap[slot] = next;
    app.edit(() => app.device.setModRoute(slot, next), 'modulation route');
    app.render();
  };
  const mode = route.flags & P.ModFlags.MOD_MODE_MASK;
  const name = modParamName(app.device, app.patch, route);

  const modeSelect = el('select', {
    onchange: (e) => write({
      flags: (route.flags & ~P.ModFlags.MOD_MODE_MASK) | Number(e.target.value),
    }),
  },
    el('option', { value: String(P.ModMode.MOD_OFFSET) }, 'offset'),
    el('option', { value: String(P.ModMode.MOD_ABSOLUTE) }, 'absolute'));
  modeSelect.value = String(mode);

  // Through `slider`, and on commit rather than on input: `write` re-renders,
  // which replaces this element - so writing on every input event would pull
  // the control out from under the finger dragging it. Every other numeric
  // control in this file is built the same way, for the same reason.
  const percent = el('span', { class: 'param-value' },
                     `${Math.round((route.depth ?? 255) * 100 / 255)} %`);
  const depth = slider({
    class: 'slider', min: '0', max: '255', step: '1',
    value: String(route.depth ?? 255),
    'aria-label': `${name} modulation depth`,
  }, {
    onInput: (v) => { percent.textContent = `${Math.round(Number(v) * 100 / 255)} %`; },
    onCommit: (v) => write({ depth: Number(v) }),
  });

  const flag = (bit, label) => {
    const box = el('input', {
      type: 'checkbox', class: 'switch',
      onchange: (e) => write({ flags: e.target.checked ? (route.flags | bit) : (route.flags & ~bit) }),
    });
    box.checked = (route.flags & bit) !== 0;
    return el('label', { class: 'bool' }, box, el('span', {}, label));
  };

  return el('div', { class: 'param mod-route' },
    el('span', { class: 'param-name' }, name),
    el('span', { class: 'param-value dom-CV' }, `CV bus ${route.bus}`),
    el('div', { class: 'param-controls' }, modeSelect, depth, percent),
    el('div', { class: 'param-controls' },
      flag(P.ModFlags.MOD_BIPOLAR, 'bipolar'),
      flag(P.ModFlags.MOD_INVERT, 'invert'),
      iconButton({ icon: 'cut', label: `stop modulating ${name}`, class: 'ghost danger',
                   onclick: () => app.clearModRoute(slot) })));
}

// A control per parameter, drawn from the descriptor: a range for a number, a
// list for an enum, a checkbox for a boolean. An editor cannot draw a control
// for a parameter whose range and meaning it does not know, which is why #20
// exists.
//
// **Grouped by what they do, not by where the firmware put them.** A
// descriptor lists its parameters in the order the algorithm stores them,
// which is the order they were written in, and that order says "root, scale,
// velocity, channel, seed" on one node and "velocity, root, seed, scale" on
// the next. Every node is asked the same few questions - which notes, when,
// how loud, how likely, in what manner - so the card asks them in that order
// on every node, and a hand that has learned where "root" lives on one card
// finds it in the same place on the rest.
export const PARAM_SECTIONS = [
  { key: 'mode', label: 'behaviour' },
  { key: 'pitch', label: 'pitch' },
  { key: 'time', label: 'timing' },
  { key: 'level', label: 'dynamics' },
  { key: 'chance', label: 'chance' },
  { key: 'midi', label: 'MIDI' },
  { key: 'other', label: 'other' },
];

// Which question a parameter answers, from its kind first - a pitch is a
// pitch whatever it is called - and then from its name. The words are the
// ones the firmware's descriptors use; a parameter this table has never met
// lands under "other" rather than being hidden, and is still a control.
const SECTION_WORDS = {
  mode: ['mode', 'direction', 'rule', 'shape', 'priority', 'hold', 'new chord', 'write',
         'link', 'map', 'cycle', 'sync', 'polarity', 'loop', 'retrigger', 'snap', 'fixed', 'quality',
         'voicing', 'inversion', 'diatonic', 'cells'],
  pitch: ['root', 'scale', 'key', 'octave', 'transpose', 'semitone', 'interval', 'degree', 'note',
          'spread', 'range', 'low', 'high', 'fifths', 'leading', 'bass', 'voices', 'tie key', 'rest key',
          'base', 'bend', 'pitch'],
  time: ['length', 'division', 'feel', 'rate', 'gate', 'width', 'delay', 'time', 'phase', 'steps',
         'pulses', 'rotation', 'repeats', 'phrase', 'decay', 'rise', 'fall', 'stall', 'swing', 'tempo',
         'bars', 'beat'],
  level: ['velocity', 'vel ', 'accent', 'curve', 'amount', 'depth', 'offset', 'dry', 'threshold',
          'hysteresis', 'level', 'smooth', 'slew'],
  chance: ['probability', 'chance', 'density', 'deviation', 'cadence', 'gravity', 'drift', 'chaos',
           'revive', 'seed', 'bits', 'edges', 'random'],
  midi: ['channel', 'controller', 'mod src', 'mod cc', 'source', 'cc'],
};

export function paramSection(pd) {
  const name = String(pd.name ?? '').toLowerCase();
  if (pd.kind === P.ParamKind.PARAM_PITCH || pd.kind === P.ParamKind.PARAM_PITCH_CLASS) return 'pitch';
  if (pd.kind === P.ParamKind.PARAM_CHANNEL) return 'midi';
  if (pd.kind === P.ParamKind.PARAM_MILLIS) return 'time';
  for (const section of PARAM_SECTIONS) {
    const words = SECTION_WORDS[section.key];
    if (words?.some((word) => name === word || name.startsWith(word) || name.includes(` ${word}`))) {
      return section.key;
    }
  }
  if (pd.kind === P.ParamKind.PARAM_PERCENT) return 'chance';
  return 'other';
}

// The header parameters of a descriptor, sorted onto the sections above and
// in the firmware's own order inside each. Tables - a sequencer's steps, a
// drum machine's lanes - are not here; they get a grid of their own.
export function paramSections(groups) {
  const sections = new Map(PARAM_SECTIONS.map((s) => [s.key, { ...s, params: [] }]));
  for (const group of groups ?? []) {
    if (!group || group.repeat > 1) continue;
    for (let f = 0; f < group.nFields; f++) {
      const pd = group.fields[f];
      if (!pd || (pd.min === 0 && pd.max === 0)) continue;      // reserved
      sections.get(paramSection(pd)).params.push({ at: group.first + f, pd });
    }
  }
  return [...sections.values()].filter((s) => s.params.length);
}

function paramPanel(app, index) {
  const node = app.patch.nodes[index];
  const d = app.device.byId.get(node.algorithmId);
  if (!d.params) return el('div', { class: 'params loading' }, 'reading parameters…');
  const sections = paramSections(d.params);
  if (!sections.length) return null;
  // One section needs no heading: the heading says what the section is *as
  // opposed to* the others, and there are none.
  const titled = sections.length > 1;
  return el('div', { class: 'param-sections' }, sections.map((section) =>
    el('div', { class: `param-section sec-${section.key}` },
      titled ? el('h4', {}, section.label) : null,
      el('div', { class: 'params' },
        section.params.map(({ at, pd }) => paramControl(app, index, at, pd))))));
}

// What a stored byte means, in the parameter's own terms. Zero means the
// descriptor's default everywhere (param.h), and a signed parameter keeps an
// int8 in the byte, so neither can be shown as the raw number.
export function paramText(pd, stored) {
  const effective = stored === 0 ? pd.def : stored;
  if (pd.kind === P.ParamKind.PARAM_SIGNED) {
    const signed = stored > 127 ? stored - 256 : stored;
    return signed > 0 ? `+${signed}` : String(signed);
  }
  if (pd.kind === P.ParamKind.PARAM_ENUM) return pd.options?.[effective - pd.min] ?? String(effective);
  if (pd.kind === P.ParamKind.PARAM_BOOL) return stored ? 'on' : 'off';
  if (pd.kind === P.ParamKind.PARAM_MILLIS) return `${effective} ms`;
  if (pd.kind === P.ParamKind.PARAM_PERCENT) return `${effective} %`;
  if (pd.kind === P.ParamKind.PARAM_PITCH) return `${noteName(effective)} (${effective})`;
  if (pd.kind === P.ParamKind.PARAM_PITCH_CLASS) return NAMES[effective % 12];
  if (pd.kind === P.ParamKind.PARAM_CHANNEL) return effective === 0 ? 'omni' : `ch ${effective}`;
  if (pd.kind === P.ParamKind.PARAM_BITFIELD) return `0b${effective.toString(2).padStart(8, '0')}`;
  return String(effective);
}

function paramControl(app, index, at, pd) {
  const node = app.patch.nodes[index];
  const value = node.params[at];
  const write = (v) => {
    const clamped = Math.max(0, Math.min(255, v | 0));
    node.params[at] = clamped;
    app.edit(() => app.device.setParam(index, at, clamped), 'parameter');
    app.render();
  };

  const controls = [];
  if (pd.kind === P.ParamKind.PARAM_ENUM) {
    const select = el('select', { class: 'grow', onchange: (e) => write(Number(e.target.value)) });
    for (let v = pd.min; v <= pd.max; v++) {
      const option = el('option', { value: String(v) }, pd.options[v - pd.min] ?? String(v));
      if (v === (value || pd.def)) option.selected = true;
      select.append(option);
    }
    controls.push(select);
  } else if (pd.kind === P.ParamKind.PARAM_BOOL) {
    const box = el('input', { type: 'checkbox', class: 'switch', onchange: (e) => write(e.target.checked ? 1 : 0) });
    box.checked = value !== 0;
    controls.push(el('label', { class: 'bool' }, box, el('span', {}, value ? 'on' : 'off')));
  } else {
    // A slider *and* a number field. A slider is unusable for a precise value
    // and hopeless on a phone, where a 1px drag is a whole step of a 255-wide
    // range; a number field alone loses the sweep. Neither replaces the
    // other, so both write the same parameter, side by side on one row: the
    // slider takes whatever the compact number field and the icon-sized learn
    // button leave it, which is most of the card. The slider is built through
    // `slider`, so a finger scrolling the tab does not write a value.
    const shown = value === 0 ? pd.def : value;
    const number = el('input', {
      type: 'number', class: 'number', min: String(pd.min), max: String(pd.max), step: '1',
      value: String(shown), 'aria-label': `${pd.name}, as a number`, inputmode: 'numeric',
    });
    const range = slider({
      class: 'slider', min: String(pd.min), max: String(pd.max), step: '1',
      value: String(shown), 'aria-label': pd.name,
    }, {
      onInput: (v) => { number.value = v; },
      onCommit: (v) => write(Number(v)),
    });
    number.addEventListener('change', (e) => {
      const v = Math.max(pd.min, Math.min(pd.max, Number(e.target.value) || 0));
      range.value = String(v);
      write(v);
    });
    controls.push(range, number);
  }

  const binding = app.bindingFor?.(index, at);
  const route = app.routeFor?.(index, at);
  return el('div', { class: 'param' },
    el('div', { class: 'param-head' },
      el('span', { class: 'param-name' }, pd.name),
      binding ? el('span', { class: 'param-cc' }, `CC ${binding.cc}`) : null,
      route ? el('span', { class: 'param-cc dom-CV' }, `CV ${route.bus}`) : null,
      el('span', { class: 'param-value' }, paramText(pd, value))),
    el('div', { class: 'param-controls' }, controls,
      learnButton(app, index, at, pd.name, binding),
      cvButton(app, index, at, pd.name, route)));
}

// The generic parameter view is wrong for a sequencer: nobody enters a drum
// pattern as a list of numbers. These are the purpose-built views.
function gridPanel(app, index) {
  const node = app.patch.nodes[index];
  const d = app.device.byId.get(node.algorithmId);
  switch (d.name) {
    case 'StepSequencer':  return stepGrid(app, index);
    case 'DrumSeqGate':
    case 'DrumSeqMidi':    return drumGrid(app, index, d.name === 'DrumSeqMidi');
    case 'NoteSequencer':
    case 'PolySequencer':  return noteLane(app, index, d.name === 'PolySequencer');
    default:               return null;
  }
}

// Steps across, click to toggle. The pattern lives in four bytes as one bit
// per step, so a click is one parameter write.
function stepGrid(app, index) {
  const node = app.patch.nodes[index];
  const length = node.params[0] || 8;
  const cells = [];
  for (let step = 0; step < P.MAX_SEQUENCE_LEN; step++) {
    const byte = 3 + (step >> 3);
    const bit = 1 << (step & 7);
    const on = (node.params[byte] & bit) !== 0;
    cells.push(el('button', {
      // The id is how the running node's position is painted onto the grid
      // every tick without re-rendering it: see perform.js.
      id: `cell-${index}-0-${step}`,
      class: `cell ${on ? 'on' : ''} ${step >= length ? 'beyond' : ''}`,
      title: `step ${step + 1}`,
      onclick: () => {
        node.params[byte] ^= bit;
        const value = node.params[byte];
        app.edit(() => app.device.setParam(index, byte, value), 'step');
        app.render();
      },
    }, ''));
  }
  return el('div', { class: 'grid' },
    el('div', { class: 'grid-title' }, `steps (${length})`),
    scroller(app, `grid-${index}`,
      el('div', { class: 'lanes' },
        el('div', { class: 'lane-row' }, el('div', { class: 'lane' }, cells)))));
}

// Lanes down, steps across, with each lane's own length visible: lanes can
// differ, and that is the polyrhythm the node exists for.
function drumGrid(app, index, isMidi) {
  const node = app.patch.nodes[index];
  const d = app.device.byId.get(node.algorithmId);
  const LANE_BASE = 16, LANE_STRIDE = 8;
  const VELOCITY_BASE = LANE_BASE + P.DRUM_SEQ_LANES * LANE_STRIDE;
  const headerLength = node.params[0] || 16;
  const lanes = [];

  for (let lane = 0; lane < P.DRUM_SEQ_LANES; lane++) {
    const laneAt = LANE_BASE + lane * LANE_STRIDE;
    const ownLength = node.params[laneAt + (isMidi ? 2 : 4)];
    const length = ownLength || headerLength;
    const cells = [];
    for (let step = 0; step < P.MAX_SEQUENCE_LEN; step++) {
      let on, write;
      if (isMidi) {
        const at = VELOCITY_BASE + lane * P.MAX_SEQUENCE_LEN + step;
        on = node.params[at] !== 0;
        write = () => {
          node.params[at] = on ? 0 : 100;
          const value = node.params[at];
          app.edit(() => app.device.setParam(index, at, value), 'step');
        };
      } else {
        const at = laneAt + (step >> 3);
        const bit = 1 << (step & 7);
        on = (node.params[at] & bit) !== 0;
        write = () => {
          node.params[at] ^= bit;
          const value = node.params[at];
          app.edit(() => app.device.setParam(index, at, value), 'step');
        };
      }
      cells.push(el('button', {
        id: `cell-${index}-${lane}-${step}`,
        class: `cell ${on ? 'on' : ''} ${step >= length ? 'beyond' : ''}`,
        title: `lane ${lane + 1}, step ${step + 1}`,
        onclick: () => { write(); app.render(); },
      }, ''));
    }
    // The gate variant has one outlet per lane, so a lane's row says which
    // outlet it is - which is the whole reason the outlets are named.
    const outlet = !isMidi && lane < d.nOut ? outletName(d, lane) : `lane ${lane + 1}`;
    lanes.push(el('div', { class: 'lane-row' },
      el('span', { class: 'lane-name' }, `${outlet} (${length})`),
      el('div', { class: 'lane' }, cells)));
  }
  // Every lane in one box, not one each: on a wide screen eight lanes that
  // scroll separately are eight patterns you cannot read against each other,
  // and the polyrhythm is the whole point of the node. Where they fit on one
  // line the names stay put at the left while the steps move under them
  // (`.lane-name` is sticky); where they do not - a phone - each lane wraps
  // instead and its name sits above it, so no width is spent on the label.
  return el('div', { class: 'grid' },
    el('div', { class: 'grid-title' }, 'pattern'),
    scroller(app, `grid-${index}`, el('div', { class: 'lanes' }, lanes)));
}

// A lane over **scale degrees**, because that is what the sequencer stores.
// The pitch each degree resolves to against the current root and scale is
// shown alongside, and changing the root moves the pitches without touching
// the stored pattern.
function noteLane(app, index, isPoly) {
  const node = app.patch.nodes[index];
  const STEP_BASE = 16;
  const voices = isPoly ? P.NOTE_SEQ_VOICES : 1;
  const stride = voices * 2 + 2;
  const length = node.params[0] || 8;
  const root = node.params[5] || 60;
  const mask = node.params[3] | ((node.params[4] & 0x0f) << 8);

  const rows = [];
  for (let voice = 0; voice < voices; voice++) {
    const cells = [];
    for (let step = 0; step < P.MAX_SEQUENCE_LEN; step++) {
      const at = STEP_BASE + step * stride + voice * 2;
      const degree = node.params[at] > 127 ? node.params[at] - 256 : node.params[at];
      const velocity = node.params[at + 1];
      const flags = node.params[STEP_BASE + step * stride + voices * 2];
      const rest = (flags & 0x20) !== 0;
      const tie = (flags & 0x40) !== 0;
      const pitch = velocity ? root + degreeToSemitone(degree, mask) : null;
      cells.push(el('div', {
        id: voice === 0 ? `cell-${index}-0-${step}` : null,
        class: `note-cell ${velocity ? 'on' : ''} ${step >= length ? 'beyond' : ''}`
             + `${rest ? ' rest' : ''}${tie ? ' tie' : ''}`,
        title: pitch === null ? `step ${step + 1}: silent` : `step ${step + 1}: degree ${degree} → ${noteName(pitch)}`,
      },
        el('input', {
          type: 'number', value: String(degree), min: '-64', max: '63', inputmode: 'numeric',
          'aria-label': `step ${step + 1} degree`,
          onchange: (e) => {
            const v = Number(e.target.value) & 0xff;
            node.params[at] = v;
            if (!node.params[at + 1]) node.params[at + 1] = 100;
            const degreeValue = node.params[at];
            const velocityValue = node.params[at + 1];
            app.edit(async () => {
              await app.device.setParam(index, at, degreeValue);
              await app.device.setParam(index, at + 1, velocityValue);
            }, 'step');
            app.render();
          },
        }),
        el('span', { class: 'pitch' }, pitch === null ? '·' : noteName(pitch))));
    }
    rows.push(el('div', { class: 'lane-row' },
      el('span', { class: 'lane-name' }, isPoly ? `voice ${voice + 1}` : 'notes'),
      el('div', { class: 'lane notes' }, cells)));
  }
  return el('div', { class: 'grid' },
    el('div', { class: 'grid-title' }, `degrees · root ${noteName(root)}`),
    scroller(app, `grid-${index}`, el('div', { class: 'lanes' }, rows)));
}

// Mirrors midi/scale.h so the displayed pitch is the one the module will play.
function scaleIntervals(mask) {
  const bits = (mask & 0x0fff) || 0x0fff;
  const out = [];
  for (let i = 0; i < 12; i++) if (bits & (1 << i)) out.push(i);
  return out;
}
export function degreeToSemitone(degree, mask) {
  const intervals = scaleIntervals(mask);
  const n = intervals.length;
  let octave = Math.floor(degree / n);
  let index = degree % n;
  if (index < 0) index += n;
  return octave * 12 + intervals[index];
}
const NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
export function noteName(pitch) {
  if (pitch < 0 || pitch > 127) return '—';
  return `${NAMES[pitch % 12]}${Math.floor(pitch / 12) - 1}`;
}
