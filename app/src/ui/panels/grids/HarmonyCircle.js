// Harmony's picture: the key's chords on the circle of fifths, and every
// move the walk would make from one of them as an arrow whose weight is how
// much it wants it - or the loop it has written down as a path through them.
//
// **The weights are the node's own.** `Harmony::weigh` is the function the
// draw reads, through the module, not a second opinion about it in
// JavaScript: a rule reimplemented here would be a rule that could drift,
// and a picture that disagrees with the music is worse than no picture. It
// follows that this view is drawn from the *running* node and says so when
// there is not one yet, where every other panel is drawn from the patch.

import { el, svg, classes, clear } from '../../dom.js';
import { Segmented } from '../../components/Segmented.js';
import { scaleMaskById } from '../../../protocol/names.js';
import { keySpelling, triadQuality, fifthsFrom, romanNumeral, QUALITY_MARK,
         PITCH_CLASSES, PITCH_CLASSES_FLAT } from '../../../core/music.js';
import { NO_STEP, paintPlayhead } from './playhead.js';
import '../Harmony.css';

const CIRCLE = { mid: 120, ring: 78, label: 105, dot: 15 };

const circlePoint = (k, r) => {
  const a = (k * 30 - 90) * Math.PI / 180;
  return [CIRCLE.mid + r * Math.cos(a), CIRCLE.mid + r * Math.sin(a)];
};

// A chord is named twice on the circle: the note name outside the ring is
// what you would call it (Dm), and the numeral inside is what it does in this
// key (ii). Both spellings are the key's own (`keySpelling`, `romanNumeral`),
// not off a list of sharps - and they are shared with the key badge, so a
// chord is called the same thing wherever it is shown.

// Which chord the arrows come from, and which picture is up, per node:
// where you are looking, not part of the patch. An absent focus means
// *follow* - the arrows come from whatever is sounding.
const viewOf = (app, index) => app.state.ui.harmony.get(index) ?? {};
const setView = (app, index, changes) => {
  app.state.ui.harmony.set(index, { ...viewOf(app, index), ...changes });
  app.render();
};

// Everything the picture needs, read from the running node in one place.
function harmonyShape(app, index) {
  const m = app.module;
  const n = m?.harmonyDegrees ? m.harmonyDegrees(index) : 0;
  if (!n) return null;
  const spelling = keySpelling(app.state.globals?.root ?? 0, scaleMaskById(app.state.globals?.scale));
  const chords = [];
  for (let d = 0; d < n; d++) {
    const pitch = m.harmonyPitch(index, d);
    const pc = pitch % 12;
    const quality = triadQuality(m.harmonyTriad(index, d), pc);
    chords.push({ degree: d, pitch, pc, quality,
                  name: `${spelling[pc]}${QUALITY_MARK[quality] ?? ''}`, roman: romanNumeral(d, quality) });
  }
  const tonicPc = chords[0].pc;
  for (const chord of chords) chord.k = fifthsFrom(chord.pc, tonicPc);
  return { n, chords, tonicPc, spelling, byPosition: new Map(chords.map((c) => [c.k, c])) };
}

const modeOf = (app, index, loopLength) => (loopLength ? (viewOf(app, index).mode ?? 'loop') : 'moves');

