// A keyboard you can actually play.
//
// The app used to draw one three times, no two the same: thirteen keys inline
// in the play panel, two octaves of pitch classes on the key page, and a
// canvas gutter down the side of the piano roll. Only the first one played
// anything, and at phone width it stopped being a keyboard at all - the keys
// wrapped onto three rows, because a wrapping flex row is what a keyboard
// becomes when it is put in a one-column panel and told to fit.
//
// So: one component, and the three faults fixed where they were.
//
//   * **It scrolls; it never wraps.** The track is one row however narrow the
//     viewport is, and the register is reached by scrolling it or by the
//     octave strip above it - which is why this belongs full-width in an
//     overlay rather than inside a panel.
//   * **A drag plays a glissando.** Keys are hit by where the pointer *is*,
//     not by which element the gesture started on, so dragging across the
//     keyboard releases the old note and starts the new one. That is what the
//     `touch-action: none` on a key was always for; until now it was a
//     comment describing an intention.
//   * **Enter and Space play a note.** The keys were buttons with pointer
//     handlers and no click handler, so the whole keyboard was silent to
//     anyone not using a pointer.
//
// **It does not know where notes go.** It takes callbacks; the caller decides
// whether that is the module in the page or one on a cable (services/play.js).

import { el, classes } from '../dom.js';
import { icon } from './icons.js';
import { noteName, octaveOf, isBlackKey } from '../../core/music.js';
import './Keyboard.css';

// Six octaves, C1 to C7: everything a part is written in, and short enough
// that the whole track is a few hundred elements rather than a thousand.
export const LOW_NOTE = 24;
export const HIGH_NOTE = 96;

// A key pressed from the keyboard rather than by a pointer has no gesture to
// end it, so it is a blip: long enough to hear, short enough not to hang.
const TAP_MS = 180;

// Velocity by where in the key the finger landed: the top of a key is the
// quiet end, as it is on a weighted controller. The number field is the
// velocity at the bottom, so a player who never touches this gets exactly
// what the field says.
const SOFTEST = 0.45;

const velocityAt = (key, clientY, loudest) => {
  const box = key.getBoundingClientRect();
  if (!box.height) return loudest;
  const down = Math.min(1, Math.max(0, (clientY - box.top) / box.height));
  return Math.max(1, Math.min(127, Math.round(loudest * (SOFTEST + (1 - SOFTEST) * down))));
};

// The white keys of the range, in order, so a black key knows which border it
// straddles.
function layout(from, to) {
  const whites = [];
  const blacks = [];
  for (let pitch = from; pitch <= to; pitch++) {
    if (isBlackKey(pitch)) blacks.push({ pitch, after: whites.length - 1 });
    else whites.push({ pitch });
  }
  return { whites, blacks };
}

