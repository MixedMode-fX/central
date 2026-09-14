// What can be added to a patch, as the list the picker shows: every algorithm
// the module reports, shelved by the category it reports with it, plus the
// patch's edges. Arithmetic on the device's descriptors; no DOM.

import * as P from '../protocol/generated.js';
import { ALGORITHM_CATEGORIES } from '../protocol/names.js';
import { BlockKind } from './graph.js';

// What can be put on the canvas besides an algorithm: the two things a patch
// has that are not one. A direction is not a kind of block - a jack is a
// jack, and which way it faces is one of its settings - so what is added is
// the port, pointing the way most patches want it: a jack listens, a MIDI
// port plays.
export const ENDPOINTS = [
  { key: 'jack', label: 'jack', kind: BlockKind.Jack,
    direction: P.GatePortDirection.GATE_PORT_IN, note: 'gate',
    hint: 'One of the module’s gate jacks. In or out is a toggle on the jack itself.' },
  { key: 'midi', label: 'MIDI port', kind: BlockKind.MidiIn, note: 'notes',
    hint: 'A cable’s worth of notes, on a note bus. In or out is a toggle on the port itself.' },
];

// Every algorithm the module reports, shelved by the category it reports with
// it, plus whatever else the caller can add (the patch's edges). An algorithm
// whose category this app does not know - a module running newer firmware -
// lands under "other" rather than disappearing, which is the reason the
// category is appended to the registry record rather than spliced into it.
//
// `inPatch` is the algorithm ids the patch already holds. An algorithm the
// module says there may only be one of - the Key node, because the key has one
// value - is left off the list once it is in the patch, rather than offered
// and then refused by the validator on the way out.
export function catalogue(algorithms, endpoints = [], inPatch = []) {
  const shelves = new Map(ALGORITHM_CATEGORIES.map((c) => [c.value, []]));
  const other = shelves.get(P.AlgorithmCategory.CATEGORY_NONE);
  const held = new Set(inPatch);

  for (const d of algorithms ?? []) {
    if (!d) continue;
    if (d.singleton && held.has(d.id)) continue;
    const shelf = shelves.get(d.category) ?? other;
    shelf.push({
      value: String(d.id),
      label: d.name,
      note: `${d.nIn} in · ${d.nOut} out${d.wantsTick ? ' · clocked' : ''}`,
      hint: d.summary ?? '',
    });
  }

  const groups = ALGORITHM_CATEGORIES
    .filter((c) => shelves.get(c.value).length)
    .map((c) => ({ key: c.key, label: c.label, options: shelves.get(c.value) }));

  if (endpoints.length) {
    groups.push({
      key: 'edges',
      label: 'edges',
      options: endpoints.map((e) => ({ value: e.key, label: e.label, note: e.note, hint: e.hint })),
    });
  }
  return groups;
}

// The words a search runs against: everything the row shows, plus the shelf it
// is on - so "midi" finds the MIDI algorithms and "seq" finds the sequencers
// whether or not either word is in the name.
const haystack = (option, group) =>
  `${group.label} ${option.label} ${option.note ?? ''} ${option.hint ?? ''}`.toLowerCase();

// Every term has to match somewhere, which is what makes "note seq" narrow to
// one row instead of matching everything with a note in it. Empty groups fall
// away, so a shelf never appears with nothing on it.
export function filterGroups(groups, query) {
  const terms = String(query ?? '').toLowerCase().split(/\s+/).filter(Boolean);
  if (!terms.length) return groups;
  return groups
    .map((group) => ({
      ...group,
      options: group.options.filter((o) => terms.every((t) => haystack(o, group).includes(t))),
    }))
    .filter((group) => group.options.length);
}

export const optionsOf = (groups) => groups.flatMap((group) => group.options);

export const optionFor = (groups, value) =>
  optionsOf(groups).find((o) => o.value === value) ?? null;

