// The performance surface: the whole screen, sixteen pads and eight pots.
//
// The play button used to open a column of eight panels - meters, a clock, a
// scope, a piano roll, jacks, a keyboard, four sections of listener and a MIDI
// monitor. That is an instrument panel, and on stage you would scroll past an
// oscilloscope to reach a control. Those panels are still there, as the
// *module* tab, which is where an instrument panel belongs; this is what the
// play button opens.
//
// **It is drawn as a machine, not as a page.** A hardware controller has a
// case, a silkscreen and one display; it does not label every knob with a
// sentence about what it is bound to. So:
//
//   * **One readout, not eight numbers.** A knob shows its position as a
//     pointer, the way a knob does. The number of whatever you are touching
//     goes to the strip at the top, which is the only place a number is read
//     from - and between gestures that strip says what patch is loaded and at
//     what tempo, which is what you want to see when your hands are still.
//   * **Silkscreen, not prose.** One short line per control, in the panel
//     type: its caption, or what it sends when it has no caption. Whether it
//     is bound is a lamp, not the words "not bound" under all eight.
//   * **Nothing is rebuilt under your thumb.** Pressing a pad writes to the
//     DOM directly and re-renders nothing: a latch that rebuilt the page
//     would drop the gesture that set it and flash every other control.
//
// **It fits 360x640 with no page scroll, and that constraint decides the
// counts rather than the other way round.** The shell is a grid of fixed rows
// with the pads taking whatever is left: nothing here is allowed to grow a
// scrollbar, because a control you have to scroll to is a control you do not
// have.
//
// **Fixed geometry, soft assignment** (services/surface.js): you cannot move a
// control, and you can change everything about what it does. Press to play,
// hold to ask.

import * as P from '../../protocol/generated.js';
import { el, classes } from '../dom.js';
import { icon } from '../components/icons.js';
import { KeyboardOverlay } from '../components/Keyboard.js';
import { closeMenu } from '../components/Menu.js';
import { PadKind, PadMode, POTS, PADS } from '../../services/surface.js';
import { describeTarget } from '../panels/ModMatrix.js';
import { MUSICAL_PORTS } from '../../protocol/names.js';
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

// A pad you can feel. A phone is the one place a "hardware" control has no
// travel at all, and a few milliseconds of buzz is the whole difference
// between a pad and a picture of one.
const TAP_MS = 8;
const buzz = () => { try { navigator.vibrate?.(TAP_MS); } catch { /* not everywhere */ } };

