// The Key node's picture: the key the module is in, right now.
//
// Every other node that touches the key wears a badge saying which one it is
// playing in (components/KeyBadge.js). The node whose entire job is the key
// wore nothing, because the badge is drawn from `reads_key` and this one
// writes it instead - so the one card where "what is the key?" is the only
// question had no answer on it.
//
// **It holds no key of its own** (src/algorithm/midi/key.h): what it writes
// is the *playing* key, and the stored settings are deliberately left alone
// so a preset saved mid-performance captures the key the patch was written
// in. That is exactly why this cannot be drawn from `state.globals` - a
// sequencer eight bars into a piece has moved the key and the patch still
// says C. It is asked for instead (core/globals.js), and where the answer
// differs from what the patch holds the card says both: the key it is in,
// and the key it will come back to when the patch is loaded again.
//
// **The keys are a picture, not buttons.** On the globals tab pressing one
// moves the root; here the root is the cable's to move, and a second way to
// set it on this card would be a second writer of the one value this node
// exists to own.
//
// **Painted, not rebuilt.** The key arrives from a poll between renders
// (services/session.js) and moves on note-ons, so the picture is redrawn into
// elements that already exist: a card rebuilding itself per note would fight
// every control on it.

import { el } from '../../dom.js';
import { Hint } from '../../components/Panel.js';
import { ScaleKeys } from '../../components/ScaleKeys.js';
import { SCALES, scaleMaskById } from '../../../protocol/names.js';
import { keySpelling, scaleDegrees } from '../../../core/music.js';
import { globalNow } from '../../../core/globals.js';
import '../Key.css';

const scaleLabel = (id) => SCALES.find((s) => s.value === id)?.label ?? '';

// The key, and the key the patch would come back to: read together, because a
// root from one and a scale from the other is a key nobody set.
function reading(app) {
  const root = globalNow(app, 'root');
  const scale = globalNow(app, 'scale');
  return { root, scale, moved: root.moved || scale.moved };
}

const named = (rootPc, scaleId) => {
  const pc = rootPc % 12;
  return `${keySpelling(pc, scaleMaskById(scaleId))[pc]} ${scaleLabel(scaleId)}`;
};

export function KeyNow(app) {
  const board = el('div', { class: 'key-now-board' });
  const said = el('p', { class: 'key-now-said' });
  const note = Hint('');

  let drawn = null;
  const paint = () => {
    const now = reading(app);
    const key = `${now.root.value}:${now.scale.value}:${now.root.stored}:${now.scale.stored}`;
    if (key === drawn) return;
    drawn = key;

    const root = now.root.value % 12;
    const mask = scaleMaskById(now.scale.value);
    const spelling = keySpelling(root, mask);
    const degree = scaleDegrees(mask, root);
    const notes = [];
    for (let i = 0; i < 12; i++) {
      const pitchClass = (root + i) % 12;
      if (degree[pitchClass]) notes.push(spelling[pitchClass]);
    }
    board.replaceChildren(ScaleKeys({
      root, mask, spelling, label: 'the notes of the key the module is in',
    }));
    said.replaceChildren(
      el('strong', {}, named(now.root.value, now.scale.value)),
      el('span', { class: 'hint' }, ` — ${notes.join(' ')}`));
    // Where the patch would come back to, said only when it is somewhere
    // else: a card repeating "and the patch says C major" while the patch
    // says C major is a line nobody reads twice.
    note.textContent = now.moved
      ? `this node has moved it: the patch is saved in ${named(now.root.stored, now.scale.stored)}, `
        + 'and loading it again starts there'
      : 'the key the patch was saved in: nothing has moved it since';
  };
  paint();
  app.live?.paint(paint);

  return el('div', { class: 'grid key-now' },
    el('div', { class: 'grid-title' }, 'the key, now'),
    board, said, note);
}
