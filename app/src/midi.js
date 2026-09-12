// MIDI: what comes in, what goes out, what the clock follows, and every
// controller bound to a parameter.
//
// **Learn is not a mapping interface.** Arming a learn and turning a knob is
// the fastest way to bind a controller you have in front of you, and it is the
// only way to bind one whose CC number you do not know. It is also the only
// thing the app offered, which meant a binding could not be read, changed,
// narrowed to a range, moved to another parameter or deleted - and could not
// be made at all without the hardware present. A patch built offline for a
// controller that is in the next room is exactly the case the `.syx` export
// exists for, and the bindings were the one part of the patch it could not
// build.
//
// So the table is shown as a table, every field is editable, and learn is one
// button on a row rather than the whole feature.

import * as P from './protocol.js';
import { el, busUsers, iconButton } from './views.js';
import { busCount, Domain, domainName } from './validate.js';
import { BlockKind } from './graph.js';
import { describeSupport } from './webmidi.js';
import {
  MUSICAL_PORTS, portNames, CLOCK_SOURCES, SWAP_TIMINGS,
  CC_TARGET_KINDS, CLOCK_TARGETS, TRANSPORT_TARGETS,
  TAKEOVER, RELATIVE, FOURTEEN_BIT, PASS_THROUGH, channelLabel,
} from './names.js';

// A disclosure whose open state outlives a re-render: the app holds it, keyed,
// because the DOM is rebuilt under it on every edit.
function disclosure(app, key, summaryContent, ...children) {
  const node = el('details', { class: 'binding' }, el('summary', {}, summaryContent), ...children);
  node.open = app.isOpen(key);
  node.addEventListener('toggle', () => app.setOpen(key, node.open));
  return node;
}

const emptyMapping = () => ({
  sourceMask: 0, channel: 0, cc: 1, targetKind: P.CcTargetKind.CC_TARGET_NODE,
  targetIndex: 0, param: 0, min: 0, max: 0, flags: 0,
});

// A channel selector: 0 is omni, which is a word rather than a number nobody
// can be expected to know.
function channelSelect(value, onChange, { omni = 'omni (any channel)' } = {}) {
  const select = el('select', { onchange: (e) => onChange(Number(e.target.value)) });
  for (let c = 0; c <= 16; c++) {
    const option = el('option', { value: String(c) }, c === 0 ? omni : `channel ${c}`);
    if (c === value) option.selected = true;
    select.append(option);
  }
  return select;
}

function busSelect(caps, domain, value, onChange, noneLabel = 'not connected') {
  const select = el('select', { class: `bus bus-${domainName(domain)}`,
                                onchange: (e) => onChange(e.target.value === 'none' ? P.NO_BUS : Number(e.target.value)) });
  const none = el('option', { value: 'none' }, noneLabel);
  if (value === P.NO_BUS) none.selected = true;
  select.append(none);
  for (let b = 0; b < busCount(caps, domain); b++) {
    const option = el('option', { value: String(b) }, `${domainName(domain)} bus ${b}`);
    if (b === value) option.selected = true;
    select.append(option);
  }
  return select;
}

// The port mask, as a row of toggles. The control cable is not offered: it is
// reserved for the protocol, and a patch that could route music onto it - or
// take it away from the app - is a patch that could lock the module out.
function portToggles(mask, onChange, { label }) {
  return el('div', { class: 'ports-row', role: 'group', 'aria-label': label },
    MUSICAL_PORTS.map((p) => {
      const on = (mask & p.value) !== 0;
      return el('button', {
        class: `chip ${on ? 'on' : ''}`, type: 'button',
        onclick: () => onChange(on ? (mask & ~p.value) : (mask | p.value)),
      }, p.label);
    }),
    el('button', {
      class: 'chip ghost', type: 'button',
      onclick: () => onChange(mask === MUSICAL_PORTS.reduce((m, p) => m | p.value, 0)
        ? 0 : MUSICAL_PORTS.reduce((m, p) => m | p.value, 0)),
    }, 'all'));
}

// --- routing ---------------------------------------------------------------

