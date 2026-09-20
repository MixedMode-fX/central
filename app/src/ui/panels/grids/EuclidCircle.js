// The Euclidean sequencer's picture: its steps as a ring, the pulses joined
// into the necklace that is the rhythm's shape, and a hand on the step that
// is sounding.
//
// A circle rather than a row because a Euclidean rhythm *is* circular. What
// the two knobs do is only legible on a ring: raising the pulses redistributes
// every onset at once instead of filling in a square, and the rotation turns
// the necklace under a downbeat that stays at the top. Neither reads as
// anything but a row of squares flickering.
//
// **The pattern is the node's own.** `euclidean_pattern` and `rotate_pattern`
// (src/algorithm/sequencer/euclid.h) run in the firmware and this reads the
// result back through the module, step by step, rather than spreading k over
// n a second time here: Bjorklund reimplemented in the page is a rule that
// could drift, and this one is famous for the near-misses that produce
// *rotations* of the published patterns. So, like the harmony circle, this is
// drawn from the running node and says so when there is not one yet.

import { el, svg, classes, clear } from '../../dom.js';
import { NO_STEP, paintPlayhead } from './playhead.js';
import '../Euclid.css';

const RING = { mid: 120, r: 86, hand: 66, tick: 108 };

// Step 0 at the top and time running clockwise: the way a rhythm is written
// down, and the way a clock goes round.
const stepPoint = (step, steps, r) => {
  const a = (step / steps) * 2 * Math.PI - Math.PI / 2;
  return [RING.mid + r * Math.cos(a), RING.mid + r * Math.sin(a)];
};

// Eight steps want a big dot and MAX_SEQUENCE_LEN of them want a small one,
// because what has to stay visible is the gap between two.
const dotRadius = (steps) => Math.max(3.5, Math.min(11, 96 / steps));

// The beat as the running node holds it. Null where there is no node yet, so
// every caller has one thing to test.
function euclidShape(app, index) {
  const m = app.module;
  if (!m?.seqKind || index >= m.nodeCount() || !m.seqKind(index)) return null;
  const steps = m.seqLength(index, 0);
  if (!steps) return null;
  const on = Array.from({ length: steps }, (_, step) => m.seqCell(index, 0, step) !== 0);
  const hits = on.flatMap((pulse, step) => (pulse ? [step] : []));
  // The inter-onset intervals, which is how a drummer reads a necklace:
  // E(3,8) is "3 3 2" long before it is 10010010. Cyclic, so the last gap is
  // the one that comes round past the top.
  const gaps = hits.map((step, i) => (hits[(i + 1) % hits.length] - step + steps) % steps || steps);
  return { steps, on, hits, gaps, bits: on.map((pulse) => (pulse ? '1' : '0')).join('') };
}

// A node added from the add bar has no pulses yet - the parameter's default
// is zero - so the empty ring is the first one anybody sees, and it has to say
// what is missing rather than count gaps that are not there.
const sentence = (shape) => (shape.hits.length
  ? `${shape.hits.length} ${shape.hits.length === 1 ? 'pulse' : 'pulses'} over ${shape.steps} steps`
  : `no pulses over ${shape.steps} steps: nothing sounds`);

// The ring, redrawn whenever the pattern under it changes - which is every
// turn of pulses, rotation or length, and none of them rebuild the card.
function drawRing(shape, view) {
  clear(view.plate);
  const radius = dotRadius(shape.steps);

  // The necklace: every pulse joined to the next one round the circle. Two
  // patterns with the same polygon are the same rhythm started in different
  // places, which is the one thing a picture can say and a list of steps
  // cannot.
  const necklace = shape.hits.length > 1
    ? svg('path', {
        class: 'necklace',
        d: `${shape.hits.map((step, i) => {
          const [x, y] = stepPoint(step, shape.steps, RING.r);
          return `${i ? 'L' : 'M'}${x.toFixed(1)} ${y.toFixed(1)}`;
        }).join('')}Z`,
      })
    : null;

  view.dots = shape.on.map((pulse, step) => {
    const [x, y] = stepPoint(step, shape.steps, RING.r);
    return svg('g', { class: classes('step-dot', pulse && 'on', step === 0 && 'first') },
      svg('title', {}, `step ${step + 1}: ${pulse ? 'pulse' : 'rest'}`),
      svg('circle', { cx: x.toFixed(1), cy: y.toFixed(1), r: radius.toFixed(1) }));
  });

  view.plate.append(...[necklace, view.hand, ...view.dots].filter(Boolean));
  view.picture.setAttribute('aria-label', shape.hits.length ? `${sentence(shape)}: ${shape.bits}` : sentence(shape));
  view.caption.textContent = sentence(shape);
  view.bits.textContent = shape.hits.length ? shape.bits : '';
  view.gaps.textContent = shape.hits.length ? `gaps ${shape.gaps.join(' ')}` : '';
  view.drawn = shape.bits;
}

// What the module is doing to the ring, per frame: the step sounding, and the
// pattern itself when a knob has moved under it.
function paintEuclid(app, index, view) {
  const shape = euclidShape(app, index);
  if (!shape) return;
  if (view.drawn !== shape.bits) drawRing(shape, view);
  const at = app.module.seqPosition(index, 0);
  paintPlayhead(view.dots, at);
  // Before the first advance there is no step to point at, and a hand parked
  // on step 1 would be a lie about a sequencer that has not started.
  const sounding = at !== NO_STEP && at < shape.steps;
  view.hand.setAttribute('opacity', sounding ? '1' : '0');
  if (!sounding) return;
  const [x, y] = stepPoint(at, shape.steps, RING.hand);
  view.hand.setAttribute('x2', x.toFixed(1));
  view.hand.setAttribute('y2', y.toFixed(1));
}

export function EuclidCircle(app, index) {
  const shape = euclidShape(app, index);
  if (!shape) {
    return el('div', { class: 'grid euclid' },
      el('div', { class: 'grid-title' }, 'the beat'),
      el('p', { class: 'hint' },
        'the ring is read from the running node, and appears once the module has taken the patch'));
  }

  const hand = svg('line', {
    class: 'hand', x1: RING.mid, y1: RING.mid, x2: RING.mid, y2: RING.mid - RING.hand, opacity: '0',
  });
  const plate = svg('g', {});
  const picture = svg('svg', { class: 'euclid-ring', viewBox: '0 0 240 240', role: 'img' },
    svg('circle', { class: 'euclid-guide', cx: RING.mid, cy: RING.mid, r: RING.r }),
    // The downbeat, marked outside the ring: the rotation turns the necklace
    // and leaves this where it is, which is the whole of what rotation means.
    svg('line', { class: 'euclid-top', x1: RING.mid, y1: RING.mid - RING.tick - 5, x2: RING.mid, y2: RING.mid - RING.tick + 5 }),
    plate);

  const view = {
    picture, plate, hand, dots: [],
    caption: el('span', { class: 'euclid-said' }, ''),
    bits: el('span', { class: 'euclid-bits' }, ''),
    gaps: el('span', {}, ''),
    drawn: null,
  };
  drawRing(shape, view);
  app.live?.paint(() => paintEuclid(app, index, view));

  return el('div', { class: 'grid euclid' },
    el('div', { class: 'grid-title' }, 'the beat'),
    picture,
    el('div', { class: 'hint euclid-caption' },
      view.caption,
      view.bits,
      // The gaps between onsets: what you would count out loud, and the
      // reading of a necklace that survives being rotated.
      view.gaps));
}
