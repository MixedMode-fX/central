// The play surface: the module, running, with the few controls a module has.
//
// This is the emulator's surface: what a module in a rack gives you - two
// LEDs, eight jacks, a clock and its MIDI - plus the three things a rack
// cannot. Ears, an on-screen keyboard, and a *time* axis.
//
// The pass interval, the speed multiplier, single-stepping, the bus tables and
// the registry listing stayed behind when the emulator page became this tab:
// they answer questions about the *simulation*, and this tab answers questions
// about the patch. The scope did not belong in that list and should never have
// gone with them. A divider, a Euclidean sequencer and a logic gate are things
// whose output only exists over time - a lamp can say a gate is high, it
// cannot say the pattern is right - and the piano roll beside it does the same
// job for the notes. Both are in `scope.js`.

import * as P from './protocol.js';
import { el, noteName, slider, busUsers, iconButton, paintHarmony } from './views.js';
import { portNames, MUSICAL_PORTS, CLOCK_SOURCES } from './names.js';
import { scopePanel, drawScope, rollPanel, drawRoll } from './scope.js';
import { WAVES } from './audio.js';
import { KITS, PIECE_LABELS } from './drums.js';
import { Domain } from './validate.js';
import { refreshCanvasLive } from './canvas.js';

const MIDI_TYPES = {
  0x80: 'note off', 0x90: 'note on', 0xa0: 'poly AT', 0xb0: 'CC', 0xc0: 'program',
  0xd0: 'ch AT', 0xe0: 'bend', 0xf8: 'clock', 0xfa: 'start', 0xfb: 'continue', 0xfc: 'stop',
};
const LOG_SHOWN = 40;
const WHITE_KEYS = [0, 2, 4, 5, 7, 9, 11];

export function playTab(app) {
  if (!app.module) {
    return el('p', { class: 'hint' }, 'the built-in module is not running');
  }
  if (!app.usingModule) {
    // A module on the end of a cable has its own jacks, its own LEDs and its
    // own sound. The page cannot show any of it, and pretending otherwise -
    // an on-screen keyboard that plays a module in the page while a real one
    // sits on the desk - would be a lie about what is making the noise.
    return el('div', {},
      el('section', { class: 'panel' },
        el('h2', {}, 'playing the module on the cable'),
        el('p', { class: 'hint' }, 'play it from its own inputs'),
        el('div', { class: 'row' },
          el('button', { onclick: () => app.useModule() }, 'use the built-in module'))));
  }
  return el('div', {},
    metersPanel(app),
    transportPanel(app),
    scopePanel(app),
    rollPanel(app),
    jacksPanel(app),
    keyboardPanel(app),
    listenPanel(app),
    monitorPanel(app));
}

// --- the module's own feedback ---------------------------------------------

// The two LEDs and the gate buses. With no display on the module, this *is*
// the missing panel: green flashes the beat, red means attention, and a lit
// gate bus is a signal moving.
export function metersPanel(app) {
  if (!app.module || !app.usingModule) return null;
  const dots = [];
  for (let b = 0; b < P.N_GATE_BUS; b++) {
    dots.push(el('span', { class: 'gate-dot', id: `gate-${b}`, title: `gate bus ${b}` }));
  }
  return el('section', { class: 'panel meters' },
    el('div', { class: 'meter-row' },
      el('span', { class: 'led green', id: 'led-green', title: 'green: the clock' }),
      el('span', { class: 'led red', id: 'led-red', title: 'red: attention' }),
      el('span', { class: 'clock-readout', id: 'clock-readout' }, ''),
      el('div', { class: 'gate-dots' }, dots)));
}

