// The play surface: the module, running, with the few controls a module has
// - two LEDs, eight jacks, a clock and its MIDI - plus the three things a
// rack cannot give you: ears, an on-screen keyboard, and a time axis.

import * as P from '../../protocol/generated.js';
import { el, classes } from '../dom.js';
import { Panel, Hint, Row } from '../components/Panel.js';
import { Labelled, Fields } from '../components/Field.js';
import { Select, range } from '../components/Select.js';
import { NumberField } from '../components/NumberField.js';
import { Switch } from '../components/Switch.js';
import { LevelSlider } from '../components/Slider.js';
import { IconButton } from '../components/IconButton.js';
import { ClockFields } from '../controls/ClockFields.js';
import { Meters } from '../panels/Meters.js';
import { ScopePanel, RollPanel } from '../scope/ScopePanels.js';
import { busPeers } from '../../core/patch.js';
import { Domain } from '../../core/validate.js';
import { noteName } from '../../core/music.js';
import { MUSICAL_PORTS, portNames } from '../../protocol/names.js';
import { WAVES } from '../../runtime/audio/listener.js';
import { KITS, PIECE_LABELS } from '../../runtime/audio/drums.js';
import './Play.css';

const MIDI_TYPES = {
  0x80: 'note off', 0x90: 'note on', 0xa0: 'poly AT', 0xb0: 'CC', 0xc0: 'program',
  0xd0: 'ch AT', 0xe0: 'bend', 0xf8: 'clock', 0xfa: 'start', 0xfb: 'continue', 0xfc: 'stop',
};
const LOG_SHOWN = 40;
const LOG_MS = 60;
const WHITE_KEYS = [0, 2, 4, 5, 7, 9, 11];

const peers = (app, domain, bus) => busPeers(app.device, app.state.patch, domain, bus);

export function PlayTab(app) {
  if (!app.module) return Hint('the built-in module is not running');
  if (!app.session.usingModule) {
    // A module on the cable has its own jacks, LEDs and sound; the page
    // cannot show any of it, and pretending otherwise would be a lie about
    // what is making the noise.
    return Panel('playing the module on the cable',
      Hint('play it from its own inputs'),
      Row(el('button', { onclick: () => app.useModule() }, 'use the built-in module')));
  }
  return el('div', {},
    Meters(app), TransportPanel(app), ScopePanel(app), RollPanel(app),
    JacksPanel(app), KeyboardPanel(app), ListenPanel(app), MonitorPanel(app));
}

// --- the clock ------------------------------------------------------------------

function TransportPanel(app) {
  const module = app.module;
  const g = app.state.globals;
  const extras = [];
  if (g.clockSource === 1) {
    // The sync jack is not one of the eight, so it needs its own control or
    // a CV-clocked patch cannot be tried at all without a cable.
    extras.push(Row(
      el('button', { onclick: () => module.syncPulse() }, 'sync pulse'),
      NumberField({ value: module.syncHz, min: 0, max: 100, step: 0.5, 'aria-label': 'sync pulses per second',
                    onChange: (hz) => module.setSyncRate(hz) }),
      el('span', { class: 'hint' }, 'Hz')));
  }
  if (g.clockSource === 2) extras.push(Hint('waiting for MIDI clock'));
  return Panel('clock',
    Row(IconButton({ icon: 'play', label: 'start the clock', text: 'start', onclick: () => module.clockStart() }),
        IconButton({ icon: 'stop', label: 'stop the clock', text: 'stop', onclick: () => module.clockStop() }),
        IconButton({ icon: 'resume', label: 'continue from where the clock stopped', text: 'continue',
                     class: 'ghost', onclick: () => module.clockResume() })),
    Fields(...ClockFields(app)),
    extras);
}

// --- jacks ------------------------------------------------------------------