// One MIDI port, in full. The routing panel is a list of these and the canvas
// inspector shows the one whose block was clicked, so a port edited from
// either place is edited by the same code.
//
// **A port is a source and a destination, and the card says which is which.**
// The two halves are the same two controls whichever way the port faces - a
// set of cables with a channel, and a note bus - so a card that only put them
// in a different order read, on half of the panel, as "these cables receive
// this bus". They are labelled `from` and `to` instead, and the signal runs
// down the card in that order: an output is a note bus at the top playing
// cables at the bottom.
//
// **Which way it faces is not a control here.** The panel is two lists under
// two headings, and telling a port in the "inputs" list that it is an input is
// a row of chips that says nothing. Turning one round is an action on the port
// - it moves to the other list, which is where it has gone - and it sits with
// the other two: fanning the port out, and putting it away.
export function routeCard(app, index, isOut, { header = true } = {}) {
  const caps = app.device?.capabilities;
  if (!caps) return null;
  const port = (isOut ? app.patch.midiOut : app.patch.midiIn)[index];
  if (!port) return null;
  const what = isOut ? 'MIDI out' : 'MIDI in';
  const title = `${isOut ? 'out' : 'in'} ${index + 1}`;
  const mask = () => (isOut ? port.targetMask : port.sourceMask);
  const send = () => app.edit(
    () => app.device.setMidiPort(index, isOut, mask(), port.channel, port.bus), what);

  const cables = portToggles(mask(), (chosen) => {
    if (isOut) port.targetMask = chosen; else port.sourceMask = chosen;
    send();
    if (!chosen) app.status = `${what} ${index + 1} is unused`;
    app.render();
  }, { label: `${what} ${index + 1} ${isOut ? 'targets' : 'sources'}` });

  const channel = channelSelect(port.channel, (chosen) => {
    port.channel = chosen;
    send();
    app.render();
  }, isOut ? { omni: 'keep each event\u2019s channel' } : {});

  const bus = busSelect(caps, Domain.Note, port.bus, (chosen) => {
    port.bus = chosen;
    send();
    app.render();
  }, 'no bus');

  // The `from` and `to` beside them are the only thing naming these two
  // controls, and a word on the screen is not a label: said here so a screen
  // reader gets what the eye gets.
  channel.setAttribute('aria-label', isOut
    ? `${what} ${index + 1} sends on`
    : `${what} ${index + 1} accepts`);
  bus.setAttribute('aria-label', `${what} ${index + 1} note bus`);

  const leg = (label, ...controls) => el('div', { class: 'route-leg' },
    el('span', { class: 'leg-name' }, label),
    el('div', { class: 'leg-body' }, ...controls));

  const actions = [
    iconButton({ icon: 'copy', class: 'ghost',
                 label: isOut
                   ? `play this note bus down another cable too`
                   : `feed another note bus from ${what} ${index + 1}`,
                 onclick: () => app.fanOutMidiPort(index, isOut) }),
    iconButton({ icon: 'flip', class: 'ghost',
                 label: `turn ${what} ${index + 1} round`,
                 onclick: () => app.flipMidiPort(index, isOut) }),
    header
      ? iconButton({ icon: 'trash', class: 'ghost danger',
                     label: `stop using ${what} ${index + 1}`,
                     onclick: () => app.removeBlock(
                       { kind: isOut ? BlockKind.MidiOut : BlockKind.MidiIn, index }) })
      : null,
  ];

  return el('div', { class: 'route' },
    el('div', { class: 'route-head' },
      header ? el('h4', {}, title) : null,
      el('div', { class: 'route-actions' }, actions)),
    isOut
      ? [leg('from', bus), leg('to', cables, channel)]
      : [leg('from', cables, channel), leg('to', bus)],
    busLine(app, index, isOut, port));
}

// The rest of the note bus, in the port's own terms. A MIDI port is one end of
// a bus and the card shows only that end, so the far end - and, when there is
// one, the company it keeps at this end - is the one thing it cannot say by
// showing its own settings.
//
// Which is which depends on the direction, and not by the same rule as the
// card's two legs: an input *writes* the bus, so the other writers are ports
// doing what it does and the readers are where its notes end up; an output
// reads it, so it is the other way about. Naming that is what makes a fan-out
// visible - "shared with MIDI out 2" is the second cable, said where the
// person who made it is looking.
function busLine(app, index, isOut, port) {
  if (port.bus === P.NO_BUS) return el('span', { class: 'wire empty' }, 'on no bus: off');
  const me = `${isOut ? 'MIDI out' : 'MIDI in'} ${index + 1}`;
  const { writers, readers } = busUsers(app, Domain.Note, port.bus);
  const far = (isOut ? writers : readers).filter((who) => who !== me);
  const beside = (isOut ? readers : writers).filter((who) => who !== me);

  const parts = [];
  if (far.length) parts.push(`${isOut ? 'from' : 'to'} ${far.join(', ')}`);
  else parts.push(isOut ? `nothing writes note bus ${port.bus}` : `nothing reads note bus ${port.bus} yet`);
  if (beside.length) parts.push(`shared with ${beside.join(', ')}`);
  return el('span', { class: `wire ${far.length ? '' : 'empty'}` }, parts.join(' \u00b7 '));
}

