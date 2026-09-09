// The views. Everything here is driven by what the device reported: inlet and
// outlet names and domains, parameter ranges and kinds, enum option names, and
// the module's real bus counts. Nothing about any specific algorithm is
// hardcoded, so an algorithm added to the firmware gets a working panel for
// free - and, since the registry now carries port names and a summary, a
// *described* one rather than a grid of numbered sockets.

import * as P from './protocol.js';
import { Domain, busCount, domainName } from './validate.js';

const el = (tag, attrs = {}, ...children) => {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(attrs)) {
    if (key === 'class') node.className = value;
    else if (key.startsWith('on')) node.addEventListener(key.slice(2), value);
    else if (value !== null && value !== undefined) node.setAttribute(key, value);
  }
  for (const child of children.flat()) {
    if (child === null || child === undefined) continue;
    node.append(child.nodeType ? child : document.createTextNode(String(child)));
  }
  return node;
};
export { el };

// A slider a scrolling finger cannot change.
//
// A native range input takes any touch that lands on it: the value jumps to
// where the finger touched down, the page then starts scrolling under it, and
// the gesture ends with a `change` that writes a value nobody chose. On a
// phone, where the whole editor is one long scroll and every parameter has a
// slider across it, that is not an edge case - it is what scrolling the patch
// tab does. The event trace is plain: `pointerdown`, `input` (jumped),
// `pointercancel` (the page took the gesture), `touchend`, `change`.
//
// So a touch has to *claim* the slider before it may move it: either by
// dragging along it - the axis it reads - or by holding still on it for a
// moment, which is a press rather than the start of a swipe. Until then every
// value the input produces is put straight back, and a gesture the browser
// cancels for scrolling puts it back too and commits nothing. A mouse or a
// stylus claims it on contact: neither is trying to scroll the page.
const CLAIM_PX = 8;    // a drag along the slider, far enough not to be a flick
const CLAIM_MS = 250;  // or a press held still, which is not a swipe either

