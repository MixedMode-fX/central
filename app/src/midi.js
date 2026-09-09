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
import { el } from './views.js';
import { busCount, Domain, domainName } from './validate.js';
import { describeSupport } from './webmidi.js';
import {
  MUSICAL_PORTS, portNames, CLOCK_SOURCES, SWAP_TIMINGS, KEY_SCALES, PITCH_CLASSES,
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

// One MIDI port, in full: what it accepts or sends, on which channel, and the
// note bus it copies to or from. The routing panel is a grid of these and the
// canvas inspector shows the one whose block was clicked, so a port edited
// from either place is edited by the same code.
export function routeCard(app, index, isOut) {
  const caps = app.device?.capabilities;
  if (!caps) return null;
  const port = (isOut ? app.patch.midiOut : app.patch.midiIn)[index];
  if (!port) return null;
  const what = isOut ? 'MIDI out' : 'MIDI in';
  const mask = () => (isOut ? port.targetMask : port.sourceMask);
  const send = () => app.edit(
    () => app.device.setMidiPort(index, isOut, mask(), port.channel, port.bus), what);

  return el('div', { class: 'route' },
    el('div', { class: 'route-head' },
      el('h4', {}, `${isOut ? 'out' : 'in'} ${index + 1}`),
      el('span', { class: 'hint' }, mask()
        ? (isOut
          ? `note bus ${port.bus === P.NO_BUS ? '—' : port.bus} → ${portNames(mask()).join(', ')}`
          : `${portNames(mask()).join(', ')} → note bus ${port.bus === P.NO_BUS ? '—' : port.bus}`)
        : 'unused')),
    portToggles(mask(), (chosen) => {
      if (isOut) port.targetMask = chosen; else port.sourceMask = chosen;
      send();
      app.render();
    }, { label: `${what} ${index + 1} ${isOut ? 'targets' : 'sources'}` }),
    el('div', { class: 'route-fields' },
      channelSelect(port.channel, (channel) => {
        port.channel = channel;
        send();
        app.render();
      }, isOut ? { omni: 'keep each event\u2019s channel' } : {}),
      busSelect(caps, Domain.Note, port.bus, (bus) => {
        port.bus = bus;
        send();
        app.render();
      }, 'no bus')));
}

export function routingPanel(app) {
  const caps = app.device?.capabilities;
  if (!caps) return null;

  const ins = app.patch.midiIn.slice(0, caps.midiIn).map((_, i) => routeCard(app, i, false));
  const outs = app.patch.midiOut.slice(0, caps.midiOut).map((_, i) => routeCard(app, i, true));

  return el('section', { class: 'panel' },
    el('h2', {}, 'MIDI routing'),
    el('p', { class: 'hint' },
      'An input port copies what it accepts onto a note bus; an output port sends a note '
      + 'bus to every port it names. Nodes sit between them, on the buses.'),
    el('div', { class: 'routes' },
      el('div', {}, el('h3', {}, 'inputs'), ins),
      el('div', {}, el('h3', {}, 'outputs'), outs)));
}

// --- clock, Program Change, NRPN -------------------------------------------

export function globalsPanel(app) {
  if (!app.device?.capabilities) return null;
  const g = app.globals;
  const push = () => { app.edit(() => app.device.setGlobals(g), 'settings'); app.render(); };

  const source = el('select', { onchange: (e) => { g.clockSource = Number(e.target.value); push(); } });
  for (const s of CLOCK_SOURCES) {
    const option = el('option', { value: String(s.value), title: s.hint }, s.label);
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

  // The key: one scale and one root for the whole patch. Every algorithm with
  // a scale parameter follows it unless it names a scale of its own, which is
  // what "global" is in those lists.
  const keyScale = el('select', { onchange: (e) => { g.scale = Number(e.target.value); push(); } });
  for (const s of KEY_SCALES) {
    const option = el('option', { value: String(s.value) }, s.label);
    if (s.value === g.scale) option.selected = true;
    keyScale.append(option);
  }

  const keyRoot = el('select', { onchange: (e) => { g.root = Number(e.target.value); push(); } });
  PITCH_CLASSES.forEach((name, pitchClass) => {
    const option = el('option', { value: String(pitchClass) }, name);
    if (pitchClass === (g.root ?? 0)) option.selected = true;
    keyRoot.append(option);
  });

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
    el('h2', {}, 'key'),
    el('p', { class: 'hint' },
      'The scale every algorithm follows unless it names one of its own - the quantiser, '
      + 'the chord voicer and the note sequencers. Chromatic is no key at all.'),
    el('div', { class: 'fields' },
      field('scale', keyScale, null),
      field('root', keyRoot, 'a change here moves the whole patch')),
    el('h2', { class: 'spaced' }, 'clock and recall'),
    el('div', { class: 'fields' },
      field('clock source', source),
      field('tempo', bpm, `${P.CLOCK_MIN_BPM}–${P.CLOCK_MAX_BPM} BPM, when the source is internal`),
      field('CV pulses per quarter', ppqn, 'how the sync jack is counted'),
      field('Program Change recalls presets', el('label', { class: 'bool' }, pcEnabled,
        el('span', {}, g.pcEnabled ? 'on' : 'off')),
        'off by default, so a Program Change meant for a downstream synth cannot switch your patch'),
      field('recall listens on', channelSelect(g.pcChannel, (c) => { g.pcChannel = c; push(); }), null),
      field('recall lands', quantise, 'a swap mid-bar glitches; one on a boundary does not')),
    el('div', { class: 'fields' },
      field('from these ports', portToggles(g.pcSourceMask, (mask) => { g.pcSourceMask = mask; push(); },
        { label: 'Program Change source ports' }), 'none selected means any port')),
    el('h2', { class: 'spaced' }, 'NRPN'),
    el('p', { class: 'hint' },
      'NRPN is a routable CC stream: 99/98/6/38 look like ordinary CCs to everything upstream, '
      + 'so the module only consumes them when you say so.'),
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
      }, { label: 'NRPN source ports' }), 'none selected means any port')));
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
      el('p', { class: 'hint' },
        'The app is talking to a module on a cable, so a controller belongs in that module’s '
        + 'own MIDI input rather than in this page.'));
  }
  if (!support.ok) {
    return el('section', { class: 'panel' },
      el('h2', {}, 'external controller'),
      el('p', { class: 'hint' }, support.reason),
      el('p', { class: 'hint' },
        'The on-screen keyboard under play needs none of this, and a binding can be typed in '
        + 'by hand below with no controller present.'));
  }
  if (!controller.access) {
    return el('section', { class: 'panel' },
      el('h2', {}, 'external controller'),
      el('p', { class: 'hint' },
        'Play the built-in module from a controller plugged into this computer, and send what '
        + 'the module plays back out to a real port.'),
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
      field('arriving on', arrives,
        el('span', { class: 'hint' }, 'the module’s own port, so a patch’s source filter applies')),
      field('send what it plays to', outputs)),
    el('p', { class: 'hint' }, controller.inputId
      ? 'Turn a knob and press “learn” beside a parameter to bind it — the binding is made by the '
        + 'firmware’s own control plane, exactly as it would be on hardware.'
      : 'Choose an input and the patch is played by it.'),
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
    : el('p', { class: 'hint' },
        'Nothing is bound. Add a binding below, or press “learn” beside any parameter '
        + 'and turn a controller.');

  const editors = slots.map((m, slot) => mappingEditor(app, slot, m)).filter(Boolean);

  return el('section', { class: 'panel' },
    el('h2', {}, 'MIDI control'),
    el('p', { class: 'hint' },
      `${used.length} of ${slots.length} binding slots in use. A binding is part of the patch: `
      + 'it is saved with a preset and exported with a file.'),
    summary,
    app.learnTarget
      ? el('div', { class: 'notes' },
          el('h4', {}, 'waiting for a controller'),
          el('p', {}, `Turn one to bind it to ${describeTarget(app, {
            targetKind: P.CcTargetKind.CC_TARGET_NODE,
            targetIndex: app.learnTarget.nodeIndex,
            param: app.learnTarget.param,
          })}.`),
          el('button', { onclick: () => app.cancelLearn() }, 'cancel'))
      : null,
    (() => {
      const all = el('details', { class: 'bindings-detail' },
        el('summary', {}, `every binding slot (${slots.length})`),
        el('div', { class: 'binding-list' }, editors));
      all.open = app.isOpen('bindings');
      all.addEventListener('toggle', () => app.setOpen('bindings', all.open));
      return all;
    })(),
    caps ? null : el('p', { class: 'hint' }, 'Connect a module to edit bindings.'));
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
    const option = el('option', { value: String(t.value), title: t.hint }, t.label);
    if ((m.flags & TAKEOVER.mask) === t.value) option.selected = true;
    takeover.append(option);
  }

  const relative = el('select', { onchange: (e) => {
    m.flags = (m.flags & ~RELATIVE.mask) | Number(e.target.value);
    push();
  } });
  for (const t of RELATIVE.options) {
    const option = el('option', { value: String(t.value), title: t.hint }, t.label);
    if ((m.flags & RELATIVE.mask) === t.value) option.selected = true;
    relative.append(option);
  }

  const toggle = (bit, label, hint) => {
    const box = el('input', { type: 'checkbox', class: 'switch',
      onchange: (e) => { m.flags = e.target.checked ? (m.flags | bit) : (m.flags & ~bit); push(); } });
    box.checked = (m.flags & bit) !== 0;
    return el('label', { class: 'bool', title: hint }, box, el('span', {}, label));
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
        el('span', { class: 'hint' }, 'both zero means the target’s full range, in its own units')),
      field('knob catches up by', takeover),
      field('the controller sends', relative)),
    el('div', { class: 'fields' },
      field('', toggle(FOURTEEN_BIT, '14-bit (this CC is the MSB, CC + 32 the LSB)')),
      field('', toggle(PASS_THROUGH, 'also pass the CC to the graph'))),
    el('div', { class: 'row' },
      el('button', { onclick: () => app.learnInto(slot, m) }, 'learn this one'),
      active ? el('button', { class: 'danger', onclick: () => app.clearMapping(slot) }, 'clear') : null,
      !m.sourceMask
        ? el('span', { class: 'hint' }, 'a binding with no source port is off')
        : null));
  if (active) editor.classList.add('active');
  return editor;
}
