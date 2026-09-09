// The two time views: the scope, and the piano roll.
//
// Everything else in the play tab answers "what is true now" - a lamp is lit,
// a bus is high, a note is sounding. Neither of the questions that matter most
// while a patch is being built is of that shape:
//
//   * **is this the pattern I asked for?** A divider, a Euclidean sequencer, a
//     logic gate and a clock are all things whose output is only readable
//     *over time*. A row of dots blinking at 60 Hz cannot be compared with
//     another row of dots blinking at 60 Hz; two traces drawn side by side on
//     one time axis can be read at a glance.
//   * **what is it playing?** A log of note ons is a list of numbers. The same
//     notes on a pitch-against-time grid are a melody, a chord or a mistake,
//     and which of the three it is takes no reading at all.
//
// Both are drawn from the module's own sampling, once per pass of simulated
// time (`module.trace`, `module.notes`), so what is on the screen is what the
// firmware did and not what the page happened to catch it doing. That is the
// whole reason the scope is worth having: an animation frame is sixteen
// milliseconds and a trigger is one, so a view that polls cannot be trusted
// about exactly the signals it exists to show.

import * as P from './protocol.js';
import { el, noteName } from './views.js';
import { Domain } from './validate.js';

const ROW_H = 18;            // one scope trace
const TRACE_FONT = '11px ui-monospace, SFMono-Regular, Menlo, monospace';
const ROLL_GUTTER = 30;      // room for a pitch name
const ROLL_H = 200;
const MIN_SEMITONES = 13;    // an octave, so a one-note patch is not a full-height bar

// The page's own palette, read from the stylesheet rather than written out
// again here: a canvas cannot use a CSS variable, and two copies of a colour is
// two colours the day one of them is changed. The fallbacks are what the tests
// see, where there is no stylesheet to read.
const FALLBACK = {
  in: '#6ea8fe', out: '#e8c46a', bus: '#7bd88f',
  noteOut: '#d8a67b', noteIn: '#6ea8fe',
  grid: '#2c313d', dim: '#8b93a7', back: '#12141a', panel: '#1b1e26', raised: '#232833',
};
const VARIABLE = {
  in: '--accent', out: '--warn', bus: '--gate',
  noteOut: '--note', noteIn: '--accent',
  grid: '--line', dim: '--dim', back: '--bg', panel: '--panel', raised: '--raised',
};
let COLOUR = FALLBACK;
let read = false;
function palette() {
  if (read) return COLOUR;
  read = true;
  const style = globalThis.getComputedStyle?.(document.documentElement);
  if (!style) return COLOUR;
  const found = { ...FALLBACK };
  for (const [key, variable] of Object.entries(VARIABLE)) {
    const value = style.getPropertyValue(variable).trim();
    if (value) found[key] = value;
  }
  COLOUR = found;
  return COLOUR;
}

// --- the scope --------------------------------------------------------------

// Which gate buses this patch actually touches. Sixteen traces of which
// thirteen never move is not a view, it is a wall - so the default is the ones
// the patch is using, and "every bus" is a checkbox for when something is
// firing that the patch does not admit to.
function usedGateBuses(app) {
  const used = new Set();
  for (const port of app.patch.gatePorts) {
    if (port.direction !== P.GatePortDirection.GATE_PORT_UNUSED && port.bus !== P.NO_BUS) used.add(port.bus);
  }
  for (const node of app.patch.nodes) {
    const d = app.device?.byId.get(node.algorithmId);
    if (!d) continue;
    for (let i = 0; i < d.nIn && i < P.MAX_IN; i++) {
      if (d.inDomain[i] === Domain.Gate && node.inBus[i] !== P.NO_BUS) used.add(node.inBus[i]);
    }
    for (let i = 0; i < d.nOut && i < P.MAX_OUT; i++) {
      if (d.outDomain[i] === Domain.Gate && node.outBus[i] !== P.NO_BUS) used.add(node.outBus[i]);
    }
  }
  return used;
}