export function slider(attrs, { onInput, onCommit } = {}) {
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

// One horizontal scroller, remembered. A lane is wider than a phone - 32 steps
// is several screens of it - so it scrolls inside its own box rather than
// widening the page; and because the page is rebuilt wholesale on every edit,
// the box has to be told where it was, or toggling step 20 would scroll the
// pattern back to step 1 and put the next step off the screen.
function scroller(app, key, ...children) {
  return el('div', {
    class: 'lane-scroll', 'data-scroll': key,
    onscroll: (e) => app.scrolled?.set(key, e.target.scrollLeft),
  }, ...children);
}

// What a port is called, from the device, falling back to the index for a
// module whose firmware predates port names.
export const inletName = (d, i) => d.inName?.[i] || `in ${i}`;
export const outletName = (d, i) => d.outName?.[i] || `out ${i}`;

// A bus selector for one inlet or outlet. The options are only the buses of
// the right domain, because the editor only offers domain-compatible
// connections - the module would refuse anything else.
function busSelect(caps, domain, value, optional, onChange) {
  const select = el('select', { class: `bus bus-${domainName(domain)}`, onchange: (e) => {
    onChange(e.target.value === 'none' ? P.NO_BUS : Number(e.target.value));
  } });
  const none = el('option', { value: 'none' }, optional ? 'not connected' : '— must be connected');
  if (value === P.NO_BUS) none.selected = true;
  select.append(none);
  for (let b = 0; b < busCount(caps, domain); b++) {
    const option = el('option', { value: String(b) }, `${domainName(domain)} bus ${b}`);
    if (b === value) option.selected = true;
    select.append(option);
  }
  return select;
}

// Who else is on this bus. A bus *is* the connection, so the thing a patch
// cable would have shown - what this inlet is actually listening to - has to
// be said in words, or the patch is a list of numbers that happen to match.
function busNeighbours(app, domain, bus, self) {
  if (bus === P.NO_BUS) return null;
  const writers = [];
  const readers = [];
  app.patch.nodes.forEach((node, index) => {
    const d = app.device.byId.get(node.algorithmId);
    if (!d) return;
    for (let i = 0; i < d.nOut && i < P.MAX_OUT; i++) {
      if (node.outBus[i] === bus && d.outDomain[i] === domain && !(index === self.index && self.isOutlet && self.port === i)) {
        writers.push(`${d.name} ${index} ${outletName(d, i)}`);
      }
    }
    for (let i = 0; i < d.nIn && i < P.MAX_IN; i++) {
      if (node.inBus[i] === bus && d.inDomain[i] === domain && !(index === self.index && !self.isOutlet && self.port === i)) {
        readers.push(`${d.name} ${index} ${inletName(d, i)}`);
      }
    }
  });
  if (domain === Domain.Gate) {
    app.patch.gatePorts.forEach((port, i) => {
      if (port.bus !== bus) return;
      if (port.direction === P.GatePortDirection.GATE_PORT_IN) writers.push(`jack ${i + 1}`);
      if (port.direction === P.GatePortDirection.GATE_PORT_OUT) readers.push(`jack ${i + 1}`);
    });
  }
  if (domain === Domain.Note) {
    app.patch.midiIn.forEach((port, i) => {
      if (port.sourceMask && port.bus === bus) writers.push(`MIDI in ${i + 1}`);
    });
    app.patch.midiOut.forEach((port, i) => {
      if (port.targetMask && port.bus === bus) readers.push(`MIDI out ${i + 1}`);
    });
  }
  const parts = [];
  if (writers.length) parts.push(`from ${writers.join(', ')}`);
  if (readers.length) parts.push(`to ${readers.join(', ')}`);
  if (parts.length) return el('span', { class: 'wire' }, parts.join(' · '));
  // An inlet nobody writes is the warning `advise` raises: the node will read
  // silence. An outlet nobody reads is ordinary - a spare drum lane, an output
  // waiting for a jack - so it is said, not flagged.
  return self.isOutlet
    ? el('span', { class: 'wire' }, 'nothing reads this bus yet')
    : el('span', { class: 'wire empty' }, 'nothing writes this bus');
}

function port(app, index, isOutlet, i) {
  const node = app.patch.nodes[index];
  const d = app.device.byId.get(node.algorithmId);
  const caps = app.device.capabilities;
  const domain = isOutlet ? d.outDomain[i] : d.inDomain[i];
  const bus = isOutlet ? node.outBus[i] : node.inBus[i];
  const name = isOutlet ? outletName(d, i) : inletName(d, i);
  const optional = isOutlet || i >= d.minIn;

  return el('div', { class: `port bus-${domainName(domain)}` },
    el('label', { class: 'port-head' },
      el('span', { class: 'port-name' }, name,
        optional ? null : el('span', { class: 'required', title: 'this inlet must be connected' }, '*')),
      busSelect(caps, domain, bus, optional, (chosen) => {
        if (isOutlet) node.outBus[i] = chosen; else node.inBus[i] = chosen;
        app.edit(() => app.device.setConnection(index, isOutlet, i, chosen), 'connection');
        app.render();
      })),
    busNeighbours(app, domain, bus, { index, isOutlet, port: i }));
}

// One node: what it reads, what it writes, and the buses they are on. Buses
// *are* the connections - there is no cable to draw - so each port says what
// it is for and what else is on its bus.
export function nodeCard(app, index) {
  const patch = app.patch;
  const node = patch.nodes[index];
  const d = app.device.byId.get(node.algorithmId);
  if (!d) return el('section', { class: 'node bad' }, `node ${index}: unknown algorithm ${node.algorithmId}`);

  const inlets = [];
  for (let i = 0; i < d.nIn; i++) inlets.push(port(app, index, false, i));
  const outlets = [];
  for (let i = 0; i < d.nOut; i++) outlets.push(port(app, index, true, i));

  return el('section', { class: 'node', id: `node-${index}` },
    el('header', {},
      el('span', { class: 'node-index' }, index),
      el('h3', {}, d.name),
      d.wantsTick ? el('span', { class: 'tag', title: 'runs from the master clock' }, 'clocked') : null,
      el('button', { class: 'ghost danger', onclick: () => app.removeNode(index) }, 'remove')),
    d.summary ? el('p', { class: 'summary' }, d.summary) : null,
    el('div', { class: 'ports' },
      el('div', { class: 'port-group' },
        el('h4', {}, inlets.length ? 'reads' : 'reads nothing'), inlets),
      el('div', { class: 'port-group' },
        el('h4', {}, outlets.length ? 'writes' : 'writes nothing'), outlets)),
    paramPanel(app, index),
    gridPanel(app, index));
}

// A control per parameter, drawn from the descriptor: a range for a number, a
// list for an enum, a checkbox for a boolean. An editor cannot draw a control
// for a parameter whose range and meaning it does not know, which is why #20
// exists.
function paramPanel(app, index) {
  const node = app.patch.nodes[index];
  const d = app.device.byId.get(node.algorithmId);
  const groups = d.params;
  if (!groups) return el('div', { class: 'params loading' }, 'reading parameters…');

  const controls = [];
  for (const group of groups) {
    if (!group) continue;
    // Tables - a sequencer's steps, a drum machine's lanes - get a grid of
    // their own below rather than several hundred number fields.
    if (group.repeat > 1) continue;
    for (let f = 0; f < group.nFields; f++) {
      const pd = group.fields[f];
      const at = group.first + f;
      if (!pd || (pd.min === 0 && pd.max === 0)) continue;      // reserved
      controls.push(paramControl(app, index, at, pd));
    }
  }
  if (!controls.length) return null;
  return el('div', { class: 'params' }, controls);
}

// What a stored byte means, in the parameter's own terms. Zero means the
// descriptor's default everywhere (param.h), and a signed parameter keeps an
// int8 in the byte, so neither can be shown as the raw number.
export function paramText(pd, stored) {
  const effective = stored === 0 ? pd.def : stored;
  if (pd.kind === P.ParamKind.PARAM_SIGNED) {
    const signed = stored > 127 ? stored - 256 : stored;
    return signed > 0 ? `+${signed}` : String(signed);
  }
  if (pd.kind === P.ParamKind.PARAM_ENUM) return pd.options?.[effective - pd.min] ?? String(effective);
  if (pd.kind === P.ParamKind.PARAM_BOOL) return stored ? 'on' : 'off';
  if (pd.kind === P.ParamKind.PARAM_MILLIS) return `${effective} ms`;
  if (pd.kind === P.ParamKind.PARAM_PERCENT) return `${effective} %`;
  if (pd.kind === P.ParamKind.PARAM_PITCH) return `${noteName(effective)} (${effective})`;
  if (pd.kind === P.ParamKind.PARAM_PITCH_CLASS) return NAMES[effective % 12];
  if (pd.kind === P.ParamKind.PARAM_CHANNEL) return effective === 0 ? 'omni' : `ch ${effective}`;
  if (pd.kind === P.ParamKind.PARAM_BITFIELD) return `0b${effective.toString(2).padStart(8, '0')}`;
  return String(effective);
}

function paramControl(app, index, at, pd) {
  const node = app.patch.nodes[index];
  const value = node.params[at];
  const write = (v) => {
    const clamped = Math.max(0, Math.min(255, v | 0));
    node.params[at] = clamped;
    app.edit(() => app.device.setParam(index, at, clamped), 'parameter');
    app.render();
  };

  const controls = [];
  if (pd.kind === P.ParamKind.PARAM_ENUM) {
    const select = el('select', { class: 'grow', onchange: (e) => write(Number(e.target.value)) });
    for (let v = pd.min; v <= pd.max; v++) {
      const option = el('option', { value: String(v) }, pd.options[v - pd.min] ?? String(v));
      if (v === (value || pd.def)) option.selected = true;
      select.append(option);
    }
    controls.push(select);
  } else if (pd.kind === P.ParamKind.PARAM_BOOL) {
    const box = el('input', { type: 'checkbox', class: 'switch', onchange: (e) => write(e.target.checked ? 1 : 0) });
    box.checked = value !== 0;
    controls.push(el('label', { class: 'bool' }, box, el('span', {}, value ? 'on' : 'off')));
  } else {
    // A slider *and* a number field. A slider is unusable for a precise value
    // and hopeless on a phone, where a 1px drag is a whole step of a 255-wide
    // range; a number field alone loses the sweep. Neither replaces the
    // other, so both write the same parameter - the slider across a row of
    // its own, which is what buys back the pixels a step is worth, and
    // through `slider`, so a finger scrolling the tab does not write one.
    const shown = value === 0 ? pd.def : value;
    const number = el('input', {
      type: 'number', class: 'number', min: String(pd.min), max: String(pd.max), step: '1',
      value: String(shown), 'aria-label': `${pd.name}, as a number`, inputmode: 'numeric',
    });
    const range = slider({
      class: 'slider', min: String(pd.min), max: String(pd.max), step: '1',
      value: String(shown), 'aria-label': pd.name,
    }, {
      onInput: (v) => { number.value = v; },
      onCommit: (v) => write(Number(v)),
    });
    number.addEventListener('change', (e) => {
      const v = Math.max(pd.min, Math.min(pd.max, Number(e.target.value) || 0));
      range.value = String(v);
      write(v);
    });
    controls.push(range, number);
  }

  const binding = app.bindingFor?.(index, at);
  return el('div', { class: 'param' },
    el('div', { class: 'param-head' },
      el('span', { class: 'param-name', title: `parameter ${at}, ${pd.min}..${pd.max}` }, pd.name),
      el('span', { class: 'param-value' }, paramText(pd, value))),
    el('div', { class: 'param-controls' }, controls,
      el('button', {
        class: `ghost learn ${binding ? 'bound' : ''}`,
        title: binding
          ? `CC ${binding.cc} on ${binding.channel === 0 ? 'any channel' : `channel ${binding.channel}`}`
          : 'bind a controller to this, or set one up by hand in MIDI control',
        onclick: () => app.learn(index, at),
      }, binding ? `CC ${binding.cc}` : 'learn')));
}

// The generic parameter view is wrong for a sequencer: nobody enters a drum
// pattern as a list of numbers. These are the purpose-built views.
function gridPanel(app, index) {
  const node = app.patch.nodes[index];
  const d = app.device.byId.get(node.algorithmId);
  switch (d.name) {
    case 'StepSequencer':  return stepGrid(app, index);
    case 'DrumSeqGate':
    case 'DrumSeqMidi':    return drumGrid(app, index, d.name === 'DrumSeqMidi');
    case 'NoteSequencer':
    case 'PolySequencer':  return noteLane(app, index, d.name === 'PolySequencer');
    default:               return null;
  }
}

// Steps across, click to toggle. The pattern lives in four bytes as one bit
// per step, so a click is one parameter write.
function stepGrid(app, index) {
  const node = app.patch.nodes[index];
  const length = node.params[0] || 8;
  const cells = [];
  for (let step = 0; step < P.MAX_SEQUENCE_LEN; step++) {
    const byte = 3 + (step >> 3);
    const bit = 1 << (step & 7);
    const on = (node.params[byte] & bit) !== 0;
    cells.push(el('button', {
      // The id is how the running node's position is painted onto the grid
      // every tick without re-rendering it: see perform.js.
      id: `cell-${index}-0-${step}`,
      class: `cell ${on ? 'on' : ''} ${step >= length ? 'beyond' : ''}`,
      title: `step ${step + 1}${step >= length ? ' (past the length, kept but not played)' : ''}`,
      onclick: () => {
        node.params[byte] ^= bit;
        const value = node.params[byte];
        app.edit(() => app.device.setParam(index, byte, value), 'step');
        app.render();
      },
    }, ''));
  }
  return el('div', { class: 'grid' },
    el('div', { class: 'grid-title' }, `steps — ${length} of ${P.MAX_SEQUENCE_LEN} play; the rest are kept`),
    scroller(app, `grid-${index}`,
      el('div', { class: 'lanes' },
        el('div', { class: 'lane-row' }, el('div', { class: 'lane' }, cells)))));
}

// Lanes down, steps across, with each lane's own length visible: lanes can
// differ, and that is the polyrhythm the node exists for.
function drumGrid(app, index, isMidi) {
  const node = app.patch.nodes[index];
  const d = app.device.byId.get(node.algorithmId);
  const LANE_BASE = 16, LANE_STRIDE = 8;
  const VELOCITY_BASE = LANE_BASE + P.DRUM_SEQ_LANES * LANE_STRIDE;
  const headerLength = node.params[0] || 16;
  const lanes = [];

  for (let lane = 0; lane < P.DRUM_SEQ_LANES; lane++) {
    const laneAt = LANE_BASE + lane * LANE_STRIDE;
    const ownLength = node.params[laneAt + (isMidi ? 2 : 4)];
    const length = ownLength || headerLength;
    const cells = [];
    for (let step = 0; step < P.MAX_SEQUENCE_LEN; step++) {
      let on, write;
      if (isMidi) {
        const at = VELOCITY_BASE + lane * P.MAX_SEQUENCE_LEN + step;
        on = node.params[at] !== 0;
        write = () => {
          node.params[at] = on ? 0 : 100;
          const value = node.params[at];
          app.edit(() => app.device.setParam(index, at, value), 'step');
        };
      } else {
        const at = laneAt + (step >> 3);
        const bit = 1 << (step & 7);
        on = (node.params[at] & bit) !== 0;
        write = () => {
          node.params[at] ^= bit;
          const value = node.params[at];
          app.edit(() => app.device.setParam(index, at, value), 'step');
        };
      }
      cells.push(el('button', {
        id: `cell-${index}-${lane}-${step}`,
        class: `cell ${on ? 'on' : ''} ${step >= length ? 'beyond' : ''}`,
        title: `lane ${lane + 1}, step ${step + 1}`,
        onclick: () => { write(); app.render(); },
      }, ''));
    }
    // The gate variant has one outlet per lane, so a lane's row says which
    // outlet it is - which is the whole reason the outlets are named.
    const outlet = !isMidi && lane < d.nOut ? outletName(d, lane) : `lane ${lane + 1}`;
    lanes.push(el('div', { class: 'lane-row' },
      el('span', { class: 'lane-name' }, `${outlet} (${length})`),
      el('div', { class: 'lane' }, cells)));
  }
  // Every lane in one scroller, not one each: eight lanes that scroll
  // separately are eight patterns you cannot read against each other, and the
  // polyrhythm is the whole point of the node. The names stay put while the
  // steps move under them (`.lane-name` is sticky), which is what makes the
  // grid readable on a screen narrower than the pattern.
  return el('div', { class: 'grid' },
    el('div', { class: 'grid-title' }, 'pattern'),
    scroller(app, `grid-${index}`, el('div', { class: 'lanes' }, lanes)));
}

// A lane over **scale degrees**, because that is what the sequencer stores.
// The pitch each degree resolves to against the current root and scale is
// shown alongside, and changing the root moves the pitches without touching
// the stored pattern.
function noteLane(app, index, isPoly) {
  const node = app.patch.nodes[index];
  const STEP_BASE = 16;
  const voices = isPoly ? P.NOTE_SEQ_VOICES : 1;
  const stride = voices * 2 + 2;
  const length = node.params[0] || 8;
  const root = node.params[5] || 60;
  const mask = node.params[3] | ((node.params[4] & 0x0f) << 8);

  const rows = [];
  for (let voice = 0; voice < voices; voice++) {
    const cells = [];
    for (let step = 0; step < P.MAX_SEQUENCE_LEN; step++) {
      const at = STEP_BASE + step * stride + voice * 2;
      const degree = node.params[at] > 127 ? node.params[at] - 256 : node.params[at];
      const velocity = node.params[at + 1];
      const flags = node.params[STEP_BASE + step * stride + voices * 2];
      const rest = (flags & 0x20) !== 0;
      const tie = (flags & 0x40) !== 0;
      const pitch = velocity ? root + degreeToSemitone(degree, mask) : null;
      cells.push(el('div', {
        id: voice === 0 ? `cell-${index}-0-${step}` : null,
        class: `note-cell ${velocity ? 'on' : ''} ${step >= length ? 'beyond' : ''}`
             + `${rest ? ' rest' : ''}${tie ? ' tie' : ''}`,
        title: pitch === null ? `step ${step + 1}: silent` : `step ${step + 1}: degree ${degree} → ${noteName(pitch)}`,
      },
        el('input', {
          type: 'number', value: String(degree), min: '-64', max: '63', inputmode: 'numeric',
          'aria-label': `step ${step + 1} degree`,
          onchange: (e) => {
            const v = Number(e.target.value) & 0xff;
            node.params[at] = v;
            if (!node.params[at + 1]) node.params[at + 1] = 100;
            const degreeValue = node.params[at];
            const velocityValue = node.params[at + 1];
            app.edit(async () => {
              await app.device.setParam(index, at, degreeValue);
              await app.device.setParam(index, at + 1, velocityValue);
            }, 'step');
            app.render();
          },
        }),
        el('span', { class: 'pitch' }, pitch === null ? '·' : noteName(pitch))));
    }
    rows.push(el('div', { class: 'lane-row' },
      el('span', { class: 'lane-name' }, isPoly ? `voice ${voice + 1}` : 'notes'),
      el('div', { class: 'lane notes' }, cells)));
  }
  return el('div', { class: 'grid' },
    el('div', { class: 'grid-title' },
      `degrees against root ${noteName(root)} — the stored pattern does not change when the root does`),
    scroller(app, `grid-${index}`, el('div', { class: 'lanes' }, rows)));
}

// Mirrors midi/scale.h so the displayed pitch is the one the module will play.
function scaleIntervals(mask) {
  const bits = (mask & 0x0fff) || 0x0fff;
  const out = [];
  for (let i = 0; i < 12; i++) if (bits & (1 << i)) out.push(i);
  return out;
}
export function degreeToSemitone(degree, mask) {
  const intervals = scaleIntervals(mask);
  const n = intervals.length;
  let octave = Math.floor(degree / n);
  let index = degree % n;
  if (index < 0) index += n;
  return octave * 12 + intervals[index];
}
const NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
export function noteName(pitch) {
  if (pitch < 0 || pitch > 127) return '—';
  return `${NAMES[pitch % 12]}${Math.floor(pitch / 12) - 1}`;
}
