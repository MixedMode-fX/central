// The purpose-built views of a sequencer's table: nobody enters a drum
// pattern as a list of numbers.
//
// The offsets are the firmware's parameter layout (note_sequencer.h,
// drum_sequencer.h, gate_sequencer.h), the same ones `core/patchjson.js`
// packs a `seq` block into. The step being played is outlined in the grid
// you are editing: the step being edited and the step being played are the
// same square, which is what makes this one app.

import * as P from '../../../protocol/generated.js';
import { el, classes } from '../../dom.js';
import { Scroller } from '../../controls/Scroller.js';
import { outletName } from '../../../core/patch.js';
import { noteName, degreeToSemitone } from '../../../core/music.js';
import { paintPlayhead } from './playhead.js';
import '../Grid.css';

// The box a grid lives in: a title, and the lanes in a remembered scroller.
function Grid(app, index, title, lanes) {
  return el('div', { class: 'grid' },
    el('div', { class: 'grid-title' }, title),
    Scroller(app, `grid-${index}`, el('div', { class: 'lanes' }, lanes)));
}

const Lane = (name, cells, klass = '') => el('div', { class: 'lane-row' },
  el('span', { class: 'lane-name' }, name),
  el('div', { class: classes('lane', klass) }, cells));

// The step each lane is sounding, painted onto its cells every frame.
// Nothing is rebuilt: a class moves from one cell to the next.
function playhead(app, index, lanes) {
  app.live?.paint(({ module }) => {
    if (index >= module.nodeCount() || !module.seqKind(index)) return;
    lanes.forEach((cells, lane) => paintPlayhead(cells, module.seqPosition(index, lane)));
  });
}

const stepCell = (on, beyond, title, id, onclick) => el('button', {
  class: classes('cell', on && 'on', beyond && 'beyond'), title, onclick,
}, '');

// Steps across, click to toggle. The pattern lives in four bytes as one bit
// per step, so a click is one parameter write.
export function StepGrid(app, index) {
  const node = app.state.patch.nodes[index];
  const length = node.params[0] || 8;
  const cells = [];
  for (let step = 0; step < P.MAX_SEQUENCE_LEN; step++) {
    const byte = 3 + (step >> 3);
    const bit = 1 << (step & 7);
    cells.push(stepCell((node.params[byte] & bit) !== 0, step >= length, `step ${step + 1}`, null,
                        () => app.editor.setParam(index, byte, node.params[byte] ^ bit)));
  }
  playhead(app, index, [cells]);
  return Grid(app, index, `steps (${length})`, Lane('', cells));
}

// Lanes down, steps across, with each lane's own length visible: lanes can
// differ, and that is the polyrhythm the node exists for. Every lane in one
// box, so on a wide screen they stay in step with each other.
export function DrumGrid(app, index, isMidi) {
  const node = app.state.patch.nodes[index];
  const d = app.device.byId.get(node.algorithmId);
  const LANE_BASE = 16, LANE_STRIDE = 8;
  const VELOCITY_BASE = LANE_BASE + P.DRUM_SEQ_LANES * LANE_STRIDE;
  const headerLength = node.params[0] || 16;
  const lanes = [];
  const rows = [];

  for (let lane = 0; lane < P.DRUM_SEQ_LANES; lane++) {
    const laneAt = LANE_BASE + lane * LANE_STRIDE;
    const length = node.params[laneAt + (isMidi ? 2 : 4)] || headerLength;
    const cells = [];
    for (let step = 0; step < P.MAX_SEQUENCE_LEN; step++) {
      // The MIDI variant keeps a velocity per step; the gate variant a bit.
      const at = isMidi ? VELOCITY_BASE + lane * P.MAX_SEQUENCE_LEN + step : laneAt + (step >> 3);
      const bit = 1 << (step & 7);
      const on = isMidi ? node.params[at] !== 0 : (node.params[at] & bit) !== 0;
      cells.push(stepCell(on, step >= length, `lane ${lane + 1}, step ${step + 1}`, null,
                          () => app.editor.setParam(index, at, isMidi ? (on ? 0 : 100) : node.params[at] ^ bit)));
    }
    // The gate variant has one outlet per lane, so a lane's row says which
    // outlet it is - which is the whole reason the outlets are named.
    const outlet = !isMidi && lane < d.nOut ? outletName(d, lane) : `lane ${lane + 1}`;
    lanes.push(cells);
    rows.push(Lane(`${outlet} (${length})`, cells));
  }
  playhead(app, index, lanes);
  return Grid(app, index, 'pattern', rows);
}

// A lane over **scale degrees**, because that is what the sequencer stores.
// The pitch each degree resolves to against the current root and scale is
// shown alongside, and changing the root moves the pitches without touching
// the stored pattern.
export function NoteLane(app, index, isPoly) {
  const node = app.state.patch.nodes[index];
  const STEP_BASE = 16;
  const voices = isPoly ? P.NOTE_SEQ_VOICES : 1;
  const stride = voices * 2 + 2;
  const length = node.params[0] || 8;
  const root = node.params[5] || 60;
  const mask = node.params[3] | ((node.params[4] & 0x0f) << 8);

  const rows = [];
  const lanes = [];
  for (let voice = 0; voice < voices; voice++) {
    const cells = [];
    for (let step = 0; step < P.MAX_SEQUENCE_LEN; step++) {
      const at = STEP_BASE + step * stride + voice * 2;
      const degree = node.params[at] > 127 ? node.params[at] - 256 : node.params[at];
      const velocity = node.params[at + 1];
      const flags = node.params[STEP_BASE + step * stride + voices * 2];
      const pitch = velocity ? root + degreeToSemitone(degree, mask) : null;
      cells.push(el('div', {
        class: classes('note-cell', velocity && 'on', step >= length && 'beyond',
                       (flags & 0x20) && 'rest', (flags & 0x40) && 'tie'),
        title: pitch === null ? `step ${step + 1}: silent` : `step ${step + 1}: degree ${degree} → ${noteName(pitch)}`,
      },
        el('input', {
          type: 'number', value: String(degree), min: '-64', max: '63', inputmode: 'numeric',
          'aria-label': `step ${step + 1} degree`,
          // A step given a degree sounds: a velocity of zero is a silent step.
          onchange: (e) => app.editor.setParams(index, [
            [at, Number(e.target.value) & 0xff], [at + 1, node.params[at + 1] || 100],
          ]),
        }),
        el('span', { class: 'pitch' }, pitch === null ? '·' : noteName(pitch))));
    }
    // The voices step together, so every row carries the one playhead.
    lanes.push(cells);
    rows.push(Lane(isPoly ? `voice ${voice + 1}` : 'notes', cells, 'notes'));
  }
  playhead(app, index, lanes);
  return Grid(app, index, `degrees · root ${noteName(root)}`, rows);
}