function transportPanel(app) {
  const module = app.module;
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

  const extras = [];
  if (g.clockSource === 1) {
    // The sync jack is not one of the eight, so it needs its own control or a
    // CV-clocked patch cannot be tried at all without a cable.
    const rate = el('input', {
      type: 'number', class: 'number', min: '0', max: '100', step: '0.5', inputmode: 'decimal',
      value: String(module.syncHz), 'aria-label': 'sync pulses per second',
      onchange: (e) => module.setSyncRate(Math.max(0, Number(e.target.value) || 0)),
    });
    extras.push(el('div', { class: 'row' },
      el('button', { onclick: () => module.syncPulse() }, 'sync pulse'),
      rate, el('span', { class: 'hint' }, 'Hz')));
  }
  if (g.clockSource === 2) {
    extras.push(el('p', { class: 'hint' }, 'waiting for MIDI clock'));
  }

  return el('section', { class: 'panel' },
    el('h2', {}, 'clock'),
    el('div', { class: 'row' },
      iconButton({ icon: 'play', label: 'start the clock', text: 'start', onclick: () => module.clockStart() }),
      iconButton({ icon: 'stop', label: 'stop the clock', text: 'stop', onclick: () => module.clockStop() }),
      iconButton({ icon: 'resume', label: 'continue from where the clock stopped', text: 'continue',
                   class: 'ghost', onclick: () => module.clockResume() })),
    el('div', { class: 'row' },
      el('span', { class: 'field-name' }, 'source'), source,
      el('span', { class: 'field-name' }, 'tempo'), bpm),
    extras);
}

// --- jacks ------------------------------------------------------------------

function jacksPanel(app) {
  const module = app.module;
  const cards = [];
  const unused = [];
  for (let j = 0; j < P.GPIO_N; j++) {
    // What a jack is for comes from the patch, not from the pin's mode: a jack
    // an old patch drove is left as an input when it is released, so the pin
    // cannot tell "an input" from "nothing claims this".
    const mode = app.patch.gatePorts[j]?.direction ?? P.GatePortDirection.GATE_PORT_UNUSED;
    const controls = [];
    if (mode === P.GatePortDirection.GATE_PORT_IN) {
      const rate = el('input', {
        type: 'number', class: 'number', min: '0', max: '100', step: '0.5', inputmode: 'decimal',
        value: String(module.jackRate(j)), 'aria-label': `jack ${j + 1} rate`,
        onchange: (e) => { module.setJackRate(j, Math.max(0, Number(e.target.value) || 0)); app.render(); },
      });
      controls.push(
        // A gate is an edge, so a button that only toggled would need two
        // presses to make one trigger. Tap is the common case; hold is there
        // for a level, and the rate for anything that wants a clock.
        el('button', { onpointerdown: () => module.pulseJack(j) }, 'tap'),
        el('button', {
          class: module.jackSources[j].level ? 'active' : '',
          onclick: () => { module.setJackInput(j, !module.jackSources[j].level); app.render(); },
        }, module.jackSources[j].level ? 'held high' : 'hold'),
        rate, el('span', { class: 'hint' }, 'Hz'));
    } else if (mode === P.GatePortDirection.GATE_PORT_OUT) {
      controls.push(el('span', { class: 'hint' }, 'from the patch'));
    } else {
      // Eight cards saying "not used" is a screenful of nothing on a phone.
      unused.push(j + 1);
      continue;
    }
    cards.push(el('div', { class: 'jack-card' },
      el('div', { class: 'jack-head' },
        el('span', { class: 'led jack-lamp', id: `jack-lamp-${j}` }),
        el('span', {}, `jack ${j + 1}`),
        el('span', { class: 'hint' }, mode === P.GatePortDirection.GATE_PORT_IN ? 'in' : 'out')),
      el('div', { class: 'row' }, controls)));
  }
  return el('section', { class: 'panel' },
    el('h2', {}, 'jacks'),
    cards.length
      ? el('div', { class: 'jack-cards' }, cards)
      : el('p', { class: 'hint' }, 'no jacks'),
    unused.length
      ? el('p', { class: 'hint' }, `unused: ${unused.join(', ')}`)
      : null);
}

// --- playing ----------------------------------------------------------------