function JacksPanel(app) {
  const module = app.module;
  const cards = [];
  const unused = [];
  const lamps = [];
  for (let j = 0; j < P.GPIO_N; j++) {
    // What a jack is for comes from the patch, not from the pin's mode.
    const mode = app.state.patch.gatePorts[j]?.direction ?? P.GatePortDirection.GATE_PORT_UNUSED;
    if (mode === P.GatePortDirection.GATE_PORT_UNUSED) { unused.push(j + 1); continue; }
    const isIn = mode === P.GatePortDirection.GATE_PORT_IN;
    const lamp = el('span', { class: 'led jack-lamp' });
    lamps.push({ lamp, j, isIn });
    // A gate is an edge, so a button that only toggled would need two
    // presses to make one trigger. Tap is the common case; hold is there for
    // a level, and the rate for anything that wants a clock.
    const controls = isIn ? [
      el('button', { onpointerdown: () => module.pulseJack(j) }, 'tap'),
      el('button', { class: classes(module.jackSources[j].level && 'active'),
                     onclick: () => { module.setJackInput(j, !module.jackSources[j].level); app.render(); } },
         module.jackSources[j].level ? 'held high' : 'hold'),
      NumberField({ value: module.jackRate(j), min: 0, max: 100, step: 0.5, 'aria-label': `jack ${j + 1} rate`,
                    onChange: (hz) => { module.setJackRate(j, hz); app.render(); } }),
      el('span', { class: 'hint' }, 'Hz'),
    ] : [el('span', { class: 'hint' }, 'from the patch')];
    cards.push(el('div', { class: 'jack-card' },
      el('div', { class: 'jack-head' }, lamp, el('span', {}, `jack ${j + 1}`), el('span', { class: 'hint' }, isIn ? 'in' : 'out')),
      Row(controls)));
  }
  app.live.paint(({ activity }) => {
    for (const { lamp, j, isIn } of lamps) {
      lamp.classList.toggle('lit', Boolean((isIn ? activity.jackIn : activity.jackOut) & (1 << j)));
    }
  });
  return Panel('jacks',
    cards.length ? el('div', { class: 'jack-cards' }, cards) : Hint('no jacks'),
    // Eight cards saying "not used" is a screenful of nothing on a phone.
    unused.length ? Hint(`unused: ${unused.join(', ')}`) : null);
}

// --- playing ----------------------------------------------------------------

function KeyboardPanel(app) {
  const module = app.module;
  const play = app.state.ui.play;
  const send = (type, d1, d2) => { module.deliverMidi(play.port, type, play.channel, d1, d2); app.refreshLive(); };
  const accepted = el('span', { class: 'hint' }, '');
  app.live.paint(({ module: m }) => {
    accepted.textContent = m.accepted === null ? '' : `taken by ${m.accepted} port(s)`;
  });

  const base = (play.octave + 1) * 12;
  const keys = [];
  for (let n = base; n <= base + 12 && n <= 127; n++) {
    const key = el('button', { class: classes('key', !WHITE_KEYS.includes(n % 12) && 'black'), type: 'button' }, noteName(n));
    const down = (e) => {
      e.preventDefault();
      if (key.classList.contains('held')) return;
      key.classList.add('held');
      send(0x90, n, play.velocity);
    };
    const up = (e) => {
      e?.preventDefault();
      if (!key.classList.contains('held')) return;
      key.classList.remove('held');
      module.deliverMidi(play.port, 0x80, play.channel, n, 0);
    };
    key.addEventListener('pointerdown', down);
    for (const type of ['pointerup', 'pointercancel', 'pointerleave']) key.addEventListener(type, up);
    key.addEventListener('contextmenu', (e) => e.preventDefault());
    keys.push(key);
  }
  const number = (key, min, max, label) => NumberField({
    value: play[key], min, max, 'aria-label': label, onChange: (v) => { play[key] = v; },
  });

  return Panel('play',
    Row(...Labelled('into', Select({ options: MUSICAL_PORTS, value: play.port,
                                     onChange: (port) => { play.port = port; app.render(); } })),
        ...Labelled('channel', number('channel', 1, 16, 'channel')),
        ...Labelled('velocity', number('velocity', 1, 127, 'velocity')),
        accepted),
    Row(el('button', { class: 'ghost', onclick: () => { play.octave = Math.max(-1, play.octave - 1); app.render(); } }, 'oct −'),
        el('span', { class: 'field-name' }, `C${play.octave}`),
        el('button', { class: 'ghost', onclick: () => { play.octave = Math.min(8, play.octave + 1); app.render(); } }, 'oct +')),
    el('div', { class: 'keys' }, keys),
    Row(...Labelled('CC', number('cc', 0, 127, 'CC number')),
        ...Labelled('value', number('ccValue', 0, 127, 'CC value')),
        el('button', { onclick: () => send(0xb0, play.cc, play.ccValue) }, 'send'),
        el('button', { class: 'ghost', onclick: () => {
          module.deliverMidi(play.port, 0xb0, play.channel, 123, 0);
          app.listener?.allOff();
        } }, 'all notes off')));
}

