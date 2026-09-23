// The key, on the node that plays in it.
//
// The key is one setting for the whole patch and it lives on a tab of its
// own, which is the right place to *change* it and the wrong place to have to
// go and look it up. A Transpose asked for a third plays a different third in
// D minor than in D major, a sequencer's degrees are pitches only once the
// key has said which, and a card that governs none of that still has to be
// read in it. So every node whose algorithm says it reads the key wears the
// key (`reads_key`, src/node/node.h - the module says which, not a list here).
//
// **A cable outranks the key.** A node with a root inlet is rooted on
// whatever that bus last played, which is the rule the firmware itself
// follows, so the badge stops naming the key and names the chord: the note
// the cable is on and the degree it is within the key. That is what makes a
// patched root legible - "A, which is the fifth of D minor" is a musical
// fact, where "root inlet: note bus 3" is a wiring diagram.
//
// **The key it names is the one playing, not the one stored.** A Key node
// walking the root off a note bus, or a controller bound to the key, moves
// what every node here is reading and leaves the patch alone on purpose
// (src/midi/global_key.h) - so a badge drawn from `state.globals` would name
// the key each of these nodes is *not* in. It is asked for instead
// (core/globals.js).
//
// The dial is the circle of fifths, at the size of a word: twelve positions
// in fifths from the tonic at the top, filled where the key has a note.
// It is the same geometry the Harmony panel draws large, on purpose - a
// chord's place on that circle and a root's place on this one are the same
// place, and a root a step clockwise of the tonic is the dominant whichever
// picture you learned it from.

import * as P from '../../protocol/generated.js';
import { el, svg, classes } from '../dom.js';
import { scaleMaskById, scaleName } from '../../protocol/names.js';
import { inletName } from '../../core/patch.js';
import { keySpelling, fifthsFrom, degreeOf, scaleTriad, triadQuality, romanNumeral,
         QUALITY_MARK, octaveOf } from '../../core/music.js';
import { NO_NOTE } from '../../runtime/monitor.js';
import { keyNow } from '../../core/globals.js';
import './KeyBadge.css';

const DIAL = { mid: 11, ring: 7.6, dot: 1.3, mark: 2.6 };

const dialPoint = (k, r) => {
  const a = (k * 30 - 90) * Math.PI / 180;
  return [DIAL.mid + r * Math.cos(a), DIAL.mid + r * Math.sin(a)];
};

// Which inlet is the root, or -1. The name is the only thing that says so and
// it is the module's own ("root", "axis root"), which is why this asks the
// descriptor rather than carrying a table of algorithms that have one.
export function rootInlet(descriptor) {
  for (let i = 0; i < descriptor.nIn; i++) {
    if (/(^|\s)root$/.test(inletName(descriptor, i))) return i;
  }
  return -1;
}

// What the badge is saying, in one place: the key, and the note a cable has
// rooted this node on if there is one. `note` is null while the inlet is
// patched and has played nothing yet, which is a state worth showing - it
// says the key is not what this node is listening to.
// The note a cable on a node's root inlet has rooted it on: `{ inlet, note }`,
// with `inlet` null when nothing is patched there and `note` null while the
// cable has played nothing yet. The same reading a sequencer's note lane
// takes, so both name the pitch the firmware is actually playing from.
export function rootedNote(app, index) {
  const node = app.state.patch.nodes[index];
  const descriptor = node && app.device?.byId.get(node.algorithmId);
  const inlet = descriptor ? rootInlet(descriptor) : -1;
  const rooted = (node?.inBuses?.[inlet] ?? [])[0];
  if (inlet < 0 || rooted === undefined) return { inlet: null, note: null };
  const played = app.monitor?.busNote(rooted) ?? NO_NOTE;
  return { inlet: inletName(descriptor, inlet),
           note: played === undefined || played === NO_NOTE ? null : played };
}

function reading(app, index) {
  const key = keyNow(app);
  const root = key.root;
  const mask = scaleMaskById(key.scale);
  const shape = { root, mask, spelling: keySpelling(root, mask), scale: scaleName(key.scale) };
  if (index === null) return { ...shape, inlet: null, note: null };
  return { ...shape, ...rootedNote(app, index) };
}

// Where a position on the circle is, in words: clockwise of the tonic is
// sharpwards and counter-clockwise is flatwards, so a distance of eleven is
// one fifth below rather than eleven of anything.
function fifthsWords(k) {
  const steps = k <= 6 ? k : k - 12;
  if (steps === 0) return 'the tonic of the circle';
  const n = Math.abs(steps);
  return `${n} ${n === 1 ? 'fifth' : 'fifths'} ${steps > 0 ? 'above' : 'below'} it on the circle`;
}