// The ports a patch is using, and a way to take one more into use.
//
// **An unused port is not drawn.** The module has four each way, and eight
// cards - six of them empty, each with its own eight cables and two selectors
// - was most of a phone screen spent saying "no". A port is added when it is
// wanted and goes back to unused when the last cable is turned off, the same
// way a jack does.
function routeSide(app, isOut) {
  const caps = app.device.capabilities;
  const slots = (isOut ? app.patch.midiOut : app.patch.midiIn).slice(0, isOut ? caps.midiOut : caps.midiIn);
  const used = slots.map((port, index) => ({ port, index }))
    .filter(({ port }) => (isOut ? port.targetMask : port.sourceMask));

  return el('div', { class: 'route-side' },
    el('div', { class: 'route-side-head' },
      el('h3', {}, isOut ? 'outputs' : 'inputs'),
      el('span', { class: 'hint' }, `${used.length}/${slots.length}`),
      used.length < slots.length
        ? iconButton({ icon: 'plus', class: 'ghost',
                       label: `add a MIDI ${isOut ? 'output' : 'input'}`,
                       onclick: () => app.addMidiPort(isOut) })
        : null),
    used.length
      ? used.map(({ index }) => routeCard(app, index, isOut))
      : el('p', { class: 'hint' }, isOut ? 'nothing is played out' : 'nothing is taken in'));
}

export function routingPanel(app) {
  if (!app.device?.capabilities) return null;
  return el('section', { class: 'panel' },
    el('h2', {}, 'MIDI routing'),
    el('div', { class: 'routes' }, routeSide(app, false), routeSide(app, true)));
}

// --- clock, Program Change, NRPN -------------------------------------------
//
// The key used to be here too, and is not: it is not a MIDI setting. See
// src/key.js.

