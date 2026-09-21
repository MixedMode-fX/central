// The globals: everything the module comes up with that is not a node, in
// one page. It is `GlobalSettings` in `src/patch/patch_codec.h` drawn — the
// key, the clock, Program Change recall and NRPN — and nothing else, which
// is what makes it a page rather than a drawer: a setting is here exactly
// when it travels with the patch and belongs to no node.
//
// They were spread over three tabs, which put the two most global things in
// the machine — what key it is in and what it counts time by — a tab apart,
// and buried recall and NRPN under a MIDI tab that is otherwise about the
// cables in the room.
//
// **The keyboard is the point of the first panel.** A scale chosen from a
// list is a word; a scale on a keyboard is the thing itself — which notes
// are in it, where the semitones fall, and what happens to all of it when
// the root moves. Press a key to move the root.

import * as P from '../../protocol/generated.js';
import { el, classes } from '../dom.js';
import { Panel, Hint } from '../components/Panel.js';
import { Field, Fields } from '../components/Field.js';
import { Select, range } from '../components/Select.js';
import { NumberField } from '../components/NumberField.js';
import { Switch } from '../components/Switch.js';
import { ChannelSelect } from '../controls/ChannelSelect.js';
import { PortToggles } from '../controls/PortToggles.js';
import {
  SCALES, scaleMaskById, CLOCK_SOURCES, CLOCK_MIDI_SOURCE, SWAP_TIMINGS, portNames,
} from '../../protocol/names.js';
import {
  keySpelling, noteName, registerNote, hasSharpAbove, WHITE_PITCH_CLASSES,
  DEFAULT_KEY_OCTAVE, MAX_KEY_OCTAVE,
} from '../../core/music.js';
import './Globals.css';

// Two octaves from C, which is enough for the shape of any scale to repeat
// and short enough to fit a phone. Which notes are white, and which of them a
// black key sits above, are the keyboard arithmetic every drawing of one
// shares (core/music.js).
const OCTAVES = 2;

// The clock's three settings, side by side at every width (Globals.css).
const ClockRow = (...fields) => el('div', { class: 'fields clock' }, ...fields);

// What sets the tempo once the clock is following something else, because it
// is then not the field that says "tempo": a number that looks live and is
// not is worse than no number at all.
const CLOCK_DRIVEN = {
  1: 'the tempo comes from the sync jack; the field above is the module’s own rate',
  2: 'the tempo comes from the MIDI clock on the cables it follows; the field above is the module’s own rate',
};

export function GlobalsTab(app) {
  if (!app.device?.capabilities) return Hint('no module');
  return el('div', {}, KeyPanel(app), ClockPanel(app), RecallPanel(app), NrpnPanel(app));
}

// --- the key ----------------------------------------------------------------

// Where each pitch class sits in the scale: 0 when it is not in it, otherwise
// its degree counted from the root.
function degrees(mask, root) {
  const of = new Array(12).fill(0);
  let n = 0;
  for (let step = 0; step < 12; step++) {
    if (!((mask >> step) & 1)) continue;
    n += 1;
    of[(root + step) % 12] = n;
  }
  return of;
}

// Every key is a button: pressing one moves the root, which is the fastest
// way to say "put this in F" and the only one that shows what that does to
// the rest of the notes before you commit to it.
function Keyboard({ root, mask, spelling, setRoot }) {
  const degree = degrees(mask, root);
  const step = 100 / (OCTAVES * WHITE_PITCH_CLASSES.length);
  const key = (pitchClass, black, left) => {
    const d = degree[pitchClass];
    const name = spelling[pitchClass];
    return el('button', {
      type: 'button',
      class: classes('kb-key', black ? 'kb-black' : 'kb-white', d && 'in', pitchClass === root && 'root'),
      style: black ? `left:${left}%` : null,
      title: d ? `${name}: degree ${d} of the key` : `${name}: not in the key`,
      'aria-label': `root ${name}`,
      'aria-pressed': pitchClass === root ? 'true' : 'false',
      onclick: () => setRoot(pitchClass),
    }, el('span', { class: 'kb-label' }, d ? String(d) : ''));
  };
  const keys = [];
  let index = 0;
  for (let octave = 0; octave < OCTAVES; octave++) {
    for (const pitchClass of WHITE_PITCH_CLASSES) {
      keys.push(key(pitchClass, false, 0));
      // The black key above this one, centred on the border it straddles.
      if (hasSharpAbove(pitchClass)) keys.push(key((pitchClass + 1) % 12, true, (index + 1) * step));
      index += 1;
    }
  }
  return el('div', { class: 'keyboard', role: 'group', 'aria-label': 'the notes of the key' }, keys);
}