// --- listening -------------------------------------------------------------

// A count written per frame, so a bus that is silent when it should not be
// says so where the choice that made it silent is.
function liveCount(app, text) {
  const readout = el('span', { class: 'hint' }, '');
  app.live.paint(() => { readout.textContent = text(); });
  return readout;
}

function ListenPanel(app) {
  const listener = app.listener;
  const saved = () => app.patches.saveListen();
  return Panel('listen',
    Row(el('button', { class: listener.enabled ? 'active' : 'primary', onclick: async () => {
          try { await listener.toggle(); } catch (error) { app.state.error = error.message; }
          app.render();
        } }, listener.enabled ? 'audio on' : 'enable audio'),
        ...Labelled('master', LevelSlider({ value: listener.volume, label: 'master volume',
                                            onInput: (v) => listener.setVolume(v), onCommit: saved })),
        liveCount(app, () => (listener.enabled ? `${listener.voiceCount()} voices` : ''))),
    el('h4', { class: 'spaced' }, 'players'),
    el('div', { class: 'players' }, listener.players.map((player) => PlayerRow(app, player))),
    Row(IconButton({ icon: 'plus', label: 'add a player', text: 'player', class: 'ghost', onclick: () => {
          // A new player starts on a bus rather than on the output: one of
          // those exists already.
          listener.addPlayer({ source: 'bus', bus: firstBusInUse(app), wave: nextWave(listener) });
          saved();
          app.render();
        } }),
        listener.players.length ? null : el('span', { class: 'hint' }, 'no player')),
    DrumsSection(app),
    GatesSection(app));
}

// One player: what it listens to, what it sounds like, and how loud.
function PlayerRow(app, player) {
  const listener = app.listener;
  const saved = () => app.patches.saveListen();
  const buses = app.device?.capabilities?.noteBuses ?? P.N_NOTE_BUS;
  return el('div', { class: 'player' },
    Row(Select({
          class: 'grow', 'aria-label': 'what this player listens to',
          options: [{ value: 'out', label: 'what the module sends' }, ...range(buses, (b) => `note bus ${b}`)],
          value: player.source === 'out' ? 'out' : player.bus,
          onChange: (value) => {
            listener.setPlayerSource(player.id, value === 'out' ? 'out' : 'bus', value === 'out' ? 0 : value);
            saved();
            app.render();
          },
        }),
        Select({ 'aria-label': 'timbre', options: WAVES.map((w) => ({ value: w, label: w })), value: player.wave,
                 onChange: (wave) => { player.setWave(wave); saved(); } }),
        IconButton({ icon: 'trash', label: `remove the player on ${player.describe()}`, class: 'ghost danger',
                     onclick: () => { listener.removePlayer(player.id); saved(); app.render(); } })),
    Row(...Labelled('level', LevelSlider({ value: player.volume, label: `volume of ${player.describe()}`,
                                           onInput: (v) => player.setVolume(v), onCommit: saved })),
        liveCount(app, () => (listener.enabled ? `${player.voices.size} sounding` : 'audio is off'))),
    Hint(playerHint(app, player)));
}

// What the chosen source actually carries, in the patch on screen.
function playerHint(app, player) {
  if (player.source !== 'bus') {
    return app.state.patch.midiOut.some((port) => port.targetMask) ? 'the MIDI leaving the module' : 'no MIDI output in this patch';
  }
  const { writers, readers } = peers(app, Domain.Note, player.bus);
  if (!writers.length) return `nothing writes note bus ${player.bus}`;
  return `from ${writers.join(', ')}${readers.length ? ` · to ${readers.join(', ')}` : ''}`;
}

// The bus a new player should open on: the first bus a *node* writes, in
// preference to one only a MIDI input writes, which carries what you just
// played rather than what the patch made of it.
function firstBusInUse(app) {
  const buses = app.device?.capabilities?.noteBuses ?? P.N_NOTE_BUS;
  let fallback = -1;
  for (let b = 0; b < buses; b++) {
    const { writers } = peers(app, Domain.Note, b);
    if (!writers.length) continue;
    if (writers.some((who) => !who.startsWith('MIDI in '))) return b;
    if (fallback < 0) fallback = b;
  }
  return fallback < 0 ? 0 : fallback;
}

// Two players on one waveform are one player as far as the ear is concerned.
const nextWave = (listener) => {
  const used = new Set(listener.players.map((p) => p.wave));
  return WAVES.find((wave) => !used.has(wave)) ?? WAVES[0];
};

