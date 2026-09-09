// The views. Everything here is driven by what the device reported: inlet and
// outlet domains, parameter ranges and kinds, enum option names, and the
// module's real bus counts. Nothing about any specific algorithm is hardcoded,
// so an algorithm added to the firmware gets a working panel for free.

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

// A bus selector for one inlet or outlet. The options are only the buses of
// the right domain, because the editor only offers domain-compatible
// connections - the module would refuse anything else.
function busSelect(caps, domain, value, optional, onChange) {
  const select = el('select', { class: `bus bus-${domainName(domain)}`, onchange: (e) => {
    onChange(e.target.value === 'none' ? P.NO_BUS : Number(e.target.value));
  } });
  const none = el('option', { value: 'none' }, optional ? '—' : '— (required)');
  if (value === P.NO_BUS) none.selected = true;
  select.append(none);
  for (let b = 0; b < busCount(caps, domain); b++) {
    const option = el('option', { value: String(b) }, `${domainName(domain)} ${b}`);
    if (b === value) option.selected = true;
    select.append(option);
  }
  return select;
}

// One node: its inlets, its outlets, and the buses they are on. Buses *are*
// the connections - there is no cable to draw, and a patch is legible as a
// list of what each node reads and writes.
export function nodeCard(app, index) {
  const patch = app.patch;
  const node = patch.nodes[index];
  const d = app.device.byId.get(node.algorithmId);
  const caps = app.device.capabilities;
  if (!d) return el('section', { class: 'node bad' }, `node ${index}: unknown algorithm ${node.algorithmId}`);

  const inlets = [];
  for (let i = 0; i < d.nIn; i++) {
    inlets.push(el('label', { class: 'port' },
      el('span', { class: 'port-name' }, `in ${i}`),
      busSelect(caps, d.inDomain[i], node.inBus[i], i >= d.minIn, (bus) => {
        node.inBus[i] = bus;
        app.edit(() => app.device.setConnection(index, false, i, bus));
      })));
  }
  const outlets = [];
  for (let i = 0; i < d.nOut; i++) {
    outlets.push(el('label', { class: 'port' },
      el('span', { class: 'port-name' }, `out ${i}`),
      busSelect(caps, d.outDomain[i], node.outBus[i], true, (bus) => {
        node.outBus[i] = bus;
        app.edit(() => app.device.setConnection(index, true, i, bus));
      })));
  }

  return el('section', { class: 'node' },
    el('header', {},
      el('span', { class: 'node-index' }, index),
      el('h3', {}, d.name),
      d.wantsTick ? el('span', { class: 'tag' }, 'clocked') : null,
      el('button', { class: 'ghost', onclick: () => app.removeNode(index) }, 'remove')),
    el('div', { class: 'ports' },
      el('div', { class: 'inlets' }, inlets),
      el('div', { class: 'outlets' }, outlets)),
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
  return el('div', { class: 'params' }, controls);
}

function paramControl(app, index, at, pd) {
  const node = app.patch.nodes[index];
  const value = node.params[at];
  const write = (v) => {
    node.params[at] = v;
    app.edit(() => app.device.setParam(index, at, v));
    app.render();
  };

  let input;
  if (pd.kind === P.ParamKind.PARAM_ENUM) {
    input = el('select', { onchange: (e) => write(Number(e.target.value)) });
    for (let v = pd.min; v <= pd.max; v++) {
      const option = el('option', { value: String(v) }, pd.options[v - pd.min] ?? String(v));
      if (v === (value || pd.def)) option.selected = true;
      input.append(option);
    }
  } else if (pd.kind === P.ParamKind.PARAM_BOOL) {
    input = el('input', { type: 'checkbox', onchange: (e) => write(e.target.checked ? 1 : 0) });
    input.checked = value !== 0;
  } else {
    input = el('input', {
      type: 'range', min: String(pd.min), max: String(pd.max), value: String(value || pd.def),
      oninput: (e) => write(Number(e.target.value)),
    });
  }

  const shown = pd.kind === P.ParamKind.PARAM_SIGNED
    ? String(value > 127 ? value - 256 : value)
    : String(value === 0 ? pd.def : value);

  return el('label', { class: 'param' },
    el('span', { class: 'param-name' }, pd.name),
    input,
    el('span', { class: 'param-value' }, shown),
    el('button', { class: 'ghost learn', title: 'bind a controller to this',
                   onclick: () => app.learn(index, at) }, 'learn'));
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
      class: `cell ${on ? 'on' : ''} ${step >= length ? 'beyond' : ''}`,
      title: `step ${step + 1}`,
      onclick: () => {
        node.params[byte] ^= bit;
        const value = node.params[byte];
        app.edit(() => app.device.setParam(index, byte, value));
        app.render();
      },
    }, ''));
  }
  return el('div', { class: 'grid' },
    el('div', { class: 'grid-title' }, `steps (length ${length})`),
    el('div', { class: 'lane' }, cells));
}

// Lanes down, steps across, with each lane's own length visible: lanes can
// differ, and that is the polyrhythm the node exists for.
function drumGrid(app, index, isMidi) {
  const node = app.patch.nodes[index];
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
          app.edit(() => app.device.setParam(index, at, value));
        };
      } else {
        const at = laneAt + (step >> 3);
        const bit = 1 << (step & 7);
        on = (node.params[at] & bit) !== 0;
        write = () => {
          node.params[at] ^= bit;
          const value = node.params[at];
          app.edit(() => app.device.setParam(index, at, value));
        };
      }
      cells.push(el('button', {
        class: `cell ${on ? 'on' : ''} ${step >= length ? 'beyond' : ''}`,
        onclick: () => { write(); app.render(); },
      }, ''));
    }
    lanes.push(el('div', { class: 'lane-row' },
      el('span', { class: 'lane-name' }, `lane ${lane + 1} (${length})`),
      el('div', { class: 'lane' }, cells)));
  }
  return el('div', { class: 'grid' }, el('div', { class: 'grid-title' }, 'pattern'), lanes);
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
        class: `note-cell ${velocity ? 'on' : ''} ${step >= length ? 'beyond' : ''}`
             + `${rest ? ' rest' : ''}${tie ? ' tie' : ''}`,
        title: pitch === null ? 'silent' : `degree ${degree} → ${noteName(pitch)}`,
      },
        el('input', {
          type: 'number', value: String(degree), min: '-64', max: '63',
          onchange: (e) => {
            const v = Number(e.target.value) & 0xff;
            node.params[at] = v;
            if (!node.params[at + 1]) node.params[at + 1] = 100;
            const degreeValue = node.params[at];
            const velocityValue = node.params[at + 1];
            app.edit(async () => {
              await app.device.setParam(index, at, degreeValue);
              await app.device.setParam(index, at + 1, velocityValue);
            });
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
    rows);
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