export function globalsPanel(app) {
  if (!app.device?.capabilities) return null;
  const g = app.globals;
  const push = () => { app.edit(() => app.device.setGlobals(g), 'settings'); app.render(); };

  const source = el('select', { onchange: (e) => { g.clockSource = Number(e.target.value); push(); } });
  for (const s of CLOCK_SOURCES) {
    const option = el('option', { value: String(s.value) }, s.label);
    if (s.value === g.clockSource) option.selected = true;
    source.append(option);
  }

  const bpm = el('input', {
    type: 'number', class: 'number wide', min: String(P.CLOCK_MIN_BPM), max: String(P.CLOCK_MAX_BPM),
    value: String(g.bpm), inputmode: 'numeric', 'aria-label': 'tempo in BPM',
    onchange: (e) => {
      g.bpm = Math.max(P.CLOCK_MIN_BPM, Math.min(P.CLOCK_MAX_BPM, Number(e.target.value) || P.CLOCK_DEFAULT_BPM));
      push();
    },
  });

  const ppqn = el('input', {
    type: 'number', class: 'number wide', min: '1', max: '96', value: String(g.cvPpqn),
    inputmode: 'numeric', 'aria-label': 'CV pulses per quarter note',
    onchange: (e) => { g.cvPpqn = Math.max(1, Math.min(96, Number(e.target.value) || 4)); push(); },
  });

  const pcEnabled = el('input', { type: 'checkbox', class: 'switch',
    onchange: (e) => { g.pcEnabled = e.target.checked ? 1 : 0; push(); } });
  pcEnabled.checked = g.pcEnabled !== 0;

  const quantise = el('select', { onchange: (e) => { g.pcQuantise = Number(e.target.value); push(); } });
  for (const t of SWAP_TIMINGS) {
    const option = el('option', { value: String(t.value) }, t.label);
    if (t.value === g.pcQuantise) option.selected = true;
    quantise.append(option);
  }

  const nrpnEnabled = el('input', { type: 'checkbox', class: 'switch',
    onchange: (e) => {
      g.nrpnEnabled = e.target.checked ? 1 : 0;
      app.edit(() => app.device.setNrpn(g.nrpnEnabled, g.nrpnChannel, g.nrpnSourceMask), 'NRPN');
      app.render();
    } });
  nrpnEnabled.checked = g.nrpnEnabled !== 0;

  const field = (name, control, hint) => el('div', { class: 'field' },
    el('span', { class: 'field-name' }, name), control,
    hint ? el('span', { class: 'hint' }, hint) : null);

  return el('section', { class: 'panel' },
    el('h2', {}, 'clock and recall'),
    el('div', { class: 'fields' },
      field('clock source', source),
      field('tempo', bpm, `${P.CLOCK_MIN_BPM}–${P.CLOCK_MAX_BPM} BPM`),
      field('CV pulses per quarter', ppqn, null),
      field('Program Change recalls presets', el('label', { class: 'bool' }, pcEnabled,
        el('span', {}, g.pcEnabled ? 'on' : 'off')), null),
      field('recall listens on', channelSelect(g.pcChannel, (c) => { g.pcChannel = c; push(); }), null),
      field('recall lands', quantise, null)),
    el('div', { class: 'fields' },
      field('from these ports', portToggles(g.pcSourceMask, (mask) => { g.pcSourceMask = mask; push(); },
        { label: 'Program Change source ports' }), 'none = any')),
    el('h2', { class: 'spaced' }, 'NRPN'),
    el('div', { class: 'fields' },
      field('accept NRPN', el('label', { class: 'bool' }, nrpnEnabled,
        el('span', {}, g.nrpnEnabled ? 'on' : 'off'))),
      field('on channel', channelSelect(g.nrpnChannel, (c) => {
        g.nrpnChannel = c;
        app.edit(() => app.device.setNrpn(g.nrpnEnabled, g.nrpnChannel, g.nrpnSourceMask), 'NRPN');
        app.render();
      }))),
    el('div', { class: 'fields' },
      field('from these ports', portToggles(g.nrpnSourceMask, (mask) => {
        g.nrpnSourceMask = mask;
        app.edit(() => app.device.setNrpn(g.nrpnEnabled, g.nrpnChannel, g.nrpnSourceMask), 'NRPN');
        app.render();
      }, { label: 'NRPN source ports' }), 'none = any')));
}

// --- an external controller ------------------------------------------------

// A controller plugged into the *computer*, playing the module in the page.
//
// This is what makes the app an instrument rather than a form: the keys and
// knobs on the desk reach the patch being edited, on a chosen port and its own
// channel, through the same MIDI input a cable would use. Learn works from it,
// because an incoming CC takes the path main.cpp gives it before anything else
// sees it - so a binding can be made with a real knob and no module.
export function controllerPanel(app) {
  const controller = app.controller;
  const support = describeSupport();

  if (!app.usingModule) {
    return el('section', { class: 'panel' },
      el('h2', {}, 'external controller'),
      el('p', { class: 'hint' }, 'plug it into the module on the cable'));
  }
  if (!support.ok) {
    return el('section', { class: 'panel' },
      el('h2', {}, 'external controller'),
      el('p', { class: 'hint' }, support.reason));
  }
  if (!controller.access) {
    return el('section', { class: 'panel' },
      el('h2', {}, 'external controller'),
      el('div', { class: 'row' },
        el('button', { onclick: () => app.connectController() }, 'find my MIDI devices')));
  }

  const inputs = el('select', { class: 'grow', onchange: (e) => { app.controller.listenTo(e.target.value); app.render(); } });
  inputs.append(el('option', { value: '' }, 'nothing'));
  for (const port of controller.inputs) {
    const option = el('option', { value: port.id }, port.name);
    if (port.id === controller.inputId) option.selected = true;
    inputs.append(option);
  }

  const outputs = el('select', { class: 'grow', onchange: (e) => { app.controller.sendTo(e.target.value); app.render(); } });
  outputs.append(el('option', { value: '' }, 'nothing'));
  for (const port of controller.outputs) {
    const option = el('option', { value: port.id }, port.name);
    if (port.id === controller.outputId) option.selected = true;
    outputs.append(option);
  }

  const arrives = el('select', { onchange: (e) => { app.controller.setPort(Number(e.target.value)); app.render(); } });
  for (const port of MUSICAL_PORTS) {
    const option = el('option', { value: String(port.value) }, port.label);
    if (port.value === controller.port) option.selected = true;
    arrives.append(option);
  }

  const field = (name, ...controls) => el('div', { class: 'field' },
    el('span', { class: 'field-name' }, name), ...controls);

  return el('section', { class: 'panel' },
    el('h2', {}, 'external controller'),
    el('div', { class: 'fields' },
      field('play the module from', inputs),
      field('arriving on', arrives),
      field('send what it plays to', outputs)),
    controller.inputId || controller.outputId
      ? el('p', { class: 'hint', id: 'controller-activity' }, '')
      : null);
}