function keyboardPanel(app) {
  const module = app.module;
  const state = app.play;

  const portSelect = el('select', { onchange: (e) => { state.port = Number(e.target.value); app.render(); } });
  for (const port of MUSICAL_PORTS) {
    const option = el('option', { value: String(port.value) }, port.label);
    if (port.value === state.port) option.selected = true;
    portSelect.append(option);
  }

  const channel = el('input', {
    type: 'number', class: 'number', min: '1', max: '16', value: String(state.channel),
    inputmode: 'numeric', 'aria-label': 'channel',
    onchange: (e) => { state.channel = Math.max(1, Math.min(16, Number(e.target.value) || 1)); },
  });
  const velocity = el('input', {
    type: 'number', class: 'number', min: '1', max: '127', value: String(state.velocity),
    inputmode: 'numeric', 'aria-label': 'velocity',
    onchange: (e) => { state.velocity = Math.max(1, Math.min(127, Number(e.target.value) || 100)); },
  });

  const keys = [];
  const base = (state.octave + 1) * 12;
  for (let n = base; n <= base + 12 && n <= 127; n++) {
    const black = !WHITE_KEYS.includes(n % 12);
    const key = el('button', { class: `key ${black ? 'black' : ''}`, type: 'button' }, noteName(n));
    const down = (e) => {
      e.preventDefault();
      if (key.classList.contains('held')) return;
      key.classList.add('held');
      module.deliverMidi(state.port, 0x90, state.channel, n, state.velocity);
      app.refreshLive();
    };
    const up = (e) => {
      if (e) e.preventDefault();
      if (!key.classList.contains('held')) return;
      key.classList.remove('held');
      module.deliverMidi(state.port, 0x80, state.channel, n, 0);
    };
    key.addEventListener('pointerdown', down);
    key.addEventListener('pointerup', up);
    key.addEventListener('pointercancel', up);
    key.addEventListener('pointerleave', up);
    key.addEventListener('contextmenu', (e) => e.preventDefault());
    keys.push(key);
  }

  const cc = el('input', { type: 'number', class: 'number', min: '0', max: '127', value: String(state.cc),
    inputmode: 'numeric', 'aria-label': 'CC number',
    onchange: (e) => { state.cc = Math.max(0, Math.min(127, Number(e.target.value) || 0)); } });
  const value = el('input', { type: 'number', class: 'number', min: '0', max: '127', value: String(state.ccValue),
    inputmode: 'numeric', 'aria-label': 'CC value',
    onchange: (e) => { state.ccValue = Math.max(0, Math.min(127, Number(e.target.value) || 0)); } });

  return el('section', { class: 'panel' },
    el('h2', {}, 'play'),
    el('div', { class: 'row' },
      el('span', { class: 'field-name' }, 'into'), portSelect,
      el('span', { class: 'field-name' }, 'channel'), channel,
      el('span', { class: 'field-name' }, 'velocity'), velocity,
      el('span', { class: 'hint', id: 'accepted' }, '')),
    el('div', { class: 'row' },
      el('button', { class: 'ghost', onclick: () => { state.octave = Math.max(-1, state.octave - 1); app.render(); } }, 'oct −'),
      el('span', { class: 'field-name' }, `C${state.octave}`),
      el('button', { class: 'ghost', onclick: () => { state.octave = Math.min(8, state.octave + 1); app.render(); } }, 'oct +')),
    el('div', { class: 'keys' }, keys),
    el('div', { class: 'row' },
      el('span', { class: 'field-name' }, 'CC'), cc,
      el('span', { class: 'field-name' }, 'value'), value,
      el('button', { onclick: () => {
        module.deliverMidi(state.port, 0xb0, state.channel, state.cc, state.ccValue);
        app.refreshLive();
      } }, 'send'),
      el('button', { class: 'ghost', onclick: () => {
        module.deliverMidi(state.port, 0xb0, state.channel, 123, 0);
        app.listener?.allOff(app.listener.ctx?.currentTime ?? 0);
      } }, 'all notes off')));
}

