// The two time views: the scope, and the piano roll.
//
// Everything else on the play tab answers "what is true now". Neither of the
// questions that matter most while a patch is being built is of that shape:
// is this the pattern I asked for - a divider, a Euclidean sequencer and a
// logic gate are only readable *over time* - and what is it playing, which a
// log of note ons cannot say and a pitch-against-time grid can.
//
// Both are drawn from the module's own sampling, once per pass of simulated
// time (`module.trace`, `module.notes`), so what is on the screen is what
// the firmware did and not what the page happened to catch it doing.
//
// **Colour is the domain, everywhere.** Where one view has to tell several
// signals of one domain apart - eight note buses on one roll - they are
// shades of that domain's colour, never a borrowed one.

import * as P from '../../protocol/generated.js';
import { Domain } from '../../core/validate.js';
import { usedBuses, inletName, outletName } from '../../core/patch.js';
import { noteName } from '../../core/music.js';
import { BlockKind } from '../../core/graph.js';
import { portNames } from '../../protocol/names.js';

const ROW_H = 18;            // one gate trace
const CV_ROW_H = 46;         // a control signal needs room to be a curve
const TRACE_FONT = '11px ui-monospace, SFMono-Regular, Menlo, monospace';
const ROLL_GUTTER = 30;      // room for a pitch name
export const NODE_ROLL_H = 150;
const ROLL_SLOTS = 4;        // most sub-lanes one pitch lane is split into
const MIN_SEMITONES = 13;    // an octave, so a one-note patch is not a full-height bar

// The page's own palette, read from the stylesheet rather than written out
// again here: a canvas cannot use a CSS variable, and two copies of a colour
// is two colours the day one of them is changed. The fallbacks are what the
// tests see, where there is no stylesheet to read.
const FALLBACK = {
  gate: '#7bd88f', note: '#d8a67b', cv: '#b78bd8',
  grid: '#2c313d', dim: '#8b93a7', back: '#12141a', panel: '#1b1e26', raised: '#232833',
};
const VARIABLE = {
  gate: '--gate', note: '--note', cv: '--cv',
  grid: '--line', dim: '--dim', back: '--bg', panel: '--panel', raised: '--raised',
};
let COLOUR = null;
export function palette() {
  if (COLOUR) return COLOUR;
  COLOUR = { ...FALLBACK };
  const style = globalThis.getComputedStyle?.(document.documentElement);
  if (!style) return COLOUR;
  for (const [key, variable] of Object.entries(VARIABLE)) {
    const value = style.getPropertyValue(variable).trim();
    if (value) COLOUR[key] = value;
  }
  return COLOUR;
}

// --- shades of a domain -----------------------------------------------------

// The n-th shade of a colour: the same hue nudged and the lightness stepped,
// far enough apart to tell two traces of one domain from each other and near
// enough that every one still reads as that domain. Fixed rather than
// generated from the signal, so note bus 2 is the same shade in every patch.
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

// --- the scope's rows ---------------------------------------------------------

// Which buses this patch actually touches. Sixteen traces of which thirteen
// never move is not a view, it is a wall - so the default is the ones the
// patch is using, and "every bus" is a switch for when something is firing
// that the patch does not admit to.
export function scopeRows({ patch, device, scopeAll = false }) {
  const rows = [];
  const colours = palette();
  const gateBuses = device?.capabilities?.gateBuses ?? P.N_GATE_BUS;
  const cvBuses = Math.min(P.N_CV_BUS, device?.capabilities?.cvBuses ?? P.N_CV_BUS);
  for (let j = 0; j < P.GPIO_N; j++) {
    const direction = patch.gatePorts[j]?.direction ?? P.GatePortDirection.GATE_PORT_UNUSED;
    if (direction === P.GatePortDirection.GATE_PORT_UNUSED && !scopeAll) continue;
    const out = direction === P.GatePortDirection.GATE_PORT_OUT;
    const way = direction === P.GatePortDirection.GATE_PORT_UNUSED ? '–' : (out ? 'out' : 'in');
    // A jack is a gate whichever way it faces: green, with the direction in
    // its name and a lighter shade for an output.
    rows.push({
      key: `jack${j}`, kind: 'gate', bit: j, source: out ? 'jackOut' : 'jackIn', height: ROW_H,
      label: `jack ${j + 1} ${way}`, short: `J${j + 1}${way[0]}`,
      colour: out ? shade(colours.gate, 1) : colours.gate,
    });
  }
  const gates = usedBuses(device, patch, Domain.Gate);
  for (let b = 0; b < gateBuses; b++) {
    if (!scopeAll && !gates.has(b)) continue;
    rows.push({ key: `gate${b}`, kind: 'gate', bit: b, source: 'gate', height: ROW_H,
                label: `gate ${b}`, short: `g${b}`, colour: colours.gate });
  }
  const cvs = usedBuses(device, patch, Domain.CV);
  for (let b = 0; b < cvBuses; b++) {
    if (!scopeAll && !cvs.has(b)) continue;
    rows.push({ key: `cv${b}`, kind: 'cv', bit: b, source: 'cv', height: CV_ROW_H,
                label: `CV ${b}`, short: `c${b}`, colour: shade(colours.cv, b) });
  }
  return rows;
}