// --- controller bindings ---------------------------------------------------

// One binding in words. This is the summary that did not exist: a table of
// bindings a user can read without turning every knob to find out what moves.
export function describeMapping(app, m) {
  if (!m || !m.sourceMask) return null;
  const where = m.sourceMask ? portNames(m.sourceMask).join(', ') : 'any port';
  const from = `CC ${m.cc}, ${channelLabel(m.channel)}, from ${where || 'any port'}`;
  return { from, to: describeTarget(app, m), range: describeRange(app, m) };
}

// Shared by a controller binding and a modulation route: they reach the same
// target space, so "what does this move" has to read the same for both.
export function describeTarget(app, m) {
  switch (m.targetKind) {
    case P.CcTargetKind.CC_TARGET_NODE: {
      const node = app.patch.nodes[m.targetIndex];
      if (!node) return `node ${m.targetIndex} (not in this patch)`;
      const d = app.device?.byId.get(node.algorithmId);
      const pd = app.device?.describeParam(node.algorithmId, m.param);
      return `${d?.name ?? `algorithm ${node.algorithmId}`} ${m.targetIndex} · ${pd?.name ?? `parameter ${m.param}`}`;
    }
    case P.CcTargetKind.CC_TARGET_CLOCK:
      return `clock · ${CLOCK_TARGETS.find((t) => t.value === m.param)?.label ?? m.param}`;
    case P.CcTargetKind.CC_TARGET_TRANSPORT:
      return `transport · ${TRANSPORT_TARGETS.find((t) => t.value === m.param)?.label ?? m.param}`;
    default:
      return 'a target kind this firmware does not have';
  }
}

function describeRange(app, m) {
  if (m.min === 0 && m.max === 0) return 'full range';
  return `${m.min}–${m.max}`;
}

export function mappingPanel(app) {
  const caps = app.device?.capabilities;
  const slots = app.patch.ccMap;
  const used = slots.map((m, slot) => ({ m, slot })).filter(({ m }) => m && m.sourceMask);

  const summary = used.length
    ? el('div', { class: 'table-scroll' }, el('table', { class: 'bindings' },
        el('thead', {}, el('tr', {},
          el('th', {}, 'slot'), el('th', {}, 'controller'), el('th', {}, 'moves'), el('th', {}, 'over'), el('th', {}, ''))),
        el('tbody', {}, used.map(({ m, slot }) => {
          const described = describeMapping(app, m);
          return el('tr', {},
            el('td', {}, String(slot)),
            el('td', {}, described.from),
            el('td', {}, described.to),
            el('td', {}, described.range),
            el('td', {}, el('button', { class: 'ghost danger', onclick: () => app.clearMapping(slot) }, 'clear')));
        }))))
    : el('p', { class: 'hint' }, 'nothing bound');

  const editors = slots.map((m, slot) => mappingEditor(app, slot, m)).filter(Boolean);

  return el('section', { class: 'panel' },
    el('h2', {}, 'MIDI control'),
    el('p', { class: 'hint' }, `${used.length}/${slots.length} slots`),
    summary,
    app.learnTarget
      ? el('div', { class: 'notes' },
          el('h4', {}, 'waiting for a controller'),
          el('p', {}, describeTarget(app, {
            targetKind: P.CcTargetKind.CC_TARGET_NODE,
            targetIndex: app.learnTarget.nodeIndex,
            param: app.learnTarget.param,
          })),
          el('button', { onclick: () => app.cancelLearn() }, 'cancel'))
      : null,
    (() => {
      const all = el('details', { class: 'bindings-detail' },
        el('summary', {}, `all slots (${slots.length})`),
        el('div', { class: 'binding-list' }, editors));
      all.open = app.isOpen('bindings');
      all.addEventListener('toggle', () => app.setOpen('bindings', all.open));
      return all;
    })(),
    caps ? null : el('p', { class: 'hint' }, 'no module'));
}