function listenPanel(app) {
  const listener = app.listener;
  const clicks = el('input', { type: 'checkbox', class: 'switch',
    onchange: (e) => { listener.clicks = e.target.checked; app.saveListen(); app.render(); } });
  clicks.checked = listener.clicks;
  // Through `slider`, like every other one: a finger scrolling the play tab
  // must not set the volume on its way past.
  const volume = slider({
    class: 'grow', min: '0', max: '100', value: String(Math.round(listener.volume * 100)),
    'aria-label': 'master volume',
  }, { onInput: (v) => listener.setVolume(Number(v) / 100), onCommit: () => app.saveListen() });
  const clickVolume = slider({
    class: 'grow', min: '0', max: '100', value: String(Math.round(listener.clickVolume * 100)),
    'aria-label': 'volume of the gate clicks',
  }, { onInput: (v) => listener.setClickVolume(Number(v) / 100), onCommit: () => app.saveListen() });

  return el('section', { class: 'panel' },
    el('h2', {}, 'listen'),
    el('div', { class: 'row' },
      el('button', { class: listener.enabled ? 'active' : 'primary', onclick: async () => {
        try { await listener.toggle(); } catch (error) { app.pendingError = error.message; }
        app.render();
      } }, listener.enabled ? 'audio on' : 'enable audio'),
      el('span', { class: 'field-name' }, 'master'), volume,
      el('span', { class: 'hint', id: 'voices' }, '')),

    el('h4', { class: 'spaced' }, 'players'),
    el('div', { class: 'players' }, listener.players.map((player) => playerRow(app, player))),
    el('div', { class: 'row' },
      iconButton({ icon: 'plus', label: 'add a player', text: 'player', class: 'ghost', onclick: () => {
        // A new player starts on a bus rather than on the output: one of those
        // exists already, and a second copy of it is not what anybody is
        // adding a player for.
        listener.addPlayer({ source: 'bus', bus: firstBusInUse(app), wave: nextWave(listener) });
        app.saveListen();
        app.render();
      } }),
      listener.players.length ? null : el('span', { class: 'hint' }, 'no player')),

    drumsSection(app),
    gatesSection(app, clicks, clickVolume));
}

// One player: what it listens to, what it sounds like, and how loud.
function playerRow(app, player) {
  const listener = app.listener;
  const buses = app.device?.capabilities?.noteBuses ?? P.N_NOTE_BUS;

  const source = el('select', { class: 'grow', 'aria-label': 'what this player listens to',
    onchange: (e) => {
      const value = e.target.value;
      listener.setPlayerSource(player.id, value === 'out' ? 'out' : 'bus',
                               value === 'out' ? 0 : Number(value));
      app.saveListen();
      app.render();
    } });
  const out = el('option', { value: 'out' }, 'what the module sends');
  if (player.source === 'out') out.selected = true;
  source.append(out);
  for (let b = 0; b < buses; b++) {
    const option = el('option', { value: String(b) }, `note bus ${b}`);
    if (player.source === 'bus' && player.bus === b) option.selected = true;
    source.append(option);
  }

  const wave = el('select', { 'aria-label': 'timbre',
    onchange: (e) => { player.setWave(e.target.value); app.saveListen(); } });
  for (const shape of WAVES) {
    const option = el('option', { value: shape }, shape);
    if (shape === player.wave) option.selected = true;
    wave.append(option);
  }

  const volume = slider({
    class: 'grow', min: '0', max: '100', value: String(Math.round(player.volume * 100)),
    'aria-label': `volume of ${player.describe()}`,
  }, { onInput: (v) => player.setVolume(Number(v) / 100), onCommit: () => app.saveListen() });

  return el('div', { class: 'player' },
    el('div', { class: 'row' }, source, wave,
      iconButton({ icon: 'trash', label: `remove the player on ${player.describe()}`, class: 'ghost danger',
        onclick: () => { listener.removePlayer(player.id); app.saveListen(); app.render(); } })),
    el('div', { class: 'row' },
      el('span', { class: 'field-name' }, 'level'), volume,
      el('span', { class: 'hint', id: `voices-${player.id}` }, '')),
    el('p', { class: 'hint' }, playerHint(app, player)));
}

// --- drums -------------------------------------------------------------------

// A drum machine - a drum sequencer, or anything else the patch has set to
// channel 10 - is an instrument, not a source somebody might point a sawtooth
// at, so it is not in the players list: it is here, with a kit and a level of
// its own, and it is audible because it is in the patch rather than because
// somebody added a player for it.
function drumsSection(app) {
  const rows = app.listener.drumRows();
  return el('div', {},
    el('h4', { class: 'spaced' }, 'drums'),
    el('div', { class: 'players' }, rows.map((row) => drumRow(app, row))));
}

