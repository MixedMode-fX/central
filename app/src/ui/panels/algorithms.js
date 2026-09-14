// What the app knows about particular algorithms, in one table.
//
// Everything else about a node is read from the device: its ports, its
// parameters, their ranges and their names. Two things cannot be, because
// they are about the *shape* of what the parameters mean rather than their
// values, and the descriptor has no way to say them:
//
//   * a **grid**: nobody enters a drum pattern as a list of numbers, so a
//     sequencer gets a purpose-built view of its table, and a harmony gets
//     its circle of fifths;
//   * a control that is **inert** right now, and why. A knob that moves and
//     changes nothing is the most confusing thing a module can offer, and the
//     reason is never in the parameter: `leading` is a real control in a
//     major key and inert in a natural minor one. Only what the firmware
//     actually ignores is listed: a control marked dead that is not is a
//     control nobody touches.
//
// Keyed by the algorithm's name as the firmware reports it. An algorithm not
// in this table gets the generic card, which is right for nearly all of them.

import { scaleMaskById } from '../../protocol/names.js';
import { chromatic, hasLeadingTone } from '../../core/music.js';
import { StepGrid, DrumGrid, NoteLane } from './grids/Sequencers.js';
import { HarmonyCircle } from './grids/HarmonyCircle.js';

// Tonnetz's fourth cycle, and the LFO's clock-locked sync. The descriptors
// number their options from their own minimum and the page reads the names
// from the firmware, so this is the one place a value of either enum is
// written down.
const TONNETZ_FREE = 4;
const LFO_CLOCK = 2;

// An inert rule: `when(values, key)` is given a node's effective parameter
// values by name and the key's scale mask, and says why the control does
// nothing, or nothing at all.
const inert = (param, when) => ({ param, when });

const ALGORITHMS = {
  StepSequencer: { grid: StepGrid },
  DrumSeqGate: { grid: (app, index) => DrumGrid(app, index, false) },
  DrumSeqMidi: { grid: (app, index) => DrumGrid(app, index, true) },
  NoteSequencer: { grid: (app, index) => NoteLane(app, index, false) },
  PolySequencer: { grid: (app, index) => NoteLane(app, index, true) },
  Harmony: {
    grid: HarmonyCircle,
    inert: [
      // Pitch class 11 above the root is the semitone below the tonic an
      // octave up, which is the firmware's own test for having one
      // (src/midi/root_motion.h).
      inert('leading', (_, key) => (hasLeadingTone(key) ? null : 'this key has no leading tone')),
      inert('drift', (v) => (v.loop ? null : 'nothing is looping')),
    ],
  },
  // A modulator with two rates has one of them live at a time, and which one
  // is a different control again - the classic "I turned rate and nothing
  // happened".
  LFO: {
    inert: [
      inert('rate', (v) => (v.sync === LFO_CLOCK ? 'the cycle is locked to the clock' : null)),
      inert('division', (v) => (v.sync === LFO_CLOCK ? null : 'free-running: the rate decides the cycle')),
      inert('feel', (v) => (v.sync === LFO_CLOCK ? null : 'free-running: the rate decides the cycle')),
    ],
  },
  Slew: { inert: [inert('fall', (v) => (v.link ? 'linked: fall follows rise' : null))] },
  Tonnetz: {
    inert: [
      // `free` draws all three transforms uniformly, so there is no cycle's
      // next for a deviation to be a deviation from.
      inert('deviation', (v) => (v.cycle === TONNETZ_FREE ? 'the free walk has no cycle to leave' : null)),
      // The refusal is "is every note of this triad in the key", and a
      // chromatic key holds all twelve.
      inert('diatonic', (_, key) => (chromatic(key) ? 'a chromatic key contains every triad' : null)),
    ],
  },
};

export const algorithmGrid = (name) => ALGORITHMS[name]?.grid ?? null;

// Why a control of node `index` is doing nothing right now, or null.
export function inertReason(app, index, pd) {
  const node = app.state.patch.nodes[index];
  const d = app.device.byId.get(node.algorithmId);
  const rules = ALGORITHMS[d?.name]?.inert?.filter((rule) => rule.param === pd.name) ?? [];
  if (!rules.length) return null;
  const values = effectiveValues(d, node);
  const key = scaleMaskById(app.state.globals?.scale);
  for (const rule of rules) {
    const why = rule.when(values, key);
    if (why) return why;
  }
  return null;
}

// A node's header parameters by name, a stored zero read as the default.
function effectiveValues(descriptor, node) {
  const values = {};
  for (const group of descriptor?.params ?? []) {
    if (!group || group.repeat > 1) continue;
    for (let f = 0; f < group.nFields; f++) {
      const pd = group.fields[f];
      if (!pd) continue;
      const stored = node.params[group.first + f];
      values[pd.name] = stored === 0 ? pd.def : stored;
    }
  }
  return values;
}