// The chord an external root names: what it is called, what it does in the
// key, and where on the circle it sits. A root the key does not contain has
// no degree and is said so rather than given a numeral it has not earned.
function chordOf({ root, mask, spelling }, note) {
  const pc = note % 12;
  const degree = degreeOf(pc, root, mask);
  const quality = degree < 0 ? null : triadQuality(scaleTriad(degree, root, mask), pc);
  // Spelled by the key here too: a badge reading B flat and a tooltip reading
  // A sharp are two answers to one question.
  const pitch = `${spelling[pc]}${octaveOf(note)}`;
  return {
    pc,
    name: `${spelling[pc]}${quality ? QUALITY_MARK[quality] ?? '' : ''}`,
    roman: degree < 0 ? null : romanNumeral(degree, quality),
    title: degree < 0
      ? `${pitch} is not in the key, and sits ${fifthsWords(fifthsFrom(pc, root))}`
      : `${pitch}: degree ${degree + 1} of the key, ${fifthsWords(fifthsFrom(pc, root))}`,
  };
}

export function KeyBadge(app, { index = null } = {}) {
  const shape = reading(app, index);
  // Twelve ticks, always: that is what makes it the circle of fifths rather
  // than a ring of seven dots, and it is what gives a root outside the key
  // somewhere to be.
  // The positions never move - the tonic is position 0 whatever the key is -
  // so what a key change rewrites is which of them are filled.
  const ticks = Array.from({ length: 12 }, (_, k) => {
    const [x, y] = dialPoint(k, DIAL.ring);
    return svg('circle', { class: 'kbadge-tick', cx: x, cy: y, r: DIAL.dot });
  });
  const fillTicks = ({ root, mask }) => {
    for (let k = 0; k < 12; k++) {
      const pc = (root + k * 7) % 12;
      ticks[k].setAttribute('class',
        classes('kbadge-tick', ((mask >> (((pc - root) % 12 + 12) % 12)) & 1) && 'in', k === 0 && 'tonic'));
    }
  };
  const mark = svg('circle', { class: 'kbadge-mark off', r: DIAL.mark });
  const dial = svg('svg', { class: 'kbadge-dial', viewBox: `0 0 ${DIAL.mid * 2} ${DIAL.mid * 2}`,
                            'aria-hidden': 'true' }, ticks, mark);

  const name = el('span', { class: 'kbadge-key' }, '');
  const chord = el('span', { class: 'kbadge-chord off' });
  // A button, because the next thing somebody who has just read the key wants
  // is to change it, and the page it is changed on is two taps away otherwise.
  const badge = el('button', {
    type: 'button', class: 'kbadge', title: '',
    onclick: () => app.showTab?.('globals'),
  }, dial, name, chord);

  // The key and the root both arrive while the module runs, so the whole
  // badge is written per frame into elements that already exist - rebuilding
  // a card because a sequencer moved the key would fight every control on it.
  let drawn = null;
  const paint = () => {
    const now = reading(app, index);
    // Only when what it would write has changed: a card that is merely open
    // while a sequencer runs should cost nothing.
    const key = `${now.root}:${now.mask}:${now.inlet}:${now.note}`;
    if (key === drawn) return;
    drawn = key;
    fillTicks(now);
    name.textContent = `${now.spelling[now.root]} ${now.scale}`;
    const named = now.note === null ? null : chordOf(now, now.note);
    const at = named ? fifthsFrom(named.pc, now.root) : -1;
    if (at >= 0) {
      const [x, y] = dialPoint(at, DIAL.ring);
      mark.setAttribute('cx', x);
      mark.setAttribute('cy', y);
    }
    // A class rather than `hidden`: these carry a display of their own, and
    // an SVG element has no `hidden` attribute at all.
    mark.setAttribute('class', classes('kbadge-mark', at < 0 && 'off'));
    chord.className = classes('kbadge-chord', now.inlet === null && 'off',
                              named && !named.roman && 'foreign');
    chord.replaceChildren(...(named
      ? [el('span', { class: 'kbadge-note' }, named.name),
         named.roman ? el('span', { class: 'kbadge-roman' }, named.roman) : null].filter(Boolean)
      : [el('span', { class: 'kbadge-note waiting' }, '—')]));
    badge.setAttribute('title', now.inlet === null
      ? `the key: ${now.spelling[now.root]} ${now.scale}`
      : named
        ? `${named.title}, on the ${now.inlet} inlet — the key is ${now.spelling[now.root]} ${now.scale}`
        : `the ${now.inlet} inlet has played nothing yet — the key is ${now.spelling[now.root]} ${now.scale}`);
  };
  paint();
  // Repainted every frame whatever the node is: the key itself moves now, so
  // a badge that only followed a rooted cable would be stale on every other
  // card the moment a Key node ran.
  app.live?.paint(paint);
  return badge;
}