function scopeRows(app) {
  const rows = [];
  const buses = app.device?.capabilities?.gateBuses ?? P.N_GATE_BUS;
  for (let j = 0; j < P.GPIO_N; j++) {
    const direction = app.patch.gatePorts[j]?.direction ?? P.GatePortDirection.GATE_PORT_UNUSED;
    if (direction === P.GatePortDirection.GATE_PORT_UNUSED && !app.scopeAll) continue;
    const out = direction === P.GatePortDirection.GATE_PORT_OUT;
    const way = direction === P.GatePortDirection.GATE_PORT_UNUSED ? '–' : (out ? 'out' : 'in');
    rows.push({
      kind: out ? 'out' : 'in', bit: j, source: out ? 'jackOut' : 'jackIn',
      label: `jack ${j + 1} ${way}`, short: `J${j + 1}${way[0]}`,
    });
  }
  const used = usedGateBuses(app);
  for (let b = 0; b < buses; b++) {
    if (!app.scopeAll && !used.has(b)) continue;
    rows.push({ kind: 'bus', bit: b, source: 'gate', label: `gate ${b}`, short: `b${b}` });
  }
  return rows;
}

export function scopePanel(app) {
  const rows = scopeRows(app);
  const canvas = el('canvas', { class: 'scope', id: 'scope',
                                'aria-label': 'jack and gate bus levels over the last four seconds' });
  canvas.traceRows = rows;
  const all = el('input', { type: 'checkbox', class: 'switch',
                            onchange: (e) => { app.scopeAll = e.target.checked; app.render(); } });
  all.checked = Boolean(app.scopeAll);
  return el('section', { class: 'panel' },
    el('h2', {}, 'scope — the last four seconds'),
    rows.length
      ? el('div', { class: 'scope-wrap' }, canvas)
      : el('p', { class: 'hint' },
          'This patch drives no jack and no gate bus yet. Tick “every jack and bus” to watch them anyway.'),
    el('div', { class: 'row' },
      el('label', { class: 'bool' }, all, el('span', {}, 'every jack and bus')),
      el('span', { class: 'hint' }, 'sampled once per pass — a 1 ms trigger is a line, not a maybe')));
}

