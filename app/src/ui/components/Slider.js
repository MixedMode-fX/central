// A slider a scrolling finger cannot change.
//
// A native range input takes any touch that lands on it: the value jumps to
// where the finger touched down, the page then starts scrolling under it, and
// the gesture ends with a `change` that writes a value nobody chose. On a
// phone, where the whole editor is one long scroll and every parameter has a
// slider across it, that is what scrolling the patch tab does. The event
// trace is plain: `pointerdown`, `input` (jumped), `pointercancel` (the page
// took the gesture), `touchend`, `change`.
//
// So a touch has to *claim* the slider before it may move it: either by
// dragging along it - the axis it reads - or by holding still on it for a
// moment, which is a press rather than the start of a swipe. Until then every
// value the input produces is put straight back, and a gesture the browser
// cancels for scrolling puts it back too and commits nothing. A mouse or a
// stylus claims it on contact: neither is trying to scroll the page.

import { el } from '../dom.js';
import './Slider.css';

const CLAIM_PX = 8;    // a drag along the slider, far enough not to be a flick
const CLAIM_MS = 250;  // or a press held still, which is not a swipe either

// `onInput` follows the thumb; `onCommit` is the edit, on release. Every
// numeric control writes on commit rather than on input: an edit re-renders,
// which replaces this element, and writing on every input event would pull
// the control out from under the finger dragging it.
export function Slider(attrs, { onInput, onCommit } = {}) {
  const range = el('input', { type: 'range', ...attrs });
  let gesture = null;   // a pointer is down on it: where it started, and whether it has claimed it
  let refuse = false;   // the gesture ended unclaimed, so the `change` it fires is not an edit
  let start = null;     // the value the gesture began from
  const revert = () => { if (start !== null && range.value !== start) range.value = start; };
  const end = () => { if (gesture?.timer) clearTimeout(gesture.timer); gesture = null; };

  range.addEventListener('pointerdown', (e) => {
    start = range.value;
    refuse = false;
    gesture = { x: e.clientX, y: e.clientY, claimed: e.pointerType !== 'touch', timer: 0 };
    if (!gesture.claimed) {
      gesture.timer = setTimeout(() => { if (gesture) gesture.claimed = true; }, CLAIM_MS);
    }
  });
  range.addEventListener('pointermove', (e) => {
    if (!gesture || gesture.claimed) return;
    const dx = Math.abs(e.clientX - gesture.x);
    const dy = Math.abs(e.clientY - gesture.y);
    if (dx >= CLAIM_PX && dx > dy) gesture.claimed = true;
    else if (dy >= CLAIM_PX) revert();          // this is a scroll, not an edit
  });
  // The browser takes the gesture the moment the page scrolls under the
  // finger. Whatever the slider did with it before that is undone.
  range.addEventListener('pointercancel', () => { revert(); refuse = true; end(); });
  range.addEventListener('pointerup', () => { refuse = !gesture?.claimed; end(); });
  range.addEventListener('input', () => {
    if (gesture && !gesture.claimed) { revert(); return; }
    onInput?.(range.value);
  });
  range.addEventListener('change', () => {
    if (refuse) { refuse = false; revert(); return; }
    onCommit?.(range.value);
  });
  return range;
}

// A level in 0..1, as a slider over 0..100.
export const LevelSlider = ({ value, label, onInput, onCommit }) => Slider({
  class: 'grow', min: '0', max: '100', value: String(Math.round(value * 100)), 'aria-label': label,
}, { onInput: (v) => onInput(Number(v) / 100), onCommit });