// --- drums -------------------------------------------------------------------

// A drum machine is an instrument, not a source somebody might point a
// sawtooth at, so it is not in the players list: it is here, with a kit and
// a level of its own, and it is audible because it is in the patch.
function DrumsSection(app) {
  return el('div', {},
    el('h4', { class: 'spaced' }, 'drums'),
    el('div', { class: 'players' }, app.listener.drumRows().map((row) => DrumRow(app, row))));
}

function DrumRow(app, { source, voice }) {
  const saved = () => app.patches.saveListen();
  const hint = drumHint(app, source);
  return el('div', { class: 'player' },
    Row(el('span', { class: 'field-name grow' }, voice.label),
        Select({ 'aria-label': `kit for ${voice.label}`, options: KITS.map((k) => ({ value: k.id, label: k.label })),
                 value: voice.kit, onChange: (kit) => { voice.setKit(kit); saved(); app.render(); } }),
        Switch({ checked: voice.on, label: 'on', onChange: (on) => { voice.setOn(on); saved(); app.render(); } })),
    Row(...Labelled('level', LevelSlider({ value: voice.volume, label: `level of ${voice.label}`,
                                           onInput: (v) => voice.setVolume(v), onCommit: saved })),
        liveCount(app, () => (app.listener.enabled ? `${voice.hits} hits` : 'audio is off'))),
    hint ? Hint(hint) : null);
}

// What this row is actually going to play, in the patch on screen.
function drumHint(app, source) {
  if (!source) return null;
  if (source.kind === 'note') {
    if (source.bus === P.NO_BUS) return 'on no bus';
    const { readers } = peers(app, Domain.Note, source.bus);
    const where = source.channel ? `ch ${source.channel} on note bus ${source.bus}` : `note bus ${source.bus}`;
    return `${where}${readers.length ? ` · to ${readers.join(', ')}` : ''}`;
  }
  if (!source.lanes.length) return 'on no bus';
  return source.lanes.map((lane) => `lane ${lane.lane + 1}: ${PIECE_LABELS[lane.piece]} on gate bus ${lane.bus}`).join(' · ');
}

// --- the gate listener --------------------------------------------------------

function GatesSection(app) {
  const listener = app.listener;
  const saved = () => app.patches.saveListen();
  return el('div', {},
    el('h4', { class: 'spaced' }, 'gate listener'),
    Row(Switch({ checked: listener.clicks, label: 'click on gate edges',
                 onChange: (on) => { listener.clicks = on; saved(); app.render(); } })),
    Row(...Labelled('level', LevelSlider({ value: listener.clickVolume, label: 'volume of the gate clicks',
                                           onInput: (v) => listener.setClickVolume(v), onCommit: saved }))),
    el('div', { class: 'players' }, listener.gateSources.map((source) => GateRow(app, source))),
    Row(IconButton({ icon: 'plus', label: 'listen to another gate', text: 'gate', class: 'ghost', onclick: () => {
          listener.addGateSource(nextGateSource(app));
          saved();
          app.render();
        } }),
        listener.gateSources.length ? null : el('span', { class: 'hint' }, 'no gate')));
}

const gateValue = (source) => (source.kind === 'jacks' ? 'jacks' : `${source.kind}:${source.index}`);
const parseGateValue = (value) => {
  if (value === 'jacks') return { kind: 'jacks', index: 0 };
  const [kind, index] = value.split(':');
  return { kind, index: Number(index) };
};

function GateRow(app, source) {
  const listener = app.listener;
  return el('div', { class: 'player' },
    Row(Select({ class: 'grow', 'aria-label': 'which gate this listens to', options: gateOptions(app, source),
                 value: gateValue(source),
                 onChange: (value) => { listener.setGateSource(source.id, parseGateValue(value)); app.patches.saveListen(); app.render(); } }),
        IconButton({ icon: 'trash', label: 'stop listening to this gate', class: 'ghost danger',
                     onclick: () => { listener.removeGateSource(source.id); app.patches.saveListen(); app.render(); } })),
    Hint(gateHint(app, source)));
}