// One scale, one root, one register, for the whole patch. Not a MIDI setting:
// a key is the most musical decision in the patch.
export function KeyPanel(app) {
  const g = app.state.globals;
  const set = (changes) => app.editor.setGlobals(changes, 'key');
  const root = g.root ?? 0;
  const mask = scaleMaskById(g.scale);
  const degree = degrees(mask, root);
  // Spelled by the key, like everything else here: a keyboard reading E flat
  // over a list reading D sharp is two answers to one question.
  const spelling = keySpelling(root, mask);
  // A stored zero is a byte nobody set, and the firmware reads it as the
  // default register - so the page shows the register that is playing.
  const octave = g.rootOctave || DEFAULT_KEY_OCTAVE;
  const notes = [];
  for (let i = 0; i < 12; i++) {
    const pitchClass = (root + i) % 12;
    if (degree[pitchClass]) notes.push(spelling[pitchClass]);
  }

  return Panel('the key',
    Keyboard({ root, mask, spelling, setRoot: (pitchClass) => set({ root: pitchClass }) }),
    el('p', { class: 'hint kb-notes' },
      `${spelling[root]} ${SCALES.find((s) => s.value === g.scale)?.label ?? ''} — ${notes.join(' ')}`),
    Fields(
      Field({ label: 'scale', hint: 'which notes' }, Select({
        options: SCALES, value: g.scale, onChange: (scale) => set({ scale }),
      })),
      Field({ label: 'root', hint: 'which of them is home' }, Select({
        options: range(12, (pc) => spelling[pc]), value: root, onChange: (chosen) => set({ root: chosen }),
      })),
      // The register: where the key's root sits as a pitch, and so where
      // every node that names no octave of its own plays.
      Field({ label: 'register', hint: 'where home sits' }, Select({
        options: range(MAX_KEY_OCTAVE + 1, (o) => {
          const note = registerNote(o, root);
          return `${o} — ${noteName(note)}, note ${note}`;
        }, 1),
        value: octave, onChange: (rootOctave) => set({ rootOctave }),
      }))),
    Hint('Every node plays in this key: nothing in the patch names a scale or a root of its own. '
      + 'A node’s "octave" says which register it plays in, and "key" — its default — is the one '
      + 'named here, so one setting moves the whole patch and a part that has been placed keeps '
      + 'its place. Add a Key module to move the root from a note bus, or bind a controller to '
      + 'the key in the mod matrix, on the patch tab.'));
}

// --- the clock -------------------------------------------------------------

