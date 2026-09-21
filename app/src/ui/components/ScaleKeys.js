// A key, drawn as a keyboard.
//
// A scale chosen from a list is a word; a scale on a keyboard is the thing
// itself - which notes are in it, where the semitones fall, and what happens
// to all of it when the root moves. Two panels need that picture: the globals
// tab, where the key is *set*, and the Key node, where a note bus is moving
// it and the only useful thing a card can show is where it has got to.
//
// `onRoot` is what tells the two apart. Given one, every key is a button and
// pressing it moves the root, which is the fastest way to say "put this in F"
// and the only one that shows what that does to the rest of the notes before
// you commit to it. Left out, the keys are a picture: on the Key node the
// root is the cable's to move, and a second way to set it on the same card
// would be a second writer of the one value the node exists to own.
//
// This is not the playable keyboard (components/Keyboard.js). That one sends
// notes and lives on the surface; this one draws a key and sends nothing.

import { el, classes } from '../dom.js';
import { scaleDegrees, hasSharpAbove, WHITE_PITCH_CLASSES } from '../../core/music.js';
import './ScaleKeys.css';

// Two octaves from C, which is enough for the shape of any scale to repeat
// and short enough to fit a phone. Which notes are white, and which of them a
// black key sits above, are the keyboard arithmetic every drawing of one
// shares (core/music.js).
const OCTAVES = 2;

export function ScaleKeys({ root, mask, spelling, onRoot = null, label = 'the notes of the key' }) {
  const degree = scaleDegrees(mask, root);
  const step = 100 / (OCTAVES * WHITE_PITCH_CLASSES.length);
  const key = (pitchClass, black, left) => {
    const d = degree[pitchClass];
    const name = spelling[pitchClass];
    const said = d ? `${name}: degree ${d} of the key` : `${name}: not in the key`;
    const attrs = {
      class: classes('kb-key', black ? 'kb-black' : 'kb-white', d && 'in', pitchClass === root && 'root'),
      style: black ? `left:${left}%` : null,
      title: said,
    };
    const face = el('span', { class: 'kb-label' }, d ? String(d) : '');
    // A picture is a `<div>` and a control is a `<button>`: a button that
    // does nothing is a promise to a pointer and to a screen reader that the
    // card cannot keep.
    return onRoot
      ? el('button', { type: 'button', ...attrs,
                       'aria-label': `root ${name}`,
                       'aria-pressed': pitchClass === root ? 'true' : 'false',
                       onclick: () => onRoot(pitchClass) }, face)
      : el('div', { ...attrs, role: 'img', 'aria-label': said }, face);
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
  return el('div', { class: classes('keyboard', !onRoot && 'reading'),
                     role: 'group', 'aria-label': label }, keys);
}