// --- modulation routes ------------------------------------------------------
//
// Routes are made on the canvas and their depth and mode are edited beside the
// parameter they move, which is where somebody asking "what is happening to
// this control" is looking. This is the other half: every route in one table,
// including the ones that have nowhere to be drawn - a route to the clock's
// tempo reaches something that is not a node, so no block carries it, and
// without this it would be in the patch and invisible.
export function modulationPanel(app) {
  const slots = app.patch.modMap ?? [];
  const limit = app.device?.capabilities?.modRoutes;
  const used = slots.map((r, slot) => ({ r, slot })).filter(({ r }) => r && r.bus !== P.NO_BUS);
  if (!used.length && !limit) return null;

  const summary = used.length
    ? el('div', { class: 'table-scroll' }, el('table', { class: 'bindings' },
        el('thead', {}, el('tr', {},
          el('th', {}, 'slot'), el('th', {}, 'signal'), el('th', {}, 'moves'),
          el('th', {}, 'how'), el('th', {}, ''))),
        el('tbody', {}, used.map(({ r, slot }) => el('tr', {},
          el('td', {}, String(slot)),
          el('td', {}, `CV bus ${r.bus}`),
          el('td', {}, describeTarget(app, r)),
          el('td', {}, [
            (r.flags & P.ModFlags.MOD_MODE_MASK) === P.ModMode.MOD_OFFSET ? 'offset' : 'absolute',
            `${Math.round((r.depth ?? 255) * 100 / 255)} %`,
            r.flags & P.ModFlags.MOD_BIPOLAR ? 'bipolar' : 'unipolar',
            r.flags & P.ModFlags.MOD_INVERT ? 'inverted' : null,
            r.min === 0 && r.max === 0 ? 'full range' : `${r.min}–${r.max}`,
          ].filter(Boolean).join(' · ')),
          el('td', {}, el('button', {
            class: 'ghost danger', onclick: () => app.clearModRoute(slot),
          }, 'clear')))))))
    : el('p', { class: 'hint' }, 'nothing modulated');

  return el('section', { class: 'panel' },
    el('h2', {}, 'modulation'),
    el('p', { class: 'hint' }, `${used.length}/${limit ?? slots.length} routes`),
    summary);
}

