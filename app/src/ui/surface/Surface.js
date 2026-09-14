// The performance surface: the whole screen, sixteen pads and eight pots.
//
// The play button used to open a column of eight panels - meters, a clock, a
// scope, a piano roll, jacks, a keyboard, four sections of listener and a MIDI
// monitor. That is an instrument panel, and on stage you would scroll past an
// oscilloscope to reach a control. Those panels are still there, as the
// *module* tab, which is where an instrument panel belongs; this is what the
// play button opens.
//
// **It fits 360x640 with no page scroll, and that constraint decides the
// counts rather than the other way round.** The shell is a grid of fixed rows
// with the pads taking whatever is left: nothing here is allowed to grow a
// scrollbar, because a control you have to scroll to is a control you do not
// have.
//
// **Fixed geometry, soft assignment** (services/surface.js): you cannot move a
// control, and you can change everything about what it does. Long-press one to
// say what it does.

import * as P from '../../protocol/generated.js';
import { el, classes } from '../dom.js';
import { icon } from '../components/icons.js';
import { IconButton } from '../components/IconButton.js';
import { MachineBadge } from '../components/Machine.js';
import { KeyboardOverlay } from '../components/Keyboard.js';
import { closeMenu } from '../components/Menu.js';
import { PadKind, PadMode, POTS, PADS } from '../../services/surface.js';
import { describeTarget } from '../panels/ModMatrix.js';
import { AssignSheet } from './Assign.js';
import './Surface.css';

// How long a press has to be held before it stops being a note and starts
// being a question about what this control does. Long enough not to fire
// during a drum roll, short enough to find by accident once.
const HOLD_MS = 500;
const MOVED_PX = 10;

// A vertical drag across a knob's own height and a bit: far enough to be
// deliberate, short enough to reach 127 without a second grab.
const PIXELS_PER_STEP = 1.6;

export function Surface(app) {
  closeMenu();
  const ui = app.state.ui.surface;
  return el('div', { class: 'surface' },
    TopBar(app),
    Pots(app),
    Pads(app),
    app.state.error ? el('p', { class: 'surface-error', onclick: () => app.dismissError() }, app.state.error) : null,
    ui.keyboard
      ? KeyboardOverlay({
          octave: app.state.ui.play.octave,
          velocity: app.state.ui.play.velocity,
          mounted: (fn) => app.live.onMount(fn),
          onOctave: (octave) => { app.state.ui.play.octave = octave; },
          onNoteOn: (pitch, velocity) => app.play.noteOn(pitch, velocity),
          onNoteOff: (pitch) => app.play.noteOff(pitch),
          onClose: () => { ui.keyboard = false; app.render(); },
        })
      : null,
    ui.editing ? AssignSheet(app, ui.editing) : null);
}

function TopBar(app) {
  const b = app.surface.budget();
  const armed = app.state.ui.surface.armed;
  return el('header', { class: 'surface-top' },
    IconButton({
      icon: 'edit', text: 'edit', label: 'leave the surface and go back to editing',
      class: 'ghost', onclick: () => app.togglePlay(),
    }),
    el('div', { class: 'surface-say' },
      MachineBadge(app, { compact: true }),
      el('span', { class: classes('hint', !b.free && 'spent') },
        `CC ${b.bindings}/${b.bindingSlots} · macros ${b.macros}/${b.macroSlots} · pool ${b.dests}/${b.destSlots}`)),
    armed
      ? el('button', { class: 'warn-btn', onclick: () => app.surface.disarm() }, 'waiting — cancel')
      : IconButton({
          icon: 'key', text: 'keys', label: 'summon the keyboard',
          class: classes('ghost', app.state.ui.surface.keyboard && 'active'),
          onclick: () => {
            app.state.ui.surface.keyboard = !app.state.ui.surface.keyboard;
            app.render();
          },
        }));
}

// --- the pots ----------------------------------------------------------------

function Pots(app) {
  return el('div', { class: 'pots' },
    Array.from({ length: POTS }, (_, i) => Pot(app, i)));
}

function Pot(app, index) {
  const surface = app.surface;
  const pot = surface.pot(index);
  const where = { kind: 'pot', index };
  const bound = surface.bindingOf(pot);
  const live = liveMacro(app, bound);

  const dial = el('div', {
    class: classes('knob', `hue-${pot.colour}`, live && 'reading'),
    role: 'slider', tabindex: '0',
    'aria-label': `${potName(pot)}, ${describeReach(app, bound)}`,
    'aria-valuemin': '0', 'aria-valuemax': '127', 'aria-valuenow': String(pot.value),
    style: `--at: ${pot.value / 127}`,
  }, el('span', { class: 'knob-value' }, String(pot.value)));

  // The knob paints itself while it is being dragged: a render would replace
  // the element under the finger, which is the same rule every other
  // continuous control in the app follows (components/Slider.js).
  const paint = (value) => {
    dial.style.setProperty('--at', String(value / 127));
    dial.setAttribute('aria-valuenow', String(value));
    dial.firstChild.textContent = String(value);
  };

  hold(dial, {
    onHold: () => openAssign(app, where),
    onDown: (e) => {
      dial.setPointerCapture(e.pointerId);
      return { y: e.clientY, from: pot.value };
    },
    onMove: (e, start) => paint(surface.turn(index, start.from - (e.clientY - start.y) / PIXELS_PER_STEP)),
    onUp: () => surface.turn(index, pot.value, { commit: true }),
  });

  dial.addEventListener('keydown', (e) => {
    const step = { ArrowUp: 1, ArrowRight: 1, ArrowDown: -1, ArrowLeft: -1 }[e.key];
    const to = e.key === 'Home' ? 0 : e.key === 'End' ? 127 : null;
    if (step === undefined && to === null) return;
    e.preventDefault();
    paint(surface.turn(index, to ?? pot.value + step * (e.shiftKey ? 10 : 1), { commit: true }));
  });

  // A pot on a macro is the one control here that genuinely reads back: the
  // module says where the macro is (SYSEX_MACRO_STATE), so the ring follows
  // an LFO sweeping it, not just what this pot last sent.
  if (live !== null) {
    app.live.watchMacro(live);
    app.live.paint(() => {
      const state = app.session.macroLive.get(live);
      if (!state) return;
      dial.style.setProperty('--live', String(state.position / 255));
      dial.classList.toggle('engaged', state.engaged);
    });
  }

  return el('div', { class: 'pot' },
    dial,
    el('span', { class: 'pot-name' }, potName(pot)),
    el('span', { class: 'pot-what hint' }, describeReach(app, bound)));
}