export function HarmonyCircle(app, index) {
  const shape = harmonyShape(app, index);
  if (!shape) {
    return el('div', { class: 'grid' },
      el('div', { class: 'grid-title' }, 'circle of fifths'),
      el('p', { class: 'hint' }, 'the circle is read from the running node, and appears once the module has taken the patch'));
  }
  const m = app.module;
  const loopLength = m.harmonyLoopLength(index);
  const mode = modeOf(app, index, loopLength);
  const focus = viewOf(app, index).focus ?? null;

  // The twelve positions are always drawn - that is what makes it the circle
  // of fifths and not a ring of seven dots. A position the key has no chord
  // on is named by where it sits: counter-clockwise of the tonic is flat.
  const marks = Array.from({ length: 12 }, (_, k) => {
    const [x, y] = circlePoint(k, CIRCLE.label);
    const chord = shape.byPosition.get(k);
    const pc = (shape.tonicPc + k * 7) % 12;
    return svg('text', {
      x, y, class: classes('circle-label', chord && 'in-key'),
      'text-anchor': 'middle', 'dominant-baseline': 'middle',
    }, chord ? chord.name : (k >= 7 ? PITCH_CLASSES_FLAT : PITCH_CLASSES)[pc]);
  });

  // Clicking a chord pins the arrows to it; clicking it again lets them
  // follow the music. One control, and no third state to explain.
  const dots = shape.chords.map((chord) => {
    const [x, y] = circlePoint(chord.k, CIRCLE.ring);
    const focused = focus === chord.degree;
    const toggle = () => setView(app, index, { focus: focused ? null : chord.degree });
    return svg('g', {
      class: classes('chord-dot', `q-${chord.quality}`, focused && 'focused'),
      role: 'button', tabindex: '0',
      onclick: toggle,
      onkeydown: (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); toggle(); } },
    },
      svg('title', {}, `${chord.roman} — ${chord.name} (${shape.spelling[chord.pc]}${Math.floor(chord.pitch / 12) - 1})`),
      svg('circle', { cx: x, cy: y, r: CIRCLE.dot }),
      svg('text', { x, y, 'text-anchor': 'middle', 'dominant-baseline': 'middle' }, chord.roman));
  });

  const fan = svg('g', { class: 'fan' });
  const picture = svg('svg', {
    class: 'circle-of-fifths', viewBox: '0 0 240 240', role: 'img',
    'aria-label': `the key's chords on the circle of fifths, ${mode === 'loop' ? 'with the loop it is playing' : 'with the moves the walk would make'}`,
  },
    svg('defs', {}, svg('marker', {
      id: `harm-arrow-${index}`, viewBox: '0 0 10 10', refX: '9', refY: '5',
      markerWidth: '5', markerHeight: '5', markerUnits: 'userSpaceOnUse', orient: 'auto',
    }, svg('path', { d: 'M0 0 L10 5 L0 10 z' }))),
    svg('circle', { class: 'circle-ring', cx: CIRCLE.mid, cy: CIRCLE.mid, r: 92 }),
    marks, fan, dots);
  const caption = el('p', { class: 'hint harmony-caption' }, '');
  const slots = loopLength ? LoopSlots(loopLength) : null;

  const view = { fan, caption, dots, slots, drawn: null };
  drawHarmony(app, index, view);
  app.live?.paint(() => paintHarmony(app, index, view));

  return el('div', { class: 'grid harmony' },
    el('div', { class: 'grid-title' }, `circle of fifths · ${shape.chords[0].name} · ${shape.n} chords`),
    loopLength
      ? Segmented({
          options: [{ value: 'loop', label: 'loop', hint: 'the progression it has written down' },
                    { value: 'moves', label: 'moves', hint: 'every move the walk would make from one chord' }],
          value: mode, label: 'what the circle shows',
          onChange: (chosen) => setView(app, index, { mode: chosen }),
        })
      : null,
    picture, caption,
    slots ? el('div', { class: 'loop-row' },
      el('span', { class: 'lane-name' }, `loop (${loopLength})`),
      el('div', { class: 'chord-slots' }, slots)) : null);
}

// The loop as its slots: the chord in each, and which one is sounding. A
// loop still being captured has empty slots at the end, and they fill one
// per advance. The same shape as a sequencer's steps, because it is the same
// thing - a pattern with a playhead in it. The chips are filled per frame,
// because a shift turns the loop round under them without an edit.
const LoopSlots = (loopLength) => Array.from({ length: loopLength }, () =>
  el('div', { class: 'chord-slot' }, el('span', { class: 'roman' }, ''), el('span', { class: 'name' }, '')));

// A chip is marked with the degree it holds, so the frame loop can skip the
// work when it has not changed.
function fillSlot(chip, shape, degree, slot) {
  const chord = degree === NO_STEP ? null : shape.chords[degree];
  chip.classList.toggle('empty', !chord);
  chip.setAttribute('data-degree', String(degree));
  chip.setAttribute('title', chord ? `chord ${slot + 1}: ${chord.roman} (${chord.name})`
                                   : `chord ${slot + 1}: not written yet`);
  chip.children[0].textContent = chord ? chord.roman : '·';
  chip.children[1].textContent = chord ? chord.name : '';
}

// A move is an arc, not a chord line: two chords a fifth apart are
// neighbours on this circle, so the most important move there is would be a
// straight line ten pixels long between two touching dots. Bending every arc
// halfway towards the middle gives the short ones room, and the curve's own
// tangents put the arrowheads square on the dots whatever the span.
function arc(index, a, b, attrs) {
  const [x0, y0] = circlePoint(a.k, CIRCLE.ring);
  const [x1, y1] = circlePoint(b.k, CIRCLE.ring);
  const cx = (x0 + x1) / 2 + (CIRCLE.mid - (x0 + x1) / 2) * 0.5;
  const cy = (y0 + y1) / 2 + (CIRCLE.mid - (y0 + y1) / 2) * 0.5;
  const step = (from, to, by) => {
    const dx = to[0] - from[0], dy = to[1] - from[1];
    const len = Math.hypot(dx, dy) || 1;
    return [from[0] + dx / len * by, from[1] + dy / len * by];
  };
  const start = step([x0, y0], [cx, cy], CIRCLE.dot + 2);
  const end = step([x1, y1], [cx, cy], CIRCLE.dot + 6);
  return svg('path', {
    d: `M${start[0].toFixed(1)} ${start[1].toFixed(1)} Q${cx.toFixed(1)} ${cy.toFixed(1)} ${end[0].toFixed(1)} ${end[1].toFixed(1)}`,
    fill: 'none', 'marker-end': `url(#harm-arrow-${index})`, ...attrs,
  });
}