function drumRow(app, { source, voice }) {
  const kit = el('select', { 'aria-label': `kit for ${voice.label}`,
    onchange: (e) => { voice.setKit(e.target.value); app.saveListen(); app.render(); } });
  for (const option of KITS) {
    const item = el('option', { value: option.id }, option.label);
    if (option.id === voice.kit) item.selected = true;
    kit.append(item);
  }

  const on = el('input', { type: 'checkbox', class: 'switch',
    onchange: (e) => { voice.setOn(e.target.checked); app.saveListen(); app.render(); } });
  on.checked = voice.on;

  const volume = slider({
    class: 'grow', min: '0', max: '100', value: String(Math.round(voice.volume * 100)),
    'aria-label': `level of ${voice.label}`,
  }, { onInput: (v) => voice.setVolume(Number(v) / 100), onCommit: () => app.saveListen() });

  const hint = drumHint(app, source);
  return el('div', { class: 'player' },
    el('div', { class: 'row' },
      el('span', { class: 'field-name grow' }, voice.label),
      kit,
      el('label', { class: 'bool' }, on, el('span', {}, 'on'))),
    el('div', { class: 'row' },
      el('span', { class: 'field-name' }, 'level'), volume,
      el('span', { class: 'hint', id: `drum-${domId(voice.key)}` }, '')),
    hint ? el('p', { class: 'hint' }, hint) : null);
}

// What this row is actually going to play, in the patch on screen. A kit
// selector over a sequencer whose outlet is on no bus should say so rather
// than sit there silently doing nothing.
function drumHint(app, source) {
  if (!source) return null;
  if (source.kind === 'note') {
    if (source.bus === P.NO_BUS) return 'on no bus';
    const { readers } = busUsers(app, Domain.Note, source.bus);
    // A node that is drums because of its channel shares its bus with whatever
    // else writes there, so the hint has to say which notes are this voice's.
    const where = source.channel
      ? `ch ${source.channel} on note bus ${source.bus}` : `note bus ${source.bus}`;
    return `${where}${readers.length ? ` \u00b7 to ${readers.join(', ')}` : ''}`;
  }
  if (!source.lanes.length) return 'on no bus';
  return source.lanes
    .map((lane) => `lane ${lane.lane + 1}: ${PIECE_LABELS[lane.piece]} on gate bus ${lane.bus}`)
    .join(' \u00b7 ');
}

// --- the gate listener --------------------------------------------------------

function gatesSection(app, clicks, clickVolume) {
  const listener = app.listener;
  return el('div', {},
    el('h4', { class: 'spaced' }, 'gate listener'),
    el('div', { class: 'row' },
      el('label', { class: 'bool' }, clicks, el('span', {}, 'click on gate edges'))),
    el('div', { class: 'row' },
      el('span', { class: 'field-name' }, 'level'), clickVolume),
    el('div', { class: 'players' }, listener.gateSources.map((source) => gateRow(app, source))),
    el('div', { class: 'row' },
      iconButton({ icon: 'plus', label: 'listen to another gate', text: 'gate', class: 'ghost', onclick: () => {
        listener.addGateSource(nextGateSource(app));
        app.saveListen();
        app.render();
      } }),
      listener.gateSources.length ? null : el('span', { class: 'hint' }, 'no gate')));
}

function gateRow(app, source) {
  const listener = app.listener;
  const select = el('select', { class: 'grow', 'aria-label': 'which gate this listens to',
    onchange: (e) => {
      listener.setGateSource(source.id, parseGateValue(e.target.value));
      app.saveListen();
      app.render();
    } });
  for (const option of gateOptions(app, source)) {
    const item = el('option', { value: option.value }, option.label);
    if (option.value === gateValue(source)) item.selected = true;
    select.append(item);
  }
  return el('div', { class: 'player' },
    el('div', { class: 'row' }, select,
      iconButton({ icon: 'trash', label: 'stop listening to this gate', class: 'ghost danger',
        onclick: () => { listener.removeGateSource(source.id); app.saveListen(); app.render(); } })),
    el('p', { class: 'hint' }, gateHint(app, source)));
}

const gateValue = (source) => (source.kind === 'jacks' ? 'jacks' : `${source.kind}:${source.index}`);