// `onNoteOn(pitch, velocity)` and `onNoteOff(pitch)` are the whole contract.
// `velocity` is the loud end of the key; `octave` says where the track is
// scrolled to when it is first drawn.
export function Keyboard({
  from = LOW_NOTE, to = HIGH_NOTE, octave = 4, velocity = 100,
  onNoteOn, onNoteOff, onOctave = null, label = 'keyboard', mounted = (fn) => fn(),
}) {
  const { whites, blacks } = layout(from, to);
  const keys = new Map();                 // pitch -> element, for the glide
  const held = new Set();

  const makeKey = (pitch, black, after) => {
    const key = el('button', {
      type: 'button',
      class: classes('pk', black ? 'pk-black' : 'pk-white', pitch % 12 === 0 && 'pk-c'),
      style: black ? `left: calc(${after + 1} * var(--pk-w))` : null,
      'aria-label': noteName(pitch),
      'data-pitch': String(pitch),
    }, el('span', { class: 'pk-name' }, pitch % 12 === 0 ? noteName(pitch) : ''));
    // A pointer is handled by the track, which is what makes a glide
    // possible; a click with no pointer behind it is a keyboard press, and
    // `detail` is zero exactly then.
    key.addEventListener('click', (e) => {
      if (e.detail !== 0) return;
      onNoteOn(pitch, velocity);
      setTimeout(() => onNoteOff(pitch), TAP_MS);
    });
    keys.set(pitch, key);
    return key;
  };

  const track = el('div', { class: 'pk-track', role: 'group', 'aria-label': label },
    whites.map(({ pitch }) => makeKey(pitch, false, 0)),
    blacks.map(({ pitch, after }) => makeKey(pitch, true, after)));

  // --- the gesture ----------------------------------------------------------
  // Which key is under the pointer, not which key the gesture started on: a
  // touch is captured to the element it began on, so `pointerenter` on the
  // keys never fires during a drag and a glissando needs the hit test.
  const at = (x, y) => {
    const found = document.elementFromPoint(x, y)?.closest?.('.pk');
    const pitch = found ? Number(found.dataset.pitch) : null;
    return pitch !== null && keys.has(pitch) ? { key: found, pitch } : null;
  };

  const press = (pitch, key, clientY) => {
    if (held.has(pitch)) return;
    held.add(pitch);
    key.classList.add('held');
    onNoteOn(pitch, velocityAt(key, clientY, velocity));
  };

  const release = (pitch) => {
    if (!held.delete(pitch)) return;
    keys.get(pitch)?.classList.remove('held');
    onNoteOff(pitch);
  };

  const releaseAll = () => { for (const pitch of [...held]) release(pitch); };

  let gesture = null;
  track.addEventListener('pointerdown', (e) => {
    const hit = at(e.clientX, e.clientY);
    if (!hit) return;
    e.preventDefault();
    gesture = e.pointerId;
    track.setPointerCapture(e.pointerId);
    press(hit.pitch, hit.key, e.clientY);
  });
  track.addEventListener('pointermove', (e) => {
    if (gesture !== e.pointerId) return;
    const hit = at(e.clientX, e.clientY);
    if (hit && held.has(hit.pitch)) return;
    releaseAll();
    if (hit) press(hit.pitch, hit.key, e.clientY);
  });
  for (const type of ['pointerup', 'pointercancel', 'pointerleave']) {
    track.addEventListener(type, (e) => {
      if (gesture !== e.pointerId) return;
      gesture = null;
      releaseAll();
    });
  }
  track.addEventListener('contextmenu', (e) => e.preventDefault());

  const scroller = el('div', { class: 'pk-scroll scroll-x' }, track);
  // Opening on middle C rather than at the bottom of the range: the register
  // a part is written in is the one that should be under the thumb.
  const show = (which) => {
    const key = keys.get(Math.max(from, Math.min(to, (which + 1) * 12)));
    if (!key) return;
    scroller.scrollLeft = Math.max(0, key.offsetLeft - scroller.clientWidth / 6);
  };
  mounted(() => show(octave));

  const octaves = [];
  for (let o = octaveOf(from); o <= octaveOf(to); o++) {
    octaves.push(el('button', {
      type: 'button', class: classes('chip', o === octave && 'on'),
      'aria-label': `show octave ${o}`,
      onclick: () => { show(o); onOctave?.(o); },
    }, `C${o}`));
  }

  return el('div', { class: 'pk' },
    el('div', { class: 'pk-octaves' },
      el('span', { class: 'hint' }, icon('midi')),
      octaves),
    scroller);
}

// The overlay: the keyboard summoned over whatever is on screen, owning the
// full width because that is the width a keyboard needs and a one-column
// panel cannot give it.
export function KeyboardOverlay({ onClose, ...options }) {
  return el('div', { class: 'pk-overlay', role: 'dialog', 'aria-label': 'keyboard' },
    el('div', { class: 'pk-overlay-bar' },
      el('span', { class: 'field-name' }, 'keyboard'),
      el('button', { class: 'ghost', onclick: () => onClose() }, 'close')),
    Keyboard(options));
}
