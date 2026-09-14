// The key the module is in: one scale, one root, one register, for the whole
// patch. Not a MIDI setting: a key is the most musical decision in the patch.
//
// **The keyboard is the point of the page.** A scale chosen from a list is a
// word; a scale on a keyboard is the thing itself - which notes are in it,
// where the semitones fall, and what happens to all of it when the root
// moves. Press a key to move the root.

import { el, classes } from '../dom.js';
import { Panel, Hint } from '../components/Panel.js';
import { Field, Fields } from '../components/Field.js';
import { Select, range } from '../components/Select.js';
import { SCALES, scaleMaskById } from '../../protocol/names.js';
import {
  keySpelling, noteName, registerNote, hasSharpAbove, WHITE_PITCH_CLASSES,
  DEFAULT_KEY_OCTAVE, MAX_KEY_OCTAVE,
} from '../../core/music.js';
import './Key.css';

// Two octaves from C, which is enough for the shape of any scale to repeat
// and short enough to fit a phone. Which notes are white, and which of them a
// black key sits above, are the keyboard arithmetic every drawing of one
// shares (core/music.js).
const OCTAVES = 2;

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

export function KeyTab(app) {
  if (!app.device?.capabilities) return Hint('no module');
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
      + 'the key under MIDI.'));
}