export function Surface(app) {
  closeMenu();
  const ui = app.state.ui.surface;
  const display = Readout(app);
  return el('div', { class: 'surface' },
    TopBar(app, display),
    Pots(app, display.say),
    Pads(app, display.say),
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

// --- the display ---------------------------------------------------------------

// The one number on the surface. It holds whatever was last touched until
// something else is, exactly as a hardware display does - it does not fade
// back after a second, because a value that vanishes while you are still
// looking at it is worse than one that is a moment stale. With nothing
// touched yet it says what is loaded and how fast it is going.
function Readout(app) {
  const name = el('span', { class: 'readout-name' }, app.state.current.name);
  const value = el('span', { class: 'readout-value' }, `${app.state.globals.bpm} BPM`);
  const node = el('div', { class: 'readout', role: 'status', 'aria-live': 'off' }, name, value);
  return {
    node,
    say(what, reading) {
      name.textContent = what;
      value.textContent = reading;
    },
  };
}

function TopBar(app, display) {
  const b = app.surface.budget();
  const machine = app.play.machine();
  const cable = MUSICAL_PORTS.find((p) => p.value === app.play.port)?.label ?? '';
  const armed = app.state.ui.surface.armed;

  const panelButton = ({ glyph, label, on = false, onclick, klass = '' }) => el('button', {
    type: 'button', class: classes('panel-btn', klass, on && 'on'),
    'aria-label': label, 'aria-pressed': on ? 'true' : 'false', title: label, onclick,
  }, icon(glyph));

  return el('header', { class: 'surface-top' },
    // Which machine is live, which cable it arrives on, and what the binding
    // table has left. Hitting a pad and hearing nothing because the browser
    // is playing to itself is the failure this line exists to prevent, so it
    // is a statement rather than something to go and look up.
    el('div', { class: classes('strip', !machine.ok && 'warn') },
      el('span', { class: classes('lamp', machine.ok && 'lit') }),
      el('span', { class: 'strip-machine' }, machine.kind === 'module' ? 'built-in' : machine.name),
      el('span', { class: 'strip-sep' }, '·'),
      el('span', {}, machine.ok ? cable : machine.reaches),
      el('span', { class: classes('strip-budget', !b.free && 'spent') }, `cc ${b.bindings}/${b.bindingSlots}`)),
    el('div', { class: 'surface-face' },
      panelButton({ glyph: 'edit', label: 'leave the surface and go back to editing',
                    onclick: () => app.togglePlay() }),
      display.node,
      armed
        ? el('button', { class: 'panel-btn learning', onclick: () => app.surface.disarm() },
             el('span', {}, 'learn'))
        : panelButton({
            glyph: 'key', label: 'summon the keyboard',
            on: app.state.ui.surface.keyboard,
            onclick: () => {
              app.state.ui.surface.keyboard = !app.state.ui.surface.keyboard;
              app.render();
            },
          })));
}

// --- the pots ----------------------------------------------------------------

function Pots(app, say) {
  return el('div', { class: 'pots' },
    Array.from({ length: POTS }, (_, i) => Pot(app, i, say)));
}

function Pot(app, index, say) {
  const surface = app.surface;
  const pot = surface.pot(index);
  const where = { kind: 'pot', index };
  const bound = surface.bindingOf(pot);
  const live = liveMacro(app, bound);
  const name = potName(app, pot, bound);

  const cap = el('div', { class: 'knob-cap' });
  const dial = el('div', {
    class: classes('knob', `hue-${pot.colour}`, live !== null && 'reading'),
    role: 'slider', tabindex: '0',
    'aria-label': `${name}, ${bound ? describeTarget(app, bound.mapping) : 'not bound'}`,
    'aria-valuemin': '0', 'aria-valuemax': '127', 'aria-valuenow': String(pot.value),
    style: `--at: ${pot.value / 127}`,
  }, cap);

  // The knob paints itself while it is being turned, and the number goes to
  // the readout: a render would replace the element under the finger, which
  // is the rule every other continuous control in the app follows
  // (components/Slider.js).
  const paint = (value) => {
    dial.style.setProperty('--at', String(value / 127));
    dial.setAttribute('aria-valuenow', String(value));
    say(name, String(value));
  };

  hold(dial, {
    onHold: () => openAssign(app, where),
    onDown: (e) => {
      dial.setPointerCapture(e.pointerId);
      dial.classList.add('turning');
      say(name, String(pot.value));
      return { y: e.clientY, from: pot.value };
    },
    onMove: (e, start) => paint(surface.turn(index, start.from - (e.clientY - start.y) / PIXELS_PER_STEP)),
    onUp: () => {
      dial.classList.remove('turning');
      surface.turn(index, pot.value, { commit: true });
    },
  });

  dial.addEventListener('keydown', (e) => {
    const step = { ArrowUp: 1, ArrowRight: 1, ArrowDown: -1, ArrowLeft: -1 }[e.key];
    const to = e.key === 'Home' ? 0 : e.key === 'End' ? 127 : null;
    if (step === undefined && to === null) return;
    e.preventDefault();
    paint(surface.turn(index, to ?? pot.value + step * (e.shiftKey ? 10 : 1), { commit: true }));
  });

  // A pot on a macro is the one control here that genuinely reads back: the
  // module says where the macro is (SYSEX_MACRO_STATE), so the mark follows
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
    el('div', { class: 'pot-label' },
      el('span', { class: classes('lamp', bound && 'lit') }),
      el('span', { class: 'silk' }, name)));
}

// What the panel is silkscreened with: the caption somebody wrote, or what
// the control sends. Never a sentence - the eight of them are read at a
// glance or not at all.
function potName(app, pot, bound) {
  if (pot.label) return pot.label;
  if (bound?.mapping.targetKind === P.CcTargetKind.CC_TARGET_MACRO) {
    return app.state.patch.macros?.[bound.mapping.targetIndex]?.name || `macro ${bound.mapping.targetIndex + 1}`;
  }
  return `cc ${pot.cc}`;
}

// The macro a control is driving, or null. Only a macro reads back; every
// other target is written and not watched.
function liveMacro(app, bound) {
  if (!bound) return null;
  return bound.mapping.targetKind === P.CcTargetKind.CC_TARGET_MACRO
    ? bound.mapping.targetIndex : null;
}

// --- the pads ----------------------------------------------------------------

function Pads(app, say) {
  return el('div', { class: 'pads' },
    Array.from({ length: PADS }, (_, i) => Pad(app, i, say)));
}

function Pad(app, index, say) {
  const surface = app.surface;
  const pad = surface.pad(index);
  const where = { kind: 'pad', index };
  const latches = pad.mode === PadMode.TOGGLE && pad.kind !== PadKind.PROGRAM;

  // A toggle shows what it **last sent**, which is not the same as what the
  // module holds: bind a pad to a macro, sweep that macro with an LFO, and
  // this pad's belief is stale. So it is drawn as a ring rather than a lit
  // cell, and says so in the one place it matters. `state.diverged` and the
  // keyboard's "taken by N ports" are the same honesty: nothing here
  // pretends to know.
  const latch = el('span', { class: 'pad-latch' }, 'last sent');
  latch.hidden = !(latches && pad.on);

  const button = el('button', {
    type: 'button',
    class: classes('pad', `hue-${pad.colour}`, latches && pad.on && 'latched',
                   pad.kind === PadKind.PROGRAM && 'launch',
                   surface.armedOn(where) && 'armed'),
    'aria-pressed': latches ? String(Boolean(pad.on)) : null,
    'aria-label': padSaid(pad, index),
    title: latches ? 'a latch shows what it last sent — the module is not asked' : null,
  },
    el('span', { class: 'pad-index' }, String(index + 1)),
    el('span', { class: 'pad-name' }, pad.label || padWhat(pad)),
    // The second line only when it says something the first does not: a pad
    // captioned "kick" needs to say it sends C2, and one showing C2 does not
    // need the word "note" under it sixteen times.
    padUnder(pad) ? el('span', { class: 'pad-what silk' }, padUnder(pad)) : null,
    latch);

  // Pressed, and nothing rebuilt: the latch and the lamp are written straight
  // into this element. A render here would replace the pad mid-gesture.
  const show = () => {
    button.classList.toggle('latched', latches && pad.on);
    latch.hidden = !(latches && pad.on);
    button.setAttribute('aria-pressed', latches ? String(Boolean(pad.on)) : null);
  };

  const press = () => {
    buzz();
    button.classList.add('hit');
    say(pad.label || padWhat(pad), surface.press(index));
    show();
  };
  const release = () => {
    button.classList.remove('hit');
    surface.release(index);
    show();
  };

  hold(button, {
    onHold: () => { release(); openAssign(app, where); },
    onDown: () => { press(); return {}; },
    onUp: () => release(),
  });

  // A pad reached without a pointer: press and release, so a latch still
  // latches and a momentary pad does not hang.
  button.addEventListener('click', (e) => {
    if (e.detail !== 0) return;
    press();
    setTimeout(release, 120);
  });

  return button;
}

function padWhat(pad) {
  if (pad.kind === PadKind.PROGRAM) return `preset ${pad.program}`;
  if (pad.kind === PadKind.CC) return `cc ${pad.cc}`;
  return noteLabel(pad.note);
}

const padUnder = (pad) => (pad.label ? padWhat(pad)
  : pad.kind === PadKind.PROGRAM ? 'launch' : null);

const NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const noteLabel = (pitch) => `${NAMES[pitch % 12]}${Math.floor(pitch / 12) - 1}`;

function padSaid(pad, index) {
  const mode = pad.kind === PadKind.PROGRAM ? 'launch'
    : pad.mode === PadMode.TOGGLE ? 'latching' : 'momentary';
  return `pad ${index + 1}: ${pad.label ? `${pad.label}, ` : ''}${padWhat(pad)}, ${mode}`;
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