// The arrows and the sentence under them, drawn into elements that already
// exist: while the arrows follow the music they are redrawn as each chord
// lands, and nothing else on the card is rebuilt.
function drawHarmony(app, index, view) {
  const shape = harmonyShape(app, index);
  if (!shape) return;
  const m = app.module;
  const playing = m.harmonyDegree(index);
  const pinned = viewOf(app, index).focus ?? null;
  const from = pinned !== null ? Math.min(pinned, shape.n - 1)
             : (playing === NO_STEP ? 0 : Math.min(playing, shape.n - 1));
  const loopLength = m.harmonyLoopLength(index);
  const mode = modeOf(app, index, loopLength);
  view.drawn = `${mode}:${from}:${loopLength}:${m.harmonyLoopPosition(index)}`;
  clear(view.fan);
  const { fan, caption } = view;

  if (mode === 'loop') {
    const chords = [];
    for (let slot = 0; slot < loopLength; slot++) {
      const degree = m.harmonyLoopChord(index, slot);
      if (degree !== NO_STEP && degree < shape.n) chords.push(shape.chords[degree]);
    }
    for (let i = 0; i + 1 < chords.length; i++) {
      if (chords[i].degree === chords[i + 1].degree) continue;   // a repeat draws nothing
      fan.append(arc(index, chords[i], chords[i + 1], { class: 'move loop' }));
    }
    // The loop is a loop: the last chord goes back to the first, dashed,
    // because nothing else on the picture says which arrow comes round.
    if (chords.length === loopLength && loopLength > 1 && chords.at(-1).degree !== chords[0].degree) {
      fan.append(arc(index, chords.at(-1), chords[0], { class: 'move loop round' }));
    }
    caption.textContent = chords.length < loopLength
      ? `writing it down: ${chords.length} of ${loopLength} chords`
      : `${chords.map((chord) => chord.roman).join(' → ')} →`;
    return;
  }

  const weights = [];
  let total = 0, top = 0, best = -1;
  for (let to = 0; to < shape.n; to++) {
    weights[to] = m.harmonyWeight(index, from, to);
    total += weights[to];
    if (weights[to] > top) { top = weights[to]; best = to; }
  }
  if (!total) { caption.textContent = 'this key has one chord: the walk has nowhere to go'; return; }
  for (let to = 0; to < shape.n; to++) {
    if (to === from || !weights[to]) continue;
    const share = weights[to] / top;
    fan.append(arc(index, shape.chords[from], shape.chords[to], {
      class: classes('move', to === best && 'top'),
      'stroke-width': (0.8 + share * 4.2).toFixed(2),
      'stroke-opacity': (0.18 + share * 0.72).toFixed(2),
    }));
  }
  const percent = (w) => `${Math.round(w * 100 / total)}%`;
  const stays = weights[from] ? `, stays on ${shape.chords[from].roman} ${percent(weights[from])}` : '';
  const source = pinned !== null ? 'from' : 'playing';
  caption.textContent = best < 0 || best === from
    ? `${source} ${shape.chords[from].roman}: it stays`
    : `${source} ${shape.chords[from].roman} → most likely ${shape.chords[best].roman} ${percent(top)}${stays}`;
}

// What the module is doing to the circle, per frame: the chord sounding, the
// slot of the loop it is sounding in, and - while the arrows follow rather than being
// pinned - the fan redrawn as each chord lands. Redrawn only when what it
// would draw has changed, so a card that is merely open costs nothing.
function paintHarmony(app, index, view) {
  const m = app.module;
  if (index >= m.nodeCount() || !m.harmonyDegrees(index)) return;
  const degree = m.harmonyDegree(index);
  paintPlayhead(view.dots, degree);
  const loopLength = m.harmonyLoopLength(index);
  const at = m.harmonyLoopPosition(index);
  if (view.slots) {
    let shape = null;
    view.slots.forEach((chip, slot) => {
      const written = m.harmonyLoopChord(index, slot);
      if (chip.getAttribute('data-degree') !== String(written)) {
        shape ??= harmonyShape(app, index);
        if (shape) fillSlot(chip, shape, written, slot);
      }
    });
    paintPlayhead(view.slots, at);
  }
  const pinned = viewOf(app, index).focus ?? null;
  const from = pinned !== null ? pinned : (degree === NO_STEP ? 0 : degree);
  if (view.drawn !== `${modeOf(app, index, loopLength)}:${from}:${loopLength}:${at}`) drawHarmony(app, index, view);
}