// --- the scope --------------------------------------------------------------

export function drawScope(canvas, module, rows) {
  if (!rows.length) return;
  const height = rows.reduce((sum, row) => sum + row.height, 0) + 12;
  const ctx = fit(canvas, height);
  if (!ctx) return;
  const width = canvas.clientWidth;
  const colours = palette();

  const trace = module.trace;
  const count = trace.filled;
  const first = (trace.head - count + trace.len) % trace.len;

  // The names are measured, not guessed at: a label that overruns its gutter
  // is drawn across the trace it names, so when the long form does not fit,
  // the short one is used.
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

  ctx.fillStyle = colours.back;
  ctx.fillRect(0, 0, width, height);

  // A line per second of simulated time, so a tempo can be read off the
  // trace rather than counted.
  const seconds = (trace.len * trace.us) / 1e6;
  ctx.strokeStyle = colours.grid;
  ctx.lineWidth = 1;
  for (let s = 1; s <= seconds; s++) {
    const x = Math.round(x0 + span - (s / seconds) * span) + 0.5;
    ctx.beginPath();
    ctx.moveTo(x, 0);
    ctx.lineTo(x, height);
    ctx.stroke();
  }

  // The axis is *time*: a module that has been running for a fifth of a
  // second draws a fifth of a second at the right-hand edge rather than
  // stretching it across four.
  const shown = (count / trace.len) * span;
  const left = Math.floor(span - shown);
  const columnsAt = (px) => {
    const at = (px - (span - shown)) / shown;
    const from = Math.floor(at * count);
    const to = Math.max(from + 1, Math.floor((at + trace.len / (span * count)) * count));
    return [from, Math.min(to, count)];
  };
  const geometry = { trace, x0, span, left, count, first, columnsAt, colours };

  let y0 = 4;
  rows.forEach((row, r) => {
    const top = y0;
    y0 += row.height;
    ctx.fillStyle = colours.dim;
    ctx.fillText(labels[r], 4, top + row.height / 2);
    if (row.kind === 'cv') drawCvRow(ctx, geometry, row, top);
    else drawGateRow(ctx, geometry, row, top);
  });
}

function drawGateRow(ctx, { trace, x0, span, left, count, first, columnsAt, colours }, row, top) {
  const hi = top + 3;
  const lo = top + row.height - 5;
  ctx.strokeStyle = colours.grid;
  ctx.beginPath();
  ctx.moveTo(x0, lo + 0.5);
  ctx.lineTo(x0 + span, lo + 0.5);
  ctx.stroke();
  if (!count) return;

  // One pixel is several columns on a narrow screen, so the columns behind a
  // pixel are OR-ed rather than sampled: a pulse never disappears because the
  // trace was scaled down.
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
}

// A control signal, as a curve. Unipolar signals sit on the bottom of the
// row and bipolar ones on a centre line: which it is comes from the signal
// itself, since the bus does not say.
function drawCvRow(ctx, { trace, x0, span, left, count, first, columnsAt, colours }, row, top) {
  const data = trace.cv[row.bit];
  let negative = false;
  for (let i = 0; i < count; i++) if (data[(first + i) % trace.len] < 0) { negative = true; break; }
  const hi = top + 4;
  const lo = top + row.height - 4;
  const zero = negative ? (hi + lo) / 2 : lo;
  const scale = (lo - hi) / P.CV_FULL;
  const y = (value) => Math.max(hi, Math.min(lo, zero - value * scale));

  ctx.strokeStyle = colours.grid;
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
    // was most recently.
    if (to > from) last = data[(first + Math.max(from, to - 1)) % trace.len];
    const x = x0 + px;
    if (!started) { ctx.moveTo(x, y(last)); started = true; } else ctx.lineTo(x, y(last));
  }
  ctx.stroke();
  // The value now, at the right-hand end, in the bus's own units.
  ctx.fillStyle = row.colour;
  ctx.font = TRACE_FONT;
  ctx.textAlign = 'right';
  ctx.fillText(String(last), x0 + span - 2, hi + 5);
  ctx.textAlign = 'left';
}

// --- the note colours ---------------------------------------------------------