function parseGateValue(value) {
  if (value === 'jacks') return { kind: 'jacks', index: 0 };
  const [kind, index] = value.split(':');
  return { kind, index: Number(index) };
}

// What there is to listen to: the jacks this patch uses, and the gate buses
// something writes. A jack on no bus and a bus nothing writes can never fire,
// so offering them is offering silence - but whatever this source is *already*
// set to stays in its own list, or choosing it would look like losing it.
function gateOptions(app, current) {
  const options = [{ value: 'jacks', label: 'every output jack' }];
  const D = P.GatePortDirection;
  for (let j = 0; j < P.GPIO_N; j++) {
    const direction = app.patch.gatePorts[j]?.direction ?? D.GATE_PORT_UNUSED;
    const mine = current.kind === 'jack' && current.index === j;
    if (direction === D.GATE_PORT_UNUSED && !mine) continue;
    const side = direction === D.GATE_PORT_IN ? 'in' : direction === D.GATE_PORT_OUT ? 'out' : 'unused';
    options.push({ value: `jack:${j}`, label: `jack ${j + 1} (${side})` });
  }
  const buses = app.device?.capabilities?.gateBuses ?? P.N_GATE_BUS;
  for (let b = 0; b < buses; b++) {
    const { writers } = busUsers(app, Domain.Gate, b);
    const mine = current.kind === 'bus' && current.index === b;
    if (!writers.length && !mine) continue;
    options.push({ value: `bus:${b}`, label: `gate bus ${b}${writers.length ? ` \u2014 from ${writers[0]}` : ''}` });
  }
  return options;
}

function gateHint(app, source) {
  if (source.kind === 'jacks') {
    const outs = app.patch.gatePorts.filter((port) => port.direction === P.GatePortDirection.GATE_PORT_OUT).length;
    return outs
      ? `${outs} output jack${outs === 1 ? '' : 's'}`
      : 'no output jack: silent';
  }
  if (source.kind === 'jack') {
    const direction = app.patch.gatePorts[source.index]?.direction ?? P.GatePortDirection.GATE_PORT_UNUSED;
    if (direction === P.GatePortDirection.GATE_PORT_UNUSED) return `jack ${source.index + 1} is not in this patch`;
    return `jack ${source.index + 1}`;
  }
  const { writers, readers } = busUsers(app, Domain.Gate, source.index);
  if (!writers.length) return `nothing writes gate bus ${source.index}`;
  return `from ${writers.join(', ')}${readers.length ? ` \u00b7 to ${readers.join(', ')}` : ''}`;
}

// The gate a new row should open on, so adding one is a press rather than a
// press and a hunt: the first gate bus a node writes, which is the signal that
// has no other way of being heard, and the jacks only if there is none.
function nextGateSource(app) {
  const chosen = new Set(app.listener.gateSources.map(gateValue));
  const buses = app.device?.capabilities?.gateBuses ?? P.N_GATE_BUS;
  for (let b = 0; b < buses; b++) {
    const { writers } = busUsers(app, Domain.Gate, b);
    if (!writers.length || chosen.has(`bus:${b}`)) continue;
    return { kind: 'bus', index: b };
  }
  return { kind: 'jacks', index: 0 };
}

// An element id from a voice key: `node:3` carries a colon, which is legal in
// an id and awkward in every selector that might later be written against it.
const domId = (key) => key.replace(/:/g, '-');

// What the chosen source actually carries, in the patch on screen. A list of
// eight identical "note bus N" is a guess; "from NoteSeq 1 out" is an answer.
function playerHint(app, player) {
  if (player.source !== 'bus') {
    const outs = app.patch.midiOut.filter((port) => port.targetMask).length;
    return outs ? 'the MIDI leaving the module' : 'no MIDI output in this patch';
  }
  const { writers, readers } = busUsers(app, Domain.Note, player.bus);
  if (!writers.length) return `nothing writes note bus ${player.bus}`;
  return `from ${writers.join(', ')}${readers.length ? ` · to ${readers.join(', ')}` : ''}`;
}

