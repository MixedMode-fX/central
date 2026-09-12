// MIDI: what comes in, what goes out, what the clock follows, and the
// controller plugged into this computer.
//
// **What a controller *moves* is not here.** A binding is part of the patch,
// like a modulation route, so both live in the mod matrix on the patch tab
// (`modmatrix.js`). What is left is the room: the cables, the channels, the
// clock and Program Change recall, none of which a patch travels with.

import * as P from './protocol.js';
import { el, busUsers, iconButton } from './views.js';
import { busCount, Domain, domainName } from './validate.js';
import { BlockKind } from './graph.js';
import { describeSupport } from './webmidi.js';
import { MUSICAL_PORTS, CLOCK_SOURCES, SWAP_TIMINGS } from './names.js';

// A channel selector: 0 is omni, which is a word rather than a number nobody
// can be expected to know. Shared with the mod matrix, which asks the same
// question about a binding: a channel is a MIDI control wherever it is edited.
export function channelSelect(value, onChange, { omni = 'omni (any channel)' } = {}) {
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
export function portToggles(mask, onChange, { label }) {
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