export function drawScope(app) {
  const canvas = document.getElementById('scope');
  if (!canvas || !app.module) return;
  const rows = canvas.traceRows ?? [];
  if (!rows.length) return;
  const height = rows.length * ROW_H + 12;
  const ctx = fit(canvas, height);
  if (!ctx) return;
  const width = canvas.clientWidth;
  const COLOUR = palette();

  const trace = app.module.trace;
  const count = trace.filled;
  const first = (trace.head - count + trace.len) % trace.len;

  // The names are measured, not guessed at: "jack 1 out" is wider than "gate 3"
  // and both are wider on a phone's font than on a laptop's. A label that
  // overruns its gutter is drawn across the trace it names, which is worse than
  // an abbreviation - so when the long form does not fit, the short one is used.
  ctx.font = TRACE_FONT;
  ctx.textBaseline = 'middle';
  const cap = width * 0.35;
  let labels = rows.map((row) => row.label);
  let widest = Math.max(...labels.map((label) => ctx.measureText(label).width));
  if (widest + 8 > cap) {
    labels = rows.map((row) => row.short ?? row.label);
    widest = Math.max(...labels.map((label) => ctx.measureText(label).width));
  }
  const x0 = Math.min(Math.max(widest + 8, 34), cap);
  const span = Math.max(1, width - x0 - 4);

  ctx.fillStyle = COLOUR.back;
  ctx.fillRect(0, 0, width, height);

  // A line per second of simulated time, so a tempo can be read off the trace
  // rather than counted.
  const seconds = (trace.len * trace.us) / 1e6;
  ctx.strokeStyle = COLOUR.grid;
  ctx.lineWidth = 1;
  for (let s = 1; s <= seconds; s++) {
    const x = Math.round(x0 + span - (s / seconds) * span) + 0.5;
    ctx.beginPath();
    ctx.moveTo(x, 0);
    ctx.lineTo(x, height);
    ctx.stroke();
  }

  rows.forEach((row, r) => {
    const y0 = r * ROW_H + 4;
    const hi = y0 + 3;
    const lo = y0 + ROW_H - 5;
    const colour = COLOUR[row.kind] ?? COLOUR.bus;
    ctx.fillStyle = COLOUR.dim;
    ctx.fillText(labels[r], 4, y0 + ROW_H / 2);
    ctx.strokeStyle = COLOUR.grid;
    ctx.beginPath();
    ctx.moveTo(x0, lo + 0.5);
    ctx.lineTo(x0 + span, lo + 0.5);
    ctx.stroke();
    if (!count) return;

    // One pixel is several columns on a narrow screen, so the columns behind a
    // pixel are OR-ed rather than sampled: a pulse never disappears because
    // the trace was scaled down.
    const data = trace[row.source];
    const mask = 1 << row.bit;
    ctx.strokeStyle = colour;
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    let previous = -1;
    // The axis is *time*, not "however many columns there are": a module that
    // has been running for a fifth of a second draws a fifth of a second at the
    // right-hand edge, under the same one-second grid lines, rather than
    // stretching it across four.
    for (let px = Math.floor(span - (count / trace.len) * span); px <= span; px++) {
      const at = (px - (span - (count / trace.len) * span)) / ((count / trace.len) * span);
      const from = Math.floor(at * count);
      const to = Math.max(from + 1, Math.floor((at + trace.len / (span * count)) * count));
      let value = 0;
      for (let i = from; i < to && i < count; i++) {
        if (data[(first + i) % trace.len] & mask) { value = 1; break; }
      }
      const x = x0 + px;
      const y = value ? hi : lo;
      if (previous < 0) ctx.moveTo(x, y);
      else if (value !== previous) { ctx.lineTo(x, previous ? hi : lo); ctx.lineTo(x, y); }
      else ctx.lineTo(x, y);
      previous = value;
    }
    ctx.stroke();
  });
}

// --- the piano roll ---------------------------------------------------------

export function rollPanel(app) {
  const canvas = el('canvas', { class: 'roll', id: 'roll',
                                'aria-label': 'the notes of the last eight seconds' });
  return el('section', { class: 'panel' },
    el('h2', {}, 'piano roll — what is playing'),
    el('div', { class: 'scope-wrap' }, canvas),
    el('div', { class: 'row' },
      el('span', { class: 'roll-key out' }, 'sent by the module'),
      el('span', { class: 'roll-key in' }, 'played into it'),
      el('button', { class: 'ghost', onclick: () => { app.module.clearNotes(); } }, 'clear')),
    el('p', { class: 'hint' },
      'Every note on and off, on one time line: the keyboard and a controller going in, '
      + 'and what the patch made of them coming out. A bar still growing is a note still held.'));
}