// The bus a new player should open on, so adding one is a press rather than a
// press and a hunt: the first bus a *node* writes - a sequencer's output, an
// arpeggiator's - in preference to one only a MIDI input writes, which carries
// what you just played rather than what the patch made of it.
function firstBusInUse(app) {
  const buses = app.device?.capabilities?.noteBuses ?? P.N_NOTE_BUS;
  let fallback = -1;
  for (let b = 0; b < buses; b++) {
    const { writers } = busUsers(app, Domain.Note, b);
    if (!writers.length) continue;
    if (writers.some((who) => !who.startsWith('MIDI in '))) return b;
    if (fallback < 0) fallback = b;
  }
  return fallback < 0 ? 0 : fallback;
}

// Two players on one waveform are one player as far as the ear is concerned,
// which defeats the point of having two.
function nextWave(listener) {
  const used = new Set(listener.players.map((p) => p.wave));
  return WAVES.find((wave) => !used.has(wave)) ?? WAVES[0];
}

function monitorPanel(app) {
  return el('section', { class: 'panel' },
    el('h2', {}, 'MIDI monitor'),
    el('div', { class: 'log-scroll' }, el('div', { class: 'log', id: 'midi-log' })),
    el('div', { class: 'row' },
      iconButton({ icon: 'clear', label: 'clear the log', text: 'clear', class: 'ghost',
                   onclick: () => { app.module.clearMidiLog(); app.refreshLive(); } })));
}

// --- the live bits ----------------------------------------------------------
//
// Everything above is rebuilt only when the patch changes. What moves is
// written straight into the DOM here, once per animation frame: re-rendering
// the page for a blinking LED would fight every open <select> and every held
// key.
//
// **Per frame, and from the module's own per-pass sampling** - not from a
// timer reading the levels for itself. This used to run ten times a second and
// read whatever was high at that instant, which is the wrong shape for the
// thing it was showing: a trigger is high for one or two passes, an animation
// frame is sixteen, and a tenth of a second is a hundred. A gate dot lit when
// the poll happened to land inside a pulse and stayed dark otherwise, so the
// lights were somewhere between late and fictional. `takeActivity()` hands
// over everything that has been high since the last paint, folded together
// with what is high now, so an edge can be one frame late but can no longer be
// missed.
export function refreshLive(app) {
  const module = app.module;
  if (!module || !app.usingModule) return;

  const live = module.takeActivity();
  const clock = module.clock();
  const green = document.getElementById('led-green');
  const red = document.getElementById('led-red');
  if (green) green.style.opacity = String(Math.max(0.08, live.green / 255));
  if (red) red.style.opacity = String(Math.max(0.08, live.red / 255));

  const readout = document.getElementById('clock-readout');
  if (readout) {
    readout.textContent = clock.running
      ? `${clock.bpm} BPM \u00b7 beat ${Math.floor(clock.count / (P.MASTER_PPQN * P.CLOCK_SUBTICK)) + 1}`
      : 'clock stopped';
  }

  for (let b = 0; b < P.N_GATE_BUS; b++) {
    const dot = document.getElementById(`gate-${b}`);
    if (dot) dot.classList.toggle('lit', (live.gate & (1 << b)) !== 0);
  }

  for (let j = 0; j < P.GPIO_N; j++) {
    const lamp = document.getElementById(`jack-lamp-${j}`);
    if (!lamp) continue;
    const mode = app.patch.gatePorts[j]?.direction ?? P.GatePortDirection.GATE_PORT_UNUSED;
    const high = mode === P.GatePortDirection.GATE_PORT_OUT
      ? live.jackOut & (1 << j)
      : live.jackIn & (1 << j);
    lamp.classList.toggle('lit', Boolean(high));
    lamp.classList.toggle('out', mode === P.GatePortDirection.GATE_PORT_OUT);
  }

  const accepted = document.getElementById('accepted');
  if (accepted) {
    accepted.textContent = module.accepted === null ? '' : `taken by ${module.accepted} port(s)`;
  }
  const voices = document.getElementById('voices');
  if (voices && app.listener) {
    voices.textContent = app.listener.enabled ? `${app.listener.voiceCount()} voices` : '';
  }
  // Per player, so a bus that is silent when it should not be says so where
  // the choice that made it silent is.
  for (const player of app.listener?.players ?? []) {
    const readout = document.getElementById(`voices-${player.id}`);
    if (readout) readout.textContent = app.listener.enabled ? `${player.voices.size} sounding` : 'audio is off';
  }
  // And per drum sequencer, which answers the question a kit selector raises
  // and cannot answer by itself: is this one making any sound at all?
  for (const { voice } of app.listener?.drumRows() ?? []) {
    const readout = document.getElementById(`drum-${domId(voice.key)}`);
    if (readout) readout.textContent = app.listener.enabled ? `${voice.hits} hits` : 'audio is off';
  }

  refreshCanvasLive(app, live);
  drawScope(app);
  drawRoll(app);
  refreshLog(app);
  refreshPlayheads(app);
}