// What there is to listen to: the jacks this patch uses, and the gate buses
// something writes. A jack on no bus and a bus nothing writes can never fire
// - but whatever this source is *already* set to stays in its own list.
function gateOptions(app, current) {
  const D = P.GatePortDirection;
  const options = [{ value: 'jacks', label: 'every output jack' }];
  for (let j = 0; j < P.GPIO_N; j++) {
    const direction = app.state.patch.gatePorts[j]?.direction ?? D.GATE_PORT_UNUSED;
    const mine = current.kind === 'jack' && current.index === j;
    if (direction === D.GATE_PORT_UNUSED && !mine) continue;
    const side = direction === D.GATE_PORT_IN ? 'in' : direction === D.GATE_PORT_OUT ? 'out' : 'unused';
    options.push({ value: `jack:${j}`, label: `jack ${j + 1} (${side})` });
  }
  const buses = app.device?.capabilities?.gateBuses ?? P.N_GATE_BUS;
  for (let b = 0; b < buses; b++) {
    const { writers } = peers(app, Domain.Gate, b);
    const mine = current.kind === 'bus' && current.index === b;
    if (!writers.length && !mine) continue;
    options.push({ value: `bus:${b}`, label: `gate bus ${b}${writers.length ? ` — from ${writers[0]}` : ''}` });
  }
  return options;
}

function gateHint(app, source) {
  const ports = app.state.patch.gatePorts;
  if (source.kind === 'jacks') {
    const outs = ports.filter((port) => port.direction === P.GatePortDirection.GATE_PORT_OUT).length;
    return outs ? `${outs} output jack${outs === 1 ? '' : 's'}` : 'no output jack: silent';
  }
  if (source.kind === 'jack') {
    const direction = ports[source.index]?.direction ?? P.GatePortDirection.GATE_PORT_UNUSED;
    return direction === P.GatePortDirection.GATE_PORT_UNUSED ? `jack ${source.index + 1} is not in this patch` : `jack ${source.index + 1}`;
  }
  const { writers, readers } = peers(app, Domain.Gate, source.index);
  if (!writers.length) return `nothing writes gate bus ${source.index}`;
  return `from ${writers.join(', ')}${readers.length ? ` · to ${readers.join(', ')}` : ''}`;
}

// The gate a new row should open on: the first gate bus a node writes, which
// is the signal that has no other way of being heard.
function nextGateSource(app) {
  const chosen = new Set(app.listener.gateSources.map(gateValue));
  const buses = app.device?.capabilities?.gateBuses ?? P.N_GATE_BUS;
  for (let b = 0; b < buses; b++) {
    const { writers } = peers(app, Domain.Gate, b);
    if (!writers.length || chosen.has(`bus:${b}`)) continue;
    return { kind: 'bus', index: b };
  }
  return { kind: 'jacks', index: 0 };
}

// --- the MIDI monitor ----------------------------------------------------------

// The log, rebuilt when there is something new in it. "Something new" is the
// sequence number, not the length: the log is a ring, so once it is full its
// length never changes again. Whether to follow the tail is the reader's
// choice: scrolling back must not be undone by the next event.
function MonitorPanel(app) {
  const log = el('div', { class: 'log' });
  const scroll = el('div', { class: 'log-scroll' }, log);
  let seq = null;
  let at = 0;
  app.live.paint(({ module }) => {
    if (seq === module.midiSeq) return;
    // A stream of MIDI clock is forty events a second; there is no reading a
    // list rebuilt under the eye any faster than this.
    const now = performance.now();
    if (seq !== null && now - at < LOG_MS) return;
    seq = module.midiSeq;
    at = now;
    if (!module.midiLog.length) { log.replaceChildren(Hint('nothing yet')); return; }
    const following = scroll.scrollHeight - scroll.scrollTop - scroll.clientHeight < 24;
    log.replaceChildren(...module.midiLog.slice(-LOG_SHOWN).map((event) => el('div', { class: 'log-line' },
      el('span', { class: 'log-time' }, `${(event.t / 1e6).toFixed(2)}s`),
      el('span', { class: 'log-what' }, MIDI_TYPES[event.type] ?? `0x${event.type.toString(16)}`),
      el('span', { class: 'log-data' }, module.isNote(event.type) ? `${noteName(event.d1)} vel ${event.d2}` : `${event.d1} ${event.d2}`),
      el('span', { class: 'log-where' }, `ch ${event.channel} → ${portNames(event.target).join(', ') || event.target}`))));
    if (following) scroll.scrollTop = scroll.scrollHeight;
  });
  return Panel('MIDI monitor', scroll,
    Row(IconButton({ icon: 'clear', label: 'clear the log', text: 'clear', class: 'ghost',
                     onclick: () => { app.module.clearMidiLog(); app.refreshLive(); } })));
}