export function drawRoll(app) {
  const canvas = document.getElementById('roll');
  if (!canvas || !app.module) return;
  const ctx = fit(canvas, ROLL_H);
  if (!ctx) return;
  const width = canvas.clientWidth;
  const COLOUR = palette();
  const module = app.module;
  const span = module.rollSpan();
  const now = module.now;
  const from = now - span;
  const notes = module.notes.filter((n) => (n.end ?? now) >= from);

  ctx.fillStyle = COLOUR.back;
  ctx.fillRect(0, 0, width, ROLL_H);

  // The pitch range follows what is playing, rounded out to whole octaves, so
  // a bass line and a hi-hat are not squeezed onto the same two rows - and so a
  // patch playing one note does not draw it as a full-height slab.
  let low = 127;
  let high = 0;
  for (const note of notes) { if (note.pitch < low) low = note.pitch; if (note.pitch > high) high = note.pitch; }
  if (low > high) { low = 48; high = 72; }
  while (high - low + 1 < MIN_SEMITONES) { if (low > 0) low--; if (high < 127) high++; }
  low = Math.max(0, low - 1);
  high = Math.min(127, high + 1);
  const lanes = high - low + 1;
  const laneH = ROLL_H / lanes;
  const y = (pitch) => (high - pitch) * laneH;
  const x = (t) => ROLL_GUTTER + ((t - from) / span) * Math.max(1, width - ROLL_GUTTER);

  // A keyboard down the left edge, and the black keys shaded across the whole
  // width: without them a bar is at "some height", not at a pitch.
  ctx.font = '10px ui-monospace, SFMono-Regular, Menlo, monospace';
  ctx.textBaseline = 'middle';
  for (let pitch = low; pitch <= high; pitch++) {
    const black = [1, 3, 6, 8, 10].includes(((pitch % 12) + 12) % 12);
    ctx.fillStyle = black ? COLOUR.back : COLOUR.panel;
    ctx.fillRect(0, y(pitch), width, laneH);
    if (pitch % 12 === 0) {
      ctx.fillStyle = COLOUR.grid;
      ctx.fillRect(ROLL_GUTTER, y(pitch) + laneH - 1, width - ROLL_GUTTER, 1);
    }
    // Every lane gets its name where there is room for one, because "which
    // drum was that" is the question being asked, and counting rows up from
    // the nearest C to answer it is not reading, it is arithmetic.
    if (laneH >= 9 || pitch % 12 === 0) {
      ctx.fillStyle = COLOUR.dim;
      ctx.fillText(noteName(pitch), 2, y(pitch) + laneH / 2);
    }
  }

  ctx.strokeStyle = COLOUR.grid;
  ctx.lineWidth = 1;
  for (let s = 1; s * 1e6 < span; s++) {
    const gx = Math.round(x(now - s * 1e6)) + 0.5;
    ctx.beginPath();
    ctx.moveTo(gx, 0);
    ctx.lineTo(gx, ROLL_H);
    ctx.stroke();
  }

  for (const note of notes) {
    const start = Math.max(from, note.start);
    const end = Math.min(now, note.end ?? now);
    const left = x(start);
    const right = Math.max(left + 2, x(end));
    const top = y(note.pitch) + Math.min(1, laneH / 6);
    const barH = Math.max(2, laneH - Math.min(2, laneH / 3));
    ctx.globalAlpha = note.direction === 'in' ? 0.45 : 0.35 + 0.65 * (note.velocity / 127);
    ctx.fillStyle = note.direction === 'in' ? COLOUR.noteIn : COLOUR.noteOut;
    ctx.fillRect(left, top, right - left, barH);
    ctx.globalAlpha = 1;
    if (note.end === null) {
      ctx.strokeStyle = ctx.fillStyle;
      ctx.lineWidth = 1;
      ctx.strokeRect(left + 0.5, top + 0.5, right - left - 1, barH - 1);
    }
  }

  // The playhead, at the right edge: the notes scroll under it.
  ctx.fillStyle = COLOUR.dim;
  ctx.fillRect(width - 1, 0, 1, ROLL_H);

  if (!notes.length) {
    ctx.fillStyle = COLOUR.dim;
    ctx.font = '12px ui-sans-serif, system-ui, sans-serif';
    ctx.fillText('nothing playing — start the clock, or press a key below', ROLL_GUTTER + 8, ROLL_H / 2);
  }
}

// --- the canvas ------------------------------------------------------------

// A canvas has two sizes, and getting them confused is how a chart ends up
// blurred on a phone: the CSS box the page lays out, and the pixel buffer it
// is drawn into. The buffer follows the box times the device pixel ratio, and
// the context is scaled so everything above can be written in CSS pixels.
function fit(canvas, cssHeight) {
  const width = canvas.clientWidth;
  if (!width) return null;                          // laid out but not visible yet
  const ratio = globalThis.devicePixelRatio || 1;
  const w = Math.round(width * ratio);
  const h = Math.round(cssHeight * ratio);
  if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
  if (canvas.style.height !== `${cssHeight}px`) canvas.style.height = `${cssHeight}px`;
  const ctx = canvas.getContext('2d');
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
  return ctx;
}
