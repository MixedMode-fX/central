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
//     one time axis can be read at a glance. A control signal is the same
//     question with a level instead of an edge, so the CV buses are traces too.
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
//
// **Colour is the domain, everywhere.** A gate is green, a note is orange and
// a control signal is purple in the arrows, the sockets, the bus chips and
// here. Where one view has to tell several signals of one domain apart - eight
// note buses on one roll - they are shades of that domain's colour, never a
// borrowed one, so a green trace is a gate wherever it is seen.

import * as P from './protocol.js';
import { el, noteName, iconButton } from './views.js';
import { Domain } from './validate.js';
import { inletName, outletName } from './graph.js';

const ROW_H = 18;            // one gate trace
const CV_ROW_H = 46;         // a control signal needs room to be a curve
const TRACE_FONT = '11px ui-monospace, SFMono-Regular, Menlo, monospace';
const ROLL_GUTTER = 30;      // room for a pitch name
const ROLL_H = 220;
const NODE_ROLL_H = 150;
const ROLL_SLOTS = 4;        // most sub-lanes one pitch lane is split into
const MIN_SEMITONES = 13;    // an octave, so a one-note patch is not a full-height bar

// The page's own palette, read from the stylesheet rather than written out
// again here: a canvas cannot use a CSS variable, and two copies of a colour is
// two colours the day one of them is changed. The fallbacks are what the tests
// see, where there is no stylesheet to read.
const FALLBACK = {
  gate: '#7bd88f', note: '#d8a67b', cv: '#b78bd8',
  grid: '#2c313d', dim: '#8b93a7', back: '#12141a', panel: '#1b1e26', raised: '#232833',
};
const VARIABLE = {
  gate: '--gate', note: '--note', cv: '--cv',
  grid: '--line', dim: '--dim', back: '--bg', panel: '--panel', raised: '--raised',
};
let COLOUR = FALLBACK;
let read = false;
export function palette() {
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

// --- shades of a domain -----------------------------------------------------

// The n-th shade of a colour: the same hue nudged and the lightness stepped,
// far enough apart to tell two traces of one domain from each other and near
// enough that every one of them still reads as that domain. Fixed rather than
// generated from the signal, so note bus 2 is the same shade in every patch
// and on every reload.
const SHADES = [[0, 0], [24, 9], [-22, -9], [42, 2], [-40, 11], [8, 18], [30, -12], [-14, 16], [54, -4], [-54, 7]];

function hexToHsl(hex) {
  const m = /^#([0-9a-f]{6})$/i.exec(String(hex).trim());
  if (!m) return null;
  const n = parseInt(m[1], 16);
  const r = ((n >> 16) & 255) / 255;
  const g = ((n >> 8) & 255) / 255;
  const b = (n & 255) / 255;
  const max = Math.max(r, g, b);
  const min = Math.min(r, g, b);
  const l = (max + min) / 2;
  if (max === min) return { h: 0, s: 0, l };
  const d = max - min;
  const s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
  let h;
  if (max === r) h = ((g - b) / d + (g < b ? 6 : 0)) / 6;
  else if (max === g) h = ((b - r) / d + 2) / 6;
  else h = ((r - g) / d + 4) / 6;
  return { h: h * 360, s, l };
}

export function shade(base, index) {
  const hsl = hexToHsl(base);
  if (!hsl) return base;
  const [dh, dl] = SHADES[index % SHADES.length];
  const h = ((hsl.h + dh) % 360 + 360) % 360;
  const l = Math.max(0.3, Math.min(0.85, hsl.l + dl / 100));
  return `hsl(${Math.round(h)} ${Math.round(hsl.s * 100)}% ${Math.round(l * 100)}%)`;
}

// --- the legend ---------------------------------------------------------------

// One chip per trace, in the trace's colour, and pressing one hides it: a
// legend is a list of what is on the screen, and the fastest way to look at
// three traces out of nine is to put the other six away for a moment.
export function legend(items, hidden, onToggle) {
  return el('div', { class: 'legend', role: 'group', 'aria-label': 'traces' },
    items.map((item) => el('button', {
      type: 'button', class: `legend-chip ${hidden.has(item.key) ? 'off' : ''}`,
      style: `--key:${item.colour}`,
      'aria-pressed': hidden.has(item.key) ? 'false' : 'true',
      title: hidden.has(item.key) ? `show ${item.label}` : `hide ${item.label}`,
      onclick: () => onToggle(item.key),
    }, item.label)));
}

function toggleIn(set, key) {
  if (set.has(key)) set.delete(key); else set.add(key);
}

// --- the scope --------------------------------------------------------------

// Which buses this patch actually touches. Sixteen traces of which thirteen
// never move is not a view, it is a wall - so the default is the ones the
// patch is using, and "every bus" is a checkbox for when something is firing
// that the patch does not admit to.
function usedBuses(app, domain) {
  const used = new Set();
  if (domain === Domain.Gate) {
    for (const port of app.patch.gatePorts) {
      if (port.direction !== P.GatePortDirection.GATE_PORT_UNUSED && port.bus !== P.NO_BUS) used.add(port.bus);
    }
  }
  for (const node of app.patch.nodes) {
    const d = app.device?.byId.get(node.algorithmId);
    if (!d) continue;
    for (let i = 0; i < d.nIn && i < P.MAX_IN; i++) {
      if (d.inDomain[i] === domain && node.inBus[i] !== P.NO_BUS) used.add(node.inBus[i]);
    }
    for (let i = 0; i < d.nOut && i < P.MAX_OUT; i++) {
      if (d.outDomain[i] === domain && node.outBus[i] !== P.NO_BUS) used.add(node.outBus[i]);
    }
  }
  if (domain === Domain.CV) {
    for (const route of app.patch.modMap ?? []) {
      if (route && route.bus !== P.NO_BUS) used.add(route.bus);
    }
  }
  return used;
}

export function scopeRows(app) {
  const rows = [];
  const colours = palette();
  const gateBuses = app.device?.capabilities?.gateBuses ?? P.N_GATE_BUS;
  const cvBuses = Math.min(P.N_CV_BUS, app.device?.capabilities?.cvBuses ?? P.N_CV_BUS);
  for (let j = 0; j < P.GPIO_N; j++) {
    const direction = app.patch.gatePorts[j]?.direction ?? P.GatePortDirection.GATE_PORT_UNUSED;
    if (direction === P.GatePortDirection.GATE_PORT_UNUSED && !app.scopeAll) continue;
    const out = direction === P.GatePortDirection.GATE_PORT_OUT;
    const way = direction === P.GatePortDirection.GATE_PORT_UNUSED ? '–' : (out ? 'out' : 'in');
    // A jack is a gate, whichever way it faces: green, with the direction in
    // its name and a lighter shade for an output.
    rows.push({
      key: `jack${j}`, kind: 'gate', bit: j, source: out ? 'jackOut' : 'jackIn', height: ROW_H,
      label: `jack ${j + 1} ${way}`, short: `J${j + 1}${way[0]}`,
      colour: out ? shade(colours.gate, 1) : colours.gate,
    });
  }
  const gates = usedBuses(app, Domain.Gate);
  for (let b = 0; b < gateBuses; b++) {
    if (!app.scopeAll && !gates.has(b)) continue;
    rows.push({ key: `gate${b}`, kind: 'gate', bit: b, source: 'gate', height: ROW_H,
                label: `gate ${b}`, short: `g${b}`, colour: colours.gate });
  }
  const cvs = usedBuses(app, Domain.CV);
  for (let b = 0; b < cvBuses; b++) {
    if (!app.scopeAll && !cvs.has(b)) continue;
    rows.push({ key: `cv${b}`, kind: 'cv', bit: b, source: 'cv', height: CV_ROW_H,
                label: `CV ${b}`, short: `c${b}`, colour: shade(colours.cv, b) });
  }
  return rows;
}

export function scopePanel(app) {
  const rows = scopeRows(app);
  app.scopeHidden ??= new Set();
  const canvas = el('canvas', { class: 'scope', id: 'scope',
                                'aria-label': 'jack, gate bus and CV bus levels over the last four seconds' });
  canvas.traceRows = rows.filter((row) => !app.scopeHidden.has(row.key));
  const all = el('input', { type: 'checkbox', class: 'switch',
                            onchange: (e) => { app.scopeAll = e.target.checked; app.render(); } });
  all.checked = Boolean(app.scopeAll);
  return el('section', { class: 'panel' },
    el('h2', {}, 'scope'),
    rows.length
      ? el('div', { class: 'scope-wrap' }, canvas)
      : el('p', { class: 'hint' }, 'no jack, no gate bus, no CV bus'),
    rows.length
      ? legend(rows, app.scopeHidden, (key) => { toggleIn(app.scopeHidden, key); app.render(); })
      : null,
    el('div', { class: 'row' },
      el('label', { class: 'bool' }, all, el('span', {}, 'every jack and bus'))));
}

export function drawScope(app) {
  const canvas = document.getElementById('scope');
  if (!canvas || !app.module) return;
  const rows = canvas.traceRows ?? [];
  if (!rows.length) return;
  const height = rows.reduce((sum, row) => sum + row.height, 0) + 12;
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

  // The axis is *time*, not "however many columns there are": a module that
  // has been running for a fifth of a second draws a fifth of a second at the
  // right-hand edge, under the same one-second grid lines, rather than
  // stretching it across four.
  const shown = (count / trace.len) * span;
  const left = Math.floor(span - shown);
  const columnsAt = (px) => {
    const at = (px - (span - shown)) / shown;
    const from = Math.floor(at * count);
    const to = Math.max(from + 1, Math.floor((at + trace.len / (span * count)) * count));
    return [from, Math.min(to, count)];
  };

  let y0 = 4;
  rows.forEach((row, r) => {
    const top = y0;
    y0 += row.height;
    ctx.fillStyle = COLOUR.dim;
    ctx.fillText(labels[r], 4, top + row.height / 2);
    if (row.kind === 'cv') { drawCvRow(ctx, trace, row, top, x0, span, left, count, first, columnsAt); return; }

    const hi = top + 3;
    const lo = top + row.height - 5;
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
    ctx.strokeStyle = row.colour;
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    let previous = -1;
    for (let px = left; px <= span; px++) {
      const [from, to] = columnsAt(px);
      let value = 0;
      for (let i = from; i < to; i++) {
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

// A control signal, as a curve. Unipolar signals sit on the bottom of the
// row and bipolar ones on a centre line: which it is comes from the signal
// itself, since the bus does not say (`src/bus/domain.h`), and a signal that
// never goes negative drawn around a centre line would waste half its row.
function drawCvRow(ctx, trace, row, top, x0, span, left, count, first, columnsAt) {
  const COLOUR = palette();
  const data = trace.cv[row.bit];
  const full = P.CV_FULL;
  let negative = false;
  for (let i = 0; i < count; i++) if (data[(first + i) % trace.len] < 0) { negative = true; break; }
  const hi = top + 4;
  const lo = top + row.height - 4;
  const zero = negative ? (hi + lo) / 2 : lo;
  const scale = negative ? (lo - hi) / full : (lo - hi) / full;
  const y = (value) => Math.max(hi, Math.min(lo, zero - value * scale));

  ctx.strokeStyle = COLOUR.grid;
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(x0, Math.round(zero) + 0.5);
  ctx.lineTo(x0 + span, Math.round(zero) + 0.5);
  ctx.stroke();
  if (!count) return;

  ctx.strokeStyle = row.colour;
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  let started = false;
  let last = 0;
  for (let px = left; px <= span; px++) {
    const [from, to] = columnsAt(px);
    // The last column behind the pixel: a level, unlike an edge, is what it
    // was most recently, and a pixel too narrow for a whole cycle of an LFO
    // is a pixel the eye reads as its envelope anyway.
    if (to > from) last = data[(first + Math.max(from, to - 1)) % trace.len];
    const x = x0 + px;
    if (!started) { ctx.moveTo(x, y(last)); started = true; } else ctx.lineTo(x, y(last));
  }
  ctx.stroke();
  // The value now, at the right-hand end, in the bus's own units.
  ctx.fillStyle = row.colour;
  ctx.font = TRACE_FONT;
  ctx.textAlign = 'right';
  ctx.fillText(String(last), x0 + span - 2, negative ? hi + 5 : hi + 5);
  ctx.textAlign = 'left';
}

// --- the piano roll ---------------------------------------------------------

// What can appear in the roll, in the order it is stacked and listed: what was
// played in, what the module sent out, and then each note bus the patch writes.
// Every one is a shade of the note colour - a note is a note wherever it was
// seen - and a bus keeps its shade in every roll on the page, so the bus a
// node writes is the same colour under the node as it is on the play tab.
export const noteBusColour = (bus) => shade(palette().note, bus + 1);

export function rollSources(app) {
  const colours = palette();
  const sources = [
    { key: 'in', label: 'played in', colour: shade(colours.note, 9) },
    { key: 'out', label: 'sent out', colour: colours.note },
  ];
  for (const bus of [...(app.watchedBuses ?? [])].sort((a, b) => a - b)) {
    sources.push({ key: `bus${bus}`, bus, label: `note bus ${bus}`, colour: noteBusColour(bus) });
  }
  return sources;
}

const sourceKey = (note) => (note.direction === 'bus' ? `bus${note.bus}` : note.direction);

export function rollPanel(app) {
  app.rollHidden ??= new Set();
  const canvas = el('canvas', { class: 'roll', id: 'roll',
                                'aria-label': 'the notes of the last eight seconds' });
  const sources = rollSources(app);
  canvas.rollSources = sources.filter((source) => !app.rollHidden.has(source.key));
  canvas.rollHeight = ROLL_H;
  return el('section', { class: 'panel' },
    el('h2', {}, 'piano roll'),
    el('div', { class: 'scope-wrap' }, canvas),
    el('div', { class: 'row legend-row' },
      legend(sources, app.rollHidden, (key) => { toggleIn(app.rollHidden, key); app.render(); }),
      iconButton({ icon: 'clear', label: 'clear the roll', class: 'ghost',
                   onclick: () => { app.module.clearNotes(); } })));
}

// The roll under one node: what it reads on its note inlets and what it
// writes on its note outlets, on one time line. An arpeggiator, a chord node
// or a Tonnetz is a transformation, and the only way to see a transformation
// is to see both sides of it - the held chord and the figure it became, the
// walk and the triads it visited. The buses keep the shades they have on the
// play tab's roll, so the two views agree.
export function nodeRollSources(app, index) {
  const node = app.patch.nodes[index];
  const d = app.device?.byId.get(node?.algorithmId);
  if (!d) return [];
  const sources = [];
  const seen = new Set();
  const add = (bus, role, name) => {
    if (bus === P.NO_BUS || seen.has(bus)) return;
    seen.add(bus);
    sources.push({ key: `bus${bus}`, bus, role, colour: noteBusColour(bus),
                   label: `${role === 'in' ? 'reads' : 'writes'} ${name} · bus ${bus}` });
  };
  for (let i = 0; i < d.nIn && i < P.MAX_IN; i++) {
    if (d.inDomain[i] === Domain.Note) add(node.inBus[i], 'in', inletName(d, i));
  }
  for (let i = 0; i < d.nOut && i < P.MAX_OUT; i++) {
    if (d.outDomain[i] === Domain.Note) add(node.outBus[i], 'out', outletName(d, i));
  }
  return sources;
}

export function nodeRollPanel(app, index) {
  if (!app.module || !app.usingModule) return null;
  const sources = nodeRollSources(app, index);
  if (!sources.length) return null;
  app.rollHidden ??= new Set();
  const hiddenKey = (key) => `node:${index}:${key}`;
  const canvas = el('canvas', { class: 'roll', id: `roll-node-${index}`, 'data-node': String(index),
                                'aria-label': `the notes this node read and wrote in the last eight seconds` });
  canvas.rollSources = sources.filter((source) => !app.rollHidden.has(hiddenKey(source.key)));
  canvas.rollHeight = NODE_ROLL_H;
  const items = sources.map((source) => ({ ...source, key: hiddenKey(source.key) }));
  return el('div', { class: 'grid node-roll' },
    el('div', { class: 'grid-title' }, 'what it plays'),
    el('div', { class: 'scope-wrap' }, canvas),
    legend(items, app.rollHidden, (key) => { toggleIn(app.rollHidden, key); app.render(); }));
}

export function drawRoll(app) {
  drawRollOn(app, document.getElementById('roll'));
  for (const canvas of document.querySelectorAll('canvas.roll[data-node]')) drawRollOn(app, canvas);
}

function drawRollOn(app, canvas) {
  if (!canvas || !app.module) return;
  const height = canvas.rollHeight ?? ROLL_H;
  const ctx = fit(canvas, height);
  if (!ctx) return;
  const width = canvas.clientWidth;
  const COLOUR = palette();
  const module = app.module;
  const span = module.rollSpan();
  const now = module.now;
  const from = now - span;
  const sources = canvas.rollSources ?? [];
  const byKey = new Map(sources.map((source) => [source.key, source]));
  const notes = module.notes.filter((n) => (n.end ?? now) >= from && byKey.has(sourceKey(n)));

  ctx.fillStyle = COLOUR.back;
  ctx.fillRect(0, 0, width, height);

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
  const laneH = height / lanes;
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
    ctx.lineTo(gx, height);
    ctx.stroke();
  }

  // A pitch lane is split between the sources actually playing in it, so the
  // same note seen in two places is two bars rather than one drawn over the
  // other. Only the sources present get a slot: a roll with one source in it
  // uses the whole lane, as it should.
  const present = sources.filter((source) => notes.some((note) => sourceKey(note) === source.key));
  const slots = Math.max(1, Math.min(ROLL_SLOTS, present.length));
  const slotOf = new Map(present.map((source, i) => [source.key, Math.min(i, slots - 1)]));

  for (const note of notes) {
    const key = sourceKey(note);
    const source = byKey.get(key);
    const start = Math.max(from, note.start);
    const end = Math.min(now, note.end ?? now);
    const left = x(start);
    const right = Math.max(left + 2, x(end));
    const slot = slotOf.get(key) ?? 0;
    const slotH = laneH / slots;
    const top = y(note.pitch) + slot * slotH;
    const barH = Math.max(1.5, slotH - Math.min(1.5, slotH / 4));
    // What a node reads is context for what it wrote, so it is drawn fainter.
    const faint = note.direction === 'in' || source?.role === 'in';
    ctx.globalAlpha = faint ? 0.45 : 0.4 + 0.6 * (note.velocity / 127);
    ctx.fillStyle = source?.colour ?? COLOUR.dim;
    ctx.fillRect(left, top, right - left, barH);
    ctx.globalAlpha = 1;
    if (note.end === null) {
      ctx.strokeStyle = ctx.fillStyle;
      ctx.lineWidth = 1;
      ctx.strokeRect(left + 0.5, top + 0.5, right - left - 1, Math.max(1, barH - 1));
    }
  }

  // The playhead, at the right edge: the notes scroll under it.
  ctx.fillStyle = COLOUR.dim;
  ctx.fillRect(width - 1, 0, 1, height);

  if (!notes.length) {
    ctx.fillStyle = COLOUR.dim;
    ctx.font = '12px ui-sans-serif, system-ui, sans-serif';
    ctx.fillText('nothing playing', ROLL_GUTTER + 8, height / 2);
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