// The log, rebuilt when there is something new in it.
//
// "Something new" is the sequence number, not the length. The log is a ring of
// two hundred events: once it is full, every further event shifts one off the
// front and leaves the length at two hundred for ever. Comparing lengths -
// which is what this did - therefore froze the view after the two hundredth
// event and only ever thawed when *clear* took the length back to zero, which
// is exactly the "it fills up, stops, and refills when I clear it" that was
// reported. `midiSeq` counts every event the module has ever sent.
//
// The mark lives on the element rather than in a variable here, so a rebuilt
// panel - the page is replaced wholesale on every edit - draws itself again
// instead of waiting for the next event to notice it is empty.
const LOG_MS = 60;
function refreshLog(app) {
  const box = document.getElementById('midi-log');
  if (!box) return;
  const module = app.module;
  const seq = module.midiSeq;
  if (Number(box.dataset.seq) === seq) return;
  // A stream of MIDI clock is forty events a second; there is no reading a
  // list that is rebuilt under the eye any faster than this.
  const at = (globalThis.performance ?? Date).now();
  if (box.dataset.seq !== undefined && at - Number(box.dataset.at ?? 0) < LOG_MS) return;
  box.dataset.seq = String(seq);
  box.dataset.at = String(at);

  const log = module.midiLog;
  if (!log.length) {
    box.replaceChildren(el('p', { class: 'hint' }, 'nothing yet'));
    return;
  }
  // Whether to follow the tail is the reader's choice: scrolling back through
  // what just went past must not be undone by the next event. The scroller is
  // the box around the log, not the log - which is why this never worked.
  const scroll = box.parentElement;
  const following = !scroll || scroll.scrollHeight - scroll.scrollTop - scroll.clientHeight < 24;
  box.replaceChildren(...log.slice(-LOG_SHOWN).map((event) => el('div', { class: 'log-line' },
    el('span', { class: 'log-time' }, `${(event.t / 1e6).toFixed(2)}s`),
    el('span', { class: 'log-what' }, MIDI_TYPES[event.type] ?? `0x${event.type.toString(16)}`),
    el('span', { class: 'log-data' }, module.isNote(event.type)
      ? `${noteName(event.d1)} vel ${event.d2}`
      : `${event.d1} ${event.d2}`),
    el('span', { class: 'log-where' }, `ch ${event.channel} \u2192 ${portNames(event.target).join(', ') || event.target}`))));
  if (scroll && following) scroll.scrollTop = scroll.scrollHeight;
}

// A playhead on the step grids in the patch tab, read from the running node.
// This is what "edit it while it plays" means in practice: the step being
// edited and the step being played are the same square. A harmony's circle of
// fifths is the same idea in another shape, so it is painted from here too.
function refreshPlayheads(app) {
  if (app.tab !== 'patch') return;
  for (const cell of document.querySelectorAll('.cell.playing, .note-cell.playing')) {
    cell.classList.remove('playing');
  }
  const nodes = Math.min(app.patch.nodes.length, app.module.nodeCount());
  for (let node = 0; node < nodes; node++) {
    paintHarmony(app, node);
    if (!app.module.seqKind(node)) continue;
    const lanes = app.module.seqLanes(node);
    for (let lane = 0; lane < lanes; lane++) {
      const cell = document.getElementById(`cell-${node}-${lane}-${app.module.seqPosition(node, lane)}`);
      if (cell) cell.classList.add('playing');
    }
  }
}