const potName = (pot) => pot.label || `CC ${pot.cc}`;

// The macro a control is driving, or null. Only a macro reads back; every
// other target is written and not watched.
function liveMacro(app, bound) {
  if (!bound) return null;
  return bound.mapping.targetKind === P.CcTargetKind.CC_TARGET_MACRO
    ? bound.mapping.targetIndex : null;
}

function describeReach(app, bound) {
  if (!bound) return 'not bound';
  return describeTarget(app, bound.mapping);
}

// --- the pads ----------------------------------------------------------------

function Pads(app) {
  return el('div', { class: 'pads' },
    Array.from({ length: PADS }, (_, i) => Pad(app, i)));
}

function Pad(app, index) {
  const surface = app.surface;
  const pad = surface.pad(index);
  const where = { kind: 'pad', index };
  const latched = pad.mode === PadMode.TOGGLE && pad.kind !== PadKind.PROGRAM && pad.on;

  const button = el('button', {
    type: 'button',
    class: classes('pad', `hue-${pad.colour}`, latched && 'latched',
                   pad.kind === PadKind.PROGRAM && 'launch',
                   surface.armedOn(where) && 'armed'),
    'aria-pressed': pad.mode === PadMode.TOGGLE ? String(Boolean(pad.on)) : null,
    'aria-label': padSaid(app, pad, index),
    title: latched ? 'last sent: on — the module is not asked' : null,
  },
    el('span', { class: 'pad-name' }, padName(pad, index)),
    el('span', { class: 'pad-what' }, padWhat(app, pad)),
    // A toggle shows what it **last sent**, which is not the same as what the
    // module holds: bind a pad to a macro, sweep that macro with an LFO, and
    // this pad's belief is stale. So it is drawn as an outline rather than a
    // lit control, and says so. `state.diverged` and the keyboard's "taken by
    // N ports" are the same honesty: nothing here pretends to know.
    latched ? el('span', { class: 'pad-latch' }, 'last sent') : null);

  hold(button, {
    onHold: () => { surface.release(index); openAssign(app, where); },
    onDown: () => { surface.press(index); button.classList.add('hit'); return {}; },
    onUp: () => { surface.release(index); button.classList.remove('hit'); },
  });

  // A pad reached without a pointer: press and release, so a latch still
  // latches and a momentary pad does not hang.
  button.addEventListener('click', (e) => {
    if (e.detail !== 0) return;
    surface.press(index);
    setTimeout(() => surface.release(index), 120);
  });

  return button;
}

const padName = (pad, index) => pad.label || `${index + 1}`;

function padWhat(app, pad) {
  if (pad.kind === PadKind.PROGRAM) return `preset ${pad.program}`;
  if (pad.kind === PadKind.CC) return `CC ${pad.cc}`;
  return noteLabel(pad.note);
}

const NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const noteLabel = (pitch) => `${NAMES[pitch % 12]}${Math.floor(pitch / 12) - 1}`;

function padSaid(app, pad, index) {
  const what = padWhat(app, pad);
  const mode = pad.kind === PadKind.PROGRAM ? 'launch'
    : pad.mode === PadMode.TOGGLE ? 'latching' : 'momentary';
  return `pad ${index + 1}: ${what}, ${mode}`;
}

// --- the one gesture ----------------------------------------------------------

// Press to play, hold to say what this does. One gesture deep, on purpose: a
// stage view stays a stage view only while the thing under your thumb is the
// instrument and not a settings page.
function hold(node, { onHold, onDown = () => ({}), onMove = null, onUp = () => {} }) {
  let gesture = null;
  node.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    const start = { x: e.clientX, y: e.clientY, id: e.pointerId, held: false, ...onDown(e) };
    start.timer = setTimeout(() => {
      start.held = true;
      gesture = null;
      onHold();
    }, HOLD_MS);
    gesture = start;
  });
  node.addEventListener('pointermove', (e) => {
    if (!gesture || gesture.id !== e.pointerId) return;
    if (Math.abs(e.clientX - gesture.x) > MOVED_PX || Math.abs(e.clientY - gesture.y) > MOVED_PX) {
      clearTimeout(gesture.timer);
    }
    onMove?.(e, gesture);
  });
  for (const type of ['pointerup', 'pointercancel', 'pointerleave']) {
    node.addEventListener(type, (e) => {
      if (!gesture || gesture.id !== e.pointerId) return;
      clearTimeout(gesture.timer);
      gesture = null;
      onUp(e);
    });
  }
  // A right-click is the same question a long press asks, and it is how this
  // is reached with a mouse without holding still for half a second.
  node.addEventListener('contextmenu', (e) => { e.preventDefault(); onHold(); });
}

function openAssign(app, where) {
  app.state.ui.surface.editing = where;
  app.render();
}