function mappingEditor(app, slot, existing) {
  const m = existing ?? emptyMapping();
  const active = Boolean(existing && existing.sourceMask);
  const push = () => {
    app.patch.ccMap[slot] = m;
    app.edit(() => app.device.setCcMap(slot, m), 'binding');
    app.render();
  };

  const number = (value, min, max, label, onChange) => el('input', {
    type: 'number', class: 'number wide', min: String(min), max: String(max),
    value: String(value), inputmode: 'numeric', 'aria-label': label,
    onchange: (e) => onChange(Math.max(min, Math.min(max, Number(e.target.value) || 0))),
  });

  const kind = el('select', { onchange: (e) => {
    m.targetKind = Number(e.target.value);
    m.param = 0;
    m.targetIndex = 0;
    push();
  } });
  for (const k of CC_TARGET_KINDS) {
    const option = el('option', { value: String(k.value) }, k.label);
    if (k.value === m.targetKind) option.selected = true;
    kind.append(option);
  }

  const targetFields = [];
  if (m.targetKind === P.CcTargetKind.CC_TARGET_NODE) {
    const nodePick = el('select', { onchange: (e) => { m.targetIndex = Number(e.target.value); m.param = 0; push(); } });
    app.patch.nodes.forEach((node, index) => {
      const d = app.device?.byId.get(node.algorithmId);
      const option = el('option', { value: String(index) }, `${index} · ${d?.name ?? node.algorithmId}`);
      if (index === m.targetIndex) option.selected = true;
      nodePick.append(option);
    });
    if (!app.patch.nodes.length) nodePick.append(el('option', {}, 'no nodes in this patch'));

    const paramPick = el('select', { onchange: (e) => { m.param = Number(e.target.value); push(); } });
    const node = app.patch.nodes[m.targetIndex];
    const d = node ? app.device?.byId.get(node.algorithmId) : null;
    for (const group of d?.params ?? []) {
      if (!group || group.repeat > 1) continue;              // tables are not knob targets
      for (let f = 0; f < group.nFields; f++) {
        const pd = group.fields[f];
        if (!pd || (pd.min === 0 && pd.max === 0)) continue;
        const at = group.first + f;
        const option = el('option', { value: String(at) }, `${pd.name} (${pd.min}–${pd.max})`);
        if (at === m.param) option.selected = true;
        paramPick.append(option);
      }
    }
    targetFields.push(nodePick, paramPick);
  } else if (m.targetKind === P.CcTargetKind.CC_TARGET_CLOCK
          || m.targetKind === P.CcTargetKind.CC_TARGET_TRANSPORT) {
    const list = m.targetKind === P.CcTargetKind.CC_TARGET_CLOCK ? CLOCK_TARGETS : TRANSPORT_TARGETS;
    const pick = el('select', { onchange: (e) => { m.param = Number(e.target.value); push(); } });
    for (const t of list) {
      const option = el('option', { value: String(t.value) }, t.label);
      if (t.value === m.param) option.selected = true;
      pick.append(option);
    }
    targetFields.push(pick);
  }

  const takeover = el('select', { onchange: (e) => {
    m.flags = (m.flags & ~TAKEOVER.mask) | Number(e.target.value);
    push();
  } });
  for (const t of TAKEOVER.options) {
    const option = el('option', { value: String(t.value) }, t.label);
    if ((m.flags & TAKEOVER.mask) === t.value) option.selected = true;
    takeover.append(option);
  }

  const relative = el('select', { onchange: (e) => {
    m.flags = (m.flags & ~RELATIVE.mask) | Number(e.target.value);
    push();
  } });
  for (const t of RELATIVE.options) {
    const option = el('option', { value: String(t.value) }, t.label);
    if ((m.flags & RELATIVE.mask) === t.value) option.selected = true;
    relative.append(option);
  }

  const toggle = (bit, label) => {
    const box = el('input', { type: 'checkbox', class: 'switch',
      onchange: (e) => { m.flags = e.target.checked ? (m.flags | bit) : (m.flags & ~bit); push(); } });
    box.checked = (m.flags & bit) !== 0;
    return el('label', { class: 'bool' }, box, el('span', {}, label));
  };

  const field = (name, ...controls) => el('div', { class: 'field' },
    el('span', { class: 'field-name' }, name), ...controls);

  const editor = disclosure(app, `binding-${slot}`,
    [el('span', { class: 'slot-index' }, slot),
     active
       ? el('span', {}, `CC ${m.cc} ${channelLabel(m.channel)} → ${describeTarget(app, m)}`)
       : el('span', { class: 'hint' }, 'empty')],
    el('div', { class: 'fields' },
      field('CC number', number(m.cc, 0, 119, `binding ${slot} CC number`, (v) => { m.cc = v; push(); })),
      field('channel', channelSelect(m.channel, (c) => { m.channel = c; push(); })),
      field('controls', kind, ...targetFields)),
    el('div', { class: 'fields' },
      field('listens on', portToggles(m.sourceMask, (mask) => { m.sourceMask = mask; push(); },
        { label: `binding ${slot} source ports` }))),
    el('div', { class: 'fields' },
      field('sweeps from', number(m.min, 0, 16383, `binding ${slot} low end`, (v) => { m.min = v; push(); })),
      field('to', number(m.max, 0, 16383, `binding ${slot} high end`, (v) => { m.max = v; push(); }),
        el('span', { class: 'hint' }, 'both zero: full range')),
      field('knob catches up by', takeover),
      field('the controller sends', relative)),
    el('div', { class: 'fields' },
      field('', toggle(FOURTEEN_BIT, '14-bit')),
      field('', toggle(PASS_THROUGH, 'pass the CC on'))),
    el('div', { class: 'row' },
      el('button', { onclick: () => app.learnInto(slot, m) }, 'learn'),
      active ? el('button', { class: 'danger', onclick: () => app.clearMapping(slot) }, 'clear') : null,
      !m.sourceMask
        ? el('span', { class: 'hint' }, 'no source port: off')
        : null));
  if (active) editor.classList.add('active');
  return editor;
}
