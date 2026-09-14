// Which question a parameter answers, so a card can ask the same few
// questions in the same order on every node.
//
// A descriptor lists its parameters in the order the algorithm stores them,
// which is the order they were written in, and that order says "root, scale,
// velocity, channel, seed" on one node and "velocity, root, seed, scale" on
// the next. Every node is asked the same few questions - which notes, when,
// how loud, how likely, in what manner - so the card asks them in that order
// on every node, and a hand that has learned where "root" lives on one card
// finds it in the same place on the rest.
//
// **An algorithm that labels its groups is believed instead.** Sorting by
// what a name sounds like is right often enough to be worth doing and wrong
// exactly where one algorithm has several controls over one mechanism. A
// descriptor that says what its groups are (src/node/param.h) is describing
// its own machine and wins.

import * as P from '../../protocol/generated.js';
import { knobParams } from '../../core/patch.js';

export const PARAM_SECTIONS = [
  { key: 'mode', label: 'behaviour' },
  { key: 'pitch', label: 'pitch' },
  { key: 'time', label: 'timing' },
  { key: 'level', label: 'dynamics' },
  { key: 'chance', label: 'chance' },
  { key: 'midi', label: 'MIDI' },
  { key: 'other', label: 'other' },
];

// The words the firmware's descriptors use, by section. A parameter this
// table has never met lands under "other" rather than being hidden.
const SECTION_WORDS = {
  mode: ['mode', 'direction', 'rule', 'shape', 'priority', 'hold', 'new chord', 'write',
         'link', 'map', 'cycle', 'sync', 'polarity', 'loop', 'retrigger', 'snap', 'fixed', 'quality',
         'voicing', 'inversion', 'diatonic', 'cells'],
  pitch: ['root', 'octave', 'transpose', 'semitone', 'interval', 'degree', 'note',
          'spread', 'range', 'low', 'high', 'fifths', 'leading', 'bass', 'voices', 'tie key', 'rest key',
          'base', 'bend', 'pitch'],
  time: ['length', 'division', 'feel', 'rate', 'gate', 'width', 'delay', 'time', 'phase', 'steps',
         'pulses', 'rotation', 'repeats', 'phrase', 'decay', 'rise', 'fall', 'stall', 'swing', 'tempo',
         'bars', 'beat'],
  level: ['velocity', 'vel ', 'accent', 'curve', 'amount', 'depth', 'offset', 'dry', 'threshold',
          'hysteresis', 'level', 'smooth', 'slew', 'scale'],
  chance: ['probability', 'chance', 'density', 'deviation', 'cadence', 'gravity', 'drift', 'chaos',
           'revive', 'seed', 'bits', 'edges', 'random'],
  midi: ['channel', 'controller', 'mod src', 'mod cc', 'source', 'cc'],
};

// From its kind first - a pitch is a pitch whatever it is called - and then
// from its name.
export function paramSection(pd) {
  const name = String(pd.name ?? '').toLowerCase();
  if (pd.kind === P.ParamKind.PARAM_PITCH || pd.kind === P.ParamKind.PARAM_PITCH_CLASS) return 'pitch';
  if (pd.kind === P.ParamKind.PARAM_CHANNEL) return 'midi';
  if (pd.kind === P.ParamKind.PARAM_MILLIS) return 'time';
  for (const section of PARAM_SECTIONS) {
    const words = SECTION_WORDS[section.key];
    if (words?.some((word) => name === word || name.startsWith(word) || name.includes(` ${word}`))) {
      return section.key;
    }
  }
  if (pd.kind === P.ParamKind.PARAM_PERCENT) return 'chance';
  return 'other';
}

// The header parameters of a descriptor, sorted onto sections. Groups
// sharing a label are one section, in the order the labels first appear; a
// descriptor that labels only some of its groups keeps the guess for the
// rest, after the ones it named.
export function paramSections(groups) {
  const labelled = new Map();
  const sections = new Map(PARAM_SECTIONS.map((s) => [s.key, { ...s, params: [] }]));
  for (const group of groups ?? []) {
    if (!group || group.repeat > 1) continue;
    const label = group.label || '';
    for (const param of knobParams({ params: [group] })) {
      if (label) {
        if (!labelled.has(label)) labelled.set(label, { key: label, label, params: [] });
        labelled.get(label).params.push(param);
      } else {
        sections.get(paramSection(param.pd)).params.push(param);
      }
    }
  }
  return [...labelled.values(), ...[...sections.values()].filter((x) => x.params.length)];
}
