// The key the module is in: one scale, one root, one register, for the whole
// patch.
//
// It is a page of its own because it is not a MIDI setting and never was. It
// sat under "MIDI" beside the clock and the Program Change filters for the
// same reason a spare drawer fills up - the panel that pushed GlobalSettings
// happened to live there - and a key is the most musical decision in the
// patch, not a transport detail. It is now the first thing beside the patch
// itself.
//
// **There is nowhere else it is set.** No algorithm carries a scale, a root
// or a key parameter any more (src/midi/global_key.h): three ways for a node
// to leave the key it was in made the one decision a musician actually makes
// into twenty-four parameters that had to agree. What a node still chooses is
// the register it plays in, on its own page, because a bass line and a lead
// are the same key two octaves apart.
//
// **The keyboard is the point of the page.** A scale chosen from a list is a
// word; a scale on a keyboard is the thing itself - which notes are in it,
// where the semitones fall, and what happens to all of it when the root
// moves. Press a key to move the root.

import { el, noteName } from './views.js';
import { KEY_SCALES, PITCH_CLASSES, scaleMaskById, keySpelling,
         DEFAULT_KEY_OCTAVE, MAX_KEY_OCTAVE } from './names.js';

// Two octaves from C, which is enough for the shape of any scale to repeat
// and short enough to fit a phone. The white pitch classes in order; a black
// key sits above every white one that is not E or B.
const WHITE = [0, 2, 4, 5, 7, 9, 11];
const OCTAVES = 2;
const SHARP_ABOVE = new Set([0, 2, 5, 7, 9]);

// The note name of a pitch class, and where it sits in the scale: 0 when it
// is not in it, otherwise its degree counted from the root.
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

// The keyboard. Every key is a button: pressing one moves the root, which is
// the fastest way to say "put this in F" and the only one that shows what
// that does to the rest of the notes before you commit to it.
function keyboard(root, mask, setRoot) {
  const degree = degrees(mask, root);
  // Named the way this key names them: the third of C minor is E flat on the
  // keyboard as well as in the list under it, and a piano with one spelling
  // and a sentence with another is two answers to one question.
  const spelling = keySpelling(root, mask);
  const white = OCTAVES * WHITE.length;
  const step = 100 / white;

  const key = (pitchClass, black, left) => {
    const d = degree[pitchClass];
    const classes = ['kb-key', black ? 'kb-black' : 'kb-white'];
    if (d) classes.push('in');
    if (pitchClass === root) classes.push('root');
    const name = spelling[pitchClass];
    return el('button', {
      type: 'button',
      class: classes.join(' '),
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
    for (const pitchClass of WHITE) {
      keys.push(key(pitchClass, false, 0));
      // The black key above this one, centred on the border it straddles.
      if (SHARP_ABOVE.has(pitchClass)) keys.push(key((pitchClass + 1) % 12, true, (index + 1) * step));
      index += 1;
    }
  }
  return el('div', { class: 'keyboard', role: 'group', 'aria-label': 'the notes of the key' }, keys);
}

export function keyPanel(app) {
  if (!app.device?.capabilities) return null;
  const g = app.globals;
  const push = () => { app.edit(() => app.device.setGlobals(g), 'key'); app.render(); };

  const root = g.root ?? 0;
  const mask = scaleMaskById(g.scale);
  const degree = degrees(mask, root);
  const spelling = keySpelling(root, mask);
  // A stored zero is a byte nobody set, and the firmware reads it as the
  // default register - so the page shows the register that is playing.
  const octave = g.rootOctave || DEFAULT_KEY_OCTAVE;

  const scale = el('select', { onchange: (e) => { g.scale = Number(e.target.value); push(); } });
  for (const s of KEY_SCALES) {
    const option = el('option', { value: String(s.value) }, s.label);
    if (s.value === g.scale) option.selected = true;
    scale.append(option);
  }

  // Spelled by the key it is choosing for, like everything else here: a
  // keyboard reading E flat over a list reading D sharp is two answers to one
  // question. The twelve are all offered whatever the key does with them.
  const rootSelect = el('select', { onchange: (e) => { g.root = Number(e.target.value); push(); } });
  PITCH_CLASSES.forEach((_, pitchClass) => {
    const option = el('option', { value: String(pitchClass) }, spelling[pitchClass]);
    if (pitchClass === root) option.selected = true;
    rootSelect.append(option);
  });

  // The register: where the key's root sits as a pitch, and so where every
  // node that names no octave of its own plays.
  const register = el('select', { onchange: (e) => { g.rootOctave = Number(e.target.value); push(); } });
  for (let o = 1; o <= MAX_KEY_OCTAVE; o++) {
    const note = Math.min(127, o * 12 + root);
    const option = el('option', { value: String(o) }, `${o} — ${noteName(note)}, note ${note}`);
    if (o === octave) option.selected = true;
    register.append(option);
  }

  const field = (name, control, hint) => el('div', { class: 'field' },
    el('span', { class: 'field-name' }, name), control,
    hint ? el('span', { class: 'hint' }, hint) : null);

  // The notes, spelled out: the keyboard says where they are and this says
  // what they are called, which is what a musician writes down.
  const notes = [];
  for (let i = 0; i < 12; i++) {
    const pitchClass = (root + i) % 12;
    if (degree[pitchClass]) notes.push(spelling[pitchClass]);
  }

  return el('section', { class: 'panel' },
    el('h2', {}, 'the key'),
    keyboard(root, mask, (pitchClass) => { g.root = pitchClass; push(); }),
    el('p', { class: 'hint kb-notes' },
      `${spelling[root]} ${KEY_SCALES.find((s) => s.value === g.scale)?.label ?? ''}`
      + ` — ${notes.join(' ')}`),
    el('div', { class: 'fields' },
      field('scale', scale, 'which notes'),
      field('root', rootSelect, 'which of them is home'),
      field('register', register, 'where home sits')),
    el('p', { class: 'hint' },
      'Every node plays in this key: nothing in the patch names a scale or a root of its own. '
      + 'A node’s "octave" says which register it plays in, and "key" — its default — is the one '
      + 'named here, so one setting moves the whole patch and a part that has been placed keeps '
      + 'its place. Add a Key module to move the root from a note bus, or bind a controller to '
      + 'the key under MIDI.'));
}