// A note bus keeps its shade of the note colour in every roll on the page, so
// the same bus read under two blocks is the same colour in both.
export const noteBusColour = (bus) => shade(palette().note, bus + 1);

// --- what one block carries -----------------------------------------------
//
// The signals under one block, whatever kind it is: every port it reads and
// writes, as the scope's rows for its gate and control ports and the roll's
// sources for its note ports, each over its own window. An arpeggiator, a chord node
// or a Tonnetz is a transformation, and the only way to see one is to see
// both sides of it; a divider is only readable against what clocks it; and a
// jack or a MIDI port is where the patch meets the world, so it shows the
// world's side too - the level at the jack, the notes on its cables.
//
// Every row and source keeps the key and the shade it has on the module tab,
// so a bus is the same colour in every picture of it.
const verb = (role, name) => `${role === 'in' ? 'reads' : 'writes'}${name ? ` ${name}` : ''}`;
const gateRow = (bus, role, name) => ({
  key: `gate${bus}`, kind: 'gate', bit: bus, source: 'gate', role, height: ROW_H,
  label: `${verb(role, name)} · gate ${bus}`, short: `g${bus}`, colour: palette().gate,
});
const cvRow = (bus, role, name) => ({
  key: `cv${bus}`, kind: 'cv', bit: bus, source: 'cv', role, height: CV_ROW_H,
  label: `${verb(role, name)} · CV ${bus}`, short: `c${bus}`, colour: shade(palette().cv, bus),
});
const noteSource = (bus, role, name) => ({
  key: `bus${bus}`, bus, role, colour: noteBusColour(bus), label: `${verb(role, name)} · bus ${bus}`,
});

export function blockSignals({ patch, device }, { kind, index }) {
  const rows = [];
  const sources = [];
  const seen = new Set();
  const add = (domain, bus, role, name) => {
    const key = `${domain}:${bus}`;
    if (bus === undefined || seen.has(key)) return;
    seen.add(key);
    if (domain === Domain.Gate) rows.push(gateRow(bus, role, name));
    else if (domain === Domain.CV) rows.push(cvRow(bus, role, name));
    else sources.push(noteSource(bus, role, name));
  };
  const colours = palette();

  if (kind === BlockKind.Node) {
    const node = patch.nodes[index];
    const d = device?.byId.get(node?.algorithmId);
    if (!d) return { rows, sources };
    for (let i = 0; i < d.nIn && i < P.MAX_IN; i++) {
      for (const bus of node.inBuses[i] ?? []) add(d.inDomain[i], bus, 'in', inletName(d, i));
    }
    for (let i = 0; i < d.nOut && i < P.MAX_OUT; i++) {
      for (const bus of node.outBuses[i] ?? []) add(d.outDomain[i], bus, 'out', outletName(d, i));
    }
  } else if (kind === BlockKind.Jack) {
    const port = patch.gatePorts[index];
    const direction = port?.direction ?? P.GatePortDirection.GATE_PORT_UNUSED;
    if (direction === P.GatePortDirection.GATE_PORT_UNUSED) return { rows, sources };
    const out = direction === P.GatePortDirection.GATE_PORT_OUT;
    // The jack itself, which is a gate whichever way it faces, and then the
    // buses behind it: an input writes them, an output reads them.
    rows.push({ key: `jack${index}`, kind: 'gate', bit: index, source: out ? 'jackOut' : 'jackIn',
                role: out ? 'out' : 'in', height: ROW_H,
                label: `jack ${index + 1} ${out ? 'out' : 'in'}`, short: `J${index + 1}${out ? 'o' : 'i'}`,
                colour: out ? shade(colours.gate, 1) : colours.gate });
    for (const bus of port.buses ?? []) add(Domain.Gate, bus, out ? 'in' : 'out', '');
  } else if (kind === BlockKind.MidiIn) {
    const port = patch.midiIn[index];
    if (!port?.sourceMask) return { rows, sources };
    // What arrived on its cables, then the buses it put it on.
    sources.push({ key: 'in', mask: port.sourceMask, role: 'in', colour: shade(colours.note, 9),
                   label: `played in · ${portNames(port.sourceMask).join(', ')}` });
    for (const bus of port.buses ?? []) add(Domain.Note, bus, 'out', '');
  } else if (kind === BlockKind.MidiOut) {
    const port = patch.midiOut[index];
    if (!port?.targetMask) return { rows, sources };
    for (const bus of port.buses ?? []) add(Domain.Note, bus, 'in', '');
    sources.push({ key: 'out', mask: port.targetMask, role: 'out', colour: colours.note,
                   label: `sent out · ${portNames(port.targetMask).join(', ')}` });
  }
  // Inputs first, then outputs, whatever the domain: what went in is read
  // before what came out of it, and an output jack's own level is the last
  // thing on the way out.
  const inFirst = (a, b) => (a.role === b.role ? 0 : a.role === 'in' ? -1 : 1);
  return { rows: rows.sort(inFirst), sources: sources.sort(inFirst) };
}