// What the module counts time by, and the cables the clock is routed over.
//
// **What it runs on is one row** — the source, the tempo and the CV rate —
// on a phone as much as on a desk: those three are read together and set
// together, and a column of them pushed the cables off the screen. The
// labels are short because the row is narrow, and the control says the rest.
//
// **The two masks are not MIDI routing rows**, and they are here rather than
// in that panel because of it: clock, start, stop and continue are
// transport-level and reach no note bus (src/patch/patch_codec.h), so there
// is no cable to draw them on. What is left is the pair of questions a user
// actually asks - which host this module follows, and what it clocks in turn.
export function ClockPanel(app) {
  const g = app.state.globals;
  const set = (changes) => app.editor.setGlobals(changes);
  const route = (changes) => app.editor.setClockRoute(changes);
  // The same cable in both masks with MIDI as the source is the module
  // clocking itself: what arrives on that wire goes straight back out of it.
  // Said rather than refused - on two DIN sockets it is a chain, and only the
  // user can see which end of the cable is which.
  const loop = g.clockSource === CLOCK_MIDI_SOURCE && (g.clockInMask & g.clockOutMask);
  return Panel('clock',
    ClockRow(
      Field({ label: 'source' }, Select({
        options: CLOCK_SOURCES, value: g.clockSource, 'aria-label': 'what the clock follows',
        onChange: (clockSource) => set({ clockSource }),
      })),
      Field({ label: 'tempo' }, NumberField({
        value: g.bpm, min: P.CLOCK_MIN_BPM, max: P.CLOCK_MAX_BPM, fallback: P.CLOCK_DEFAULT_BPM,
        'aria-label': `tempo in BPM, ${P.CLOCK_MIN_BPM} to ${P.CLOCK_MAX_BPM}`,
        onChange: (bpm) => set({ bpm }),
      })),
      Field({ label: 'CV PPQN' }, NumberField({
        value: g.cvPpqn, min: 1, max: 96, fallback: 4, 'aria-label': 'CV pulses per quarter note',
        onChange: (cvPpqn) => set({ cvPpqn }),
      }))),
    CLOCK_DRIVEN[g.clockSource] ? Hint(CLOCK_DRIVEN[g.clockSource]) : null,
    Fields(
      Field({ label: 'follows these ports', hint: 'none = any' }, PortToggles({
        mask: g.clockInMask, label: 'clock input ports',
        onChange: (clockInMask) => route({ clockInMask }),
      })),
      Field({ label: 'sends clock to', hint: 'none = nowhere' }, PortToggles({
        mask: g.clockOutMask, label: 'clock output ports',
        onChange: (clockOutMask) => route({ clockOutMask }),
      }))),
    loop ? Hint(`${portNames(g.clockInMask & g.clockOutMask).join(', ')} both follows and sends: on one cable that is the module clocking itself`) : null);
}

// --- Program Change recall --------------------------------------------------

// How the module is told to load another patch: whether it listens at all,
// and where a recall it hears lands. The pads that send one are on the
// surface; this is the module's side of the same conversation.
export function RecallPanel(app) {
  const g = app.state.globals;
  const set = (changes) => app.editor.setGlobals(changes);
  return Panel('patch recall',
    Fields(
      Field({ label: 'Program Change recalls presets' }, Switch({
        checked: g.pcEnabled !== 0, label: g.pcEnabled ? 'on' : 'off',
        onChange: (on) => set({ pcEnabled: on ? 1 : 0 }),
      })),
      Field({ label: 'listens on' }, ChannelSelect({ value: g.pcChannel, onChange: (pcChannel) => set({ pcChannel }) })),
      Field({ label: 'a recall lands' }, Select({
        options: SWAP_TIMINGS, value: g.pcQuantise, onChange: (pcQuantise) => set({ pcQuantise }),
      }))),
    Fields(
      Field({ label: 'from these ports', hint: 'none = any' }, PortToggles({
        mask: g.pcSourceMask, label: 'Program Change source ports',
        onChange: (pcSourceMask) => set({ pcSourceMask }),
      }))));
}

// --- NRPN --------------------------------------------------------------------

// A second transport for the bindings a CC already reaches: fourteen bits,
// and its own channel and cables to arrive on.
export function NrpnPanel(app) {
  const g = app.state.globals;
  const nrpn = (changes) => app.editor.setNrpn(changes);
  return Panel('NRPN',
    Fields(
      Field({ label: 'accept NRPN' }, Switch({
        checked: g.nrpnEnabled !== 0, label: g.nrpnEnabled ? 'on' : 'off',
        onChange: (on) => nrpn({ nrpnEnabled: on ? 1 : 0 }),
      })),
      Field({ label: 'on channel' }, ChannelSelect({ value: g.nrpnChannel, onChange: (nrpnChannel) => nrpn({ nrpnChannel }) }))),
    Fields(
      Field({ label: 'from these ports', hint: 'none = any' }, PortToggles({
        mask: g.nrpnSourceMask, label: 'NRPN source ports',
        onChange: (nrpnSourceMask) => nrpn({ nrpnSourceMask }),
      }))));
}
