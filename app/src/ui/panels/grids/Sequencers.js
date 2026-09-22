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
import { noteName, degreeToSemitone, tonicOf, DEFAULT_KEY_OCTAVE } from '../../../core/music.js';
import { scaleMaskById } from '../../../protocol/names.js';
import { keyNow, globalNow } from '../../../core/globals.js';
import { rootedNote } from '../../components/KeyBadge.js';
import { NO_STEP, paintPlayhead } from './playhead.js';
import '../Grid.css';

// The box a grid lives in: a title, and the lanes in a remembered scroller.
// The title is words, or an element a grid repaints.
function Grid(app, index, title, lanes) {
  return el('div', { class: 'grid' },
    typeof title === 'string' ? el('div', { class: 'grid-title' }, title) : title,
    Scroller(app, `grid-${index}`, el('div', { class: 'lanes' }, lanes)));
}

const Lane = (name, cells, klass = '') => el('div', { class: 'lane-row' },
  el('span', { class: 'lane-name' }, name),
  el('div', { class: classes('lane', klass) }, cells));

// The step each lane is sounding, painted onto its cells every frame from
// where the module says the node is (runtime/monitor.js). Nothing is
// rebuilt: a class moves from one cell to the next.
function playhead(app, index, lanes) {
  app.live?.watchNode(index);
  app.live?.paint(({ monitor }) => {
    const at = monitor.positionsOf(index);
    lanes.forEach((cells, lane) => paintPlayhead(cells, at[lane] ?? NO_STEP));
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
// The pitch each degree resolves to is shown alongside, worked out the way
// the firmware works it out (note_sequencer.h): from the key the module is
// playing in - the live one, not the stored one (core/globals.js) - in the
// register the sequencer's `octave` names, unless a cable on its root inlet
// has said otherwise. Changing the key moves the pitches without touching
// the stored pattern. The key moves while the module runs, so the pitch
// labels are repainted per frame into cells that already exist.
//
// A degree is committed on Enter, and on leaving a field that was typed in,
// as well as on change: a silent step reads "0" already, so typing 0 into it
// changes nothing, no change event ever comes, and the root would be the one
// degree that could not be entered. A value is written once, whichever of
// those says it first.
//
// The pitch under the degree is the step's switch: a step is silent when its
// velocity is zero (note_sequencer.h), so tapping the pitch mutes the step
// and tapping the dot sounds it again, and the degree stays where it was.
export function NoteLane(app, index, isPoly) {
  const node = app.state.patch.nodes[index];
  const STEP_BASE = 16;
  const voices = isPoly ? P.NOTE_SEQ_VOICES : 1;
  const stride = voices * 2 + 2;
  const length = node.params[0] || 8;
  const octave = node.params[3];

  const anchor = () => {
    const key = keyNow(app);
    const rooted = rootedNote(app, index);
    const root = rooted.note ?? tonicOf(octave, globalNow(app, 'rootOctave').value || DEFAULT_KEY_OCTAVE, key.root);
    return { root, mask: scaleMaskById(key.scale), inlet: rooted.inlet };
  };
  const pitchOf = (at, { root, mask }) => {
    const degree = node.params[at] > 127 ? node.params[at] - 256 : node.params[at];
    if (!node.params[at + 1]) return { degree, pitch: null };
    const pitch = root + degreeToSemitone(degree, mask);
    return { degree, pitch: pitch < 0 || pitch > 127 ? null : pitch };
  };
  const cellTitle = (step, { degree, pitch }) =>
    (pitch === null ? `step ${step + 1}: silent` : `step ${step + 1}: degree ${degree} → ${noteName(pitch)}`);
  const switchTitle = (step, on) => `step ${step + 1}: ${on ? 'sounding, tap to silence it' : 'silent, tap to sound it'}`;
  const anchorTitle = ({ root, inlet }) =>
    `degrees · root ${noteName(root)}${inlet === null ? '' : ` (on the ${inlet} inlet)`}`;

  const first = anchor();
  const title = el('div', { class: 'grid-title' }, anchorTitle(first));
  const rows = [];
  const lanes = [];
  const painted = [];
  for (let voice = 0; voice < voices; voice++) {
    const cells = [];
    for (let step = 0; step < P.MAX_SEQUENCE_LEN; step++) {
      const at = STEP_BASE + step * stride + voice * 2;
      const { degree, pitch } = pitchOf(at, first);
      const velocity = node.params[at + 1];
      const flags = node.params[STEP_BASE + step * stride + voices * 2];
      // A step given a degree sounds: a velocity of zero is a silent step.
      let committed = null;
      let typed = false;
      const commit = (value) => {
        typed = false;
        if (value === committed) return;
        committed = value;
        app.editor.setParams(index, [[at, Number(value) & 0xff], [at + 1, node.params[at + 1] || 100]]);
      };
      const label = el('button', {
        type: 'button', class: 'pitch', 'aria-pressed': velocity ? 'true' : 'false',
        'aria-label': switchTitle(step, velocity),
        onclick: () => app.editor.setParam(index, at + 1, velocity ? 0 : 100),
      }, pitch === null ? '·' : noteName(pitch));
      const cell = el('div', {
        class: classes('note-cell', velocity && 'on', step >= length && 'beyond',
                       (flags & 0x20) && 'rest', (flags & 0x40) && 'tie'),
        title: cellTitle(step, { degree, pitch }),
      },
        el('input', {
          type: 'number', value: String(degree), min: '-64', max: '63', inputmode: 'numeric',
          'aria-label': `step ${step + 1} degree`,
          onchange: (e) => commit(e.target.value),
          onkeydown: (e) => { if (e.key === 'Enter') commit(e.target.value); },
          oninput: () => { typed = true; },
          onblur: (e) => { if (typed) commit(e.target.value); },
        }),
        label);
      cells.push(cell);
      painted.push({ at, step, cell, label });
    }
    // The voices step together, so every row carries the one playhead.
    lanes.push(cells);
    rows.push(Lane(isPoly ? `voice ${voice + 1}` : 'notes', cells, 'notes'));
  }
  playhead(app, index, lanes);

  let drawn = `${first.root}:${first.mask}:${first.inlet}`;
  app.live?.paint(() => {
    const now = anchor();
    const key = `${now.root}:${now.mask}:${now.inlet}`;
    if (key === drawn) return;
    drawn = key;
    title.textContent = anchorTitle(now);
    for (const { at, step, cell, label } of painted) {
      const resolved = pitchOf(at, now);
      label.textContent = resolved.pitch === null ? '·' : noteName(resolved.pitch);
      cell.setAttribute('title', cellTitle(step, resolved));
    }
  });
  return Grid(app, index, title, rows);
}