// The roll under one node: its note ports only.
export const nodeRollSources = (ctx, index) =>
  blockSignals(ctx, { kind: BlockKind.Node, index }).sources;

const sourceKey = (note) => (note.direction === 'bus' ? `bus${note.bus}` : note.direction);

// A source naming a cable mask shows only the notes that went through one of
// those cables: a MIDI in port's "played in" is what *it* took, not what the
// keyboard sent down another cable.
const onSource = (source, note) =>
  !source.mask || (note.port !== null && note.port !== undefined && (note.port & source.mask) !== 0);


// --- the piano roll -----------------------------------------------------------

export function drawRoll(canvas, module, sources, height) {
  const ctx = fit(canvas, height);
  if (!ctx) return;
  const width = canvas.clientWidth;
  const colours = palette();
  const span = module.rollSpan();
  const now = module.now;
  const from = now - span;
  const byKey = new Map(sources.map((source) => [source.key, source]));
  const notes = module.notes.filter((n) => (n.end ?? now) >= from
    && byKey.has(sourceKey(n)) && onSource(byKey.get(sourceKey(n)), n));

  ctx.fillStyle = colours.back;
  ctx.fillRect(0, 0, width, height);

  // The pitch range follows what is playing, rounded out to whole octaves, so
  // a bass line and a hi-hat are not squeezed onto the same two rows.
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
    const black = [1, 3, 6, 8, 10].includes(pitch % 12);
    ctx.fillStyle = black ? colours.back : colours.panel;
    ctx.fillRect(0, y(pitch), width, laneH);
    if (pitch % 12 === 0) {
      ctx.fillStyle = colours.grid;
      ctx.fillRect(ROLL_GUTTER, y(pitch) + laneH - 1, width - ROLL_GUTTER, 1);
    }
    if (laneH >= 9 || pitch % 12 === 0) {
      ctx.fillStyle = colours.dim;
      ctx.fillText(noteName(pitch), 2, y(pitch) + laneH / 2);
    }
  }

  ctx.strokeStyle = colours.grid;
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
  // other.
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
    const slotH = laneH / slots;
    const top = y(note.pitch) + (slotOf.get(key) ?? 0) * slotH;
    const barH = Math.max(1.5, slotH - Math.min(1.5, slotH / 4));
    // What a node reads is context for what it wrote, so it is drawn fainter.
    const faint = note.direction === 'in' || source?.role === 'in';
    ctx.globalAlpha = faint ? 0.45 : 0.4 + 0.6 * (note.velocity / 127);
    ctx.fillStyle = source?.colour ?? colours.dim;
    ctx.fillRect(left, top, right - left, barH);
    ctx.globalAlpha = 1;
    if (note.end === null) {
      ctx.strokeStyle = ctx.fillStyle;
      ctx.lineWidth = 1;
      ctx.strokeRect(left + 0.5, top + 0.5, right - left - 1, Math.max(1, barH - 1));
    }
  }

  // The playhead, at the right edge: the notes scroll under it.
  ctx.fillStyle = colours.dim;
  ctx.fillRect(width - 1, 0, 1, height);

  if (!notes.length) {
    ctx.fillStyle = colours.dim;
    ctx.font = '12px ui-sans-serif, system-ui, sans-serif';
    ctx.fillText('nothing playing', ROLL_GUTTER + 8, height / 2);
  }
}

// A canvas has two sizes, and getting them confused is how a chart ends up
// blurred on a phone: the CSS box the page lays out, and the pixel buffer it
// is drawn into. The buffer follows the box times the device pixel ratio, and
// the context is scaled so everything above can be written in CSS pixels.
//
// The box is measured once and then watched, not read every frame. Reading
// `clientWidth` makes the browser lay the page out there and then if anything
// has touched it since it last did - and the painters touch it every frame, a
// readout here and a lamp there - so a scope that asked every frame was
// costing a whole layout per canvas per frame, most of the frame's budget on
// the module tab. A ResizeObserver is told after layout, and forces none.
const boxes = new WeakMap();      // canvas -> { width }, its CSS box as last laid out
function cssWidth(canvas) {
  let box = boxes.get(canvas);
  if (!box) {
    box = { width: canvas.clientWidth };
    boxes.set(canvas, box);
    if (globalThis.ResizeObserver) {
      new ResizeObserver((entries) => { box.width = entries[0].contentRect.width; }).observe(canvas);
    }
  }
  return box.width;
}

function fit(canvas, cssHeight) {
  const width = cssWidth(canvas);
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
