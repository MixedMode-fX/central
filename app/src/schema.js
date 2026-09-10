// The patch format, described by the firmware that reads it.
//
// The editor can build any patch this module runs, because everything it knows
// about the machine it read from the machine: the algorithms, their inlets and
// outlets and domains, every parameter's range and meaning, and how many of
// each thing there is. Anyone who wants a patch *written for them* - by a
// language model, or by a script - needs exactly that same knowledge, and has
// no way to get it: the JSON in the library tab shows the shape of a patch and
// none of the rules it has to obey.
//
// So this turns what the device reported into a **JSON Schema**, and the
// schema tab hands it over inside a prompt. Nothing about any algorithm is
// written here. A schema copied out of this page describes the module in front
// of you - including one running firmware newer than this app, whose new
// algorithm arrives with its parameters described and lands in the schema
// with no change to this file.
//
// **What it describes is the JSON dialect of `patchjson.js`**, the one the
// library tab exports and imports: algorithms by name, jacks numbered from 1
// as on the panel, buses per domain, sequencer patterns in words. That is the
// whole point of using it rather than inventing a format for the occasion - an
// answer that validates is an answer the editor can already load, and loading
// it puts it through `fromPatchJson` and then the firmware's own validator,
// which is what decides whether a patch is real.
//
// Two halves, deliberately apart, the way `picker.js` is split:
//
//   * `patchSchema()` and `promptText()` are arithmetic on the device's own
//     descriptors. No DOM, so `app/test/app.test.mjs` runs them against the
//     real firmware: it validates every example patch against the generated
//     schema and rejects a patch the firmware would reject, which is what
//     stops the schema drifting into a description of a machine this is not.
//   * `schemaTab()` is the page: a prompt to copy, a schema to download, and
//     a box to paste the answer back into.

import * as P from './protocol.js';
import { el } from './views.js';
import { domainName } from './validate.js';
import { SEQ_FAMILY } from './patchjson.js';
import { EXAMPLES } from './examples.js';
import {
  MIDI_PORTS, SCALES, PITCH_CLASSES, STEP_DIRECTIONS, METRONOME_DIVISIONS, METRONOME_FEELS,
  CLOCK_SOURCES, SWAP_TIMINGS, CC_TARGET_KINDS, CLOCK_TARGETS, TRANSPORT_TARGETS,
  TAKEOVER, RELATIVE, FOURTEEN_BIT, PASS_THROUGH,
} from './names.js';

// A backstop on how many of an algorithm's parameters are described one by
// one. What normally stops the list long before this is the firmware's own
// grouping - see `detailed()` - and no algorithm this module has comes near
// it; it is here so that an algorithm from newer firmware with a flat run of
// four hundred parameters cannot turn the schema into a phone book.
const DETAILED_PARAMS = 48;

// The example the prompt carries. A worked example is worth more than any
// amount of prose about the format, and this one is a sequencer: degrees, a
// rest, an accent, a tie, a scale by name and a jack patched to hear it.
export const WORKED_EXAMPLE = 'Note sequencer';

// --- the schema -------------------------------------------------------------

const DOMAIN_KEY = ['gate', 'note', 'cv'];

const KIND_NOTE = {
  [P.ParamKind.PARAM_BOOL]: '0 is off, anything else is on',
  [P.ParamKind.PARAM_BITFIELD]: 'eight steps of a pattern, step n in bit n',
  [P.ParamKind.PARAM_PITCH]: 'a MIDI note number',
  [P.ParamKind.PARAM_PITCH_CLASS]: `a pitch class, 0 = ${PITCH_CLASSES[0]} … 11 = ${PITCH_CLASSES[11]}`,
  [P.ParamKind.PARAM_SIGNED]: 'an int8 kept in the byte: 128..255 read as -128..-1',
  [P.ParamKind.PARAM_MILLIS]: 'milliseconds',
  [P.ParamKind.PARAM_PERCENT]: 'percent',
  [P.ParamKind.PARAM_CHANNEL]: 'a MIDI channel 1..16, or 0 for omni',
};

const defaultText = (pd) => (pd.kind === P.ParamKind.PARAM_ENUM
  ? `${pd.def} (${pd.options?.[pd.def - pd.min] ?? pd.def})`
  : String(pd.def));

// One parameter, as the firmware describes it. **Zero is legal for every
// parameter** - it is how the format says "the default" (`param.h`), which is
// why a parameter whose range starts above zero is a choice between 0 and its
// range rather than a plain interval.
export function paramSchema(pd, over = {}) {
  if (!pd) return { type: 'integer', minimum: 0, maximum: 255, ...over };
  if (pd.min === 0 && pd.max === 0) {
    return { const: 0, title: pd.name, description: 'reserved by this algorithm; leave it 0', ...over };
  }
  const notes = [];
  if (pd.kind === P.ParamKind.PARAM_ENUM && pd.options?.length) {
    notes.push(`one of ${pd.options.map((o, i) => `${pd.min + i} = ${o}`).join(', ')}`);
  } else if (KIND_NOTE[pd.kind]) {
    notes.push(KIND_NOTE[pd.kind]);
  }
  notes.push(`0 means the default, ${defaultText(pd)}`);
  const range = pd.min > 0
    ? { anyOf: [{ const: 0 }, { type: 'integer', minimum: pd.min, maximum: pd.max }] }
    : { type: 'integer', minimum: pd.min, maximum: pd.max };
  return { title: pd.name, description: notes.join('; '), ...range, ...over };
}

const busRef = (domain, nullable) =>
  ({ $ref: `#/$defs/${DOMAIN_KEY[domain] ?? 'gate'}_bus${nullable ? '_or_null' : ''}` });

// A node's inlets or outlets: one entry per connection the algorithm has, in
// the algorithm's own order, each carrying the name the firmware gave it and
// constrained to the buses of that connection's domain. An inlet the algorithm
// requires cannot be null, which is `registry::validate`'s CONFIG_INLET_NOT_
// CONNECTED said in schema.
function portsSchema(d, side) {
  const limit = side === 'in' ? P.MAX_IN : P.MAX_OUT;
  const n = Math.min(side === 'in' ? d.nIn : d.nOut, limit);
  if (!n) return false;
  const domains = side === 'in' ? d.inDomain : d.outDomain;
  const names = side === 'in' ? d.inName : d.outName;
  const required = side === 'in' ? Math.min(d.minIn, n) : 0;
  const prefixItems = [];
  for (let i = 0; i < n; i++) {
    const nullable = i >= required;
    prefixItems.push({
      ...busRef(domains[i], nullable),
      title: names[i] || `${side === 'in' ? 'inlet' : 'outlet'} ${i}`,
      description: `the ${domainName(domains[i])} bus this ${side === 'in' ? 'inlet reads' : 'outlet writes'}`
                 + (nullable ? ', or null' : ' (this one must be connected)'),
    });
  }
  return {
    type: 'array', minItems: required, maxItems: n, prefixItems,
    description: side === 'in'
      ? 'the bus each inlet reads, in the algorithm’s own order; the trailing unconnected ones may be left out'
      : 'the bus each outlet writes, in the algorithm’s own order; the trailing unconnected ones may be left out',
  };
}

// Where the one-by-one description stops. The firmware describes a repeating
// run - a sequencer's steps, a drum machine's lanes - as one `ParamGroup` with
// a repeat count rather than as three hundred descriptors, and that is exactly
// the point at which listing them stops being useful: nobody writes a drum
// pattern as 336 numbers, which is what the `seq` block is for. So the cut
// comes from the firmware's own grouping, the same way `views.js` decides
// which parameters get a control and which get a grid.
function detailed(d) {
  let limit = d.nParams;
  for (const group of d.params ?? []) {
    if (group && group.repeat > 1) limit = Math.min(limit, group.first);
  }
  return Math.min(limit, DETAILED_PARAMS);
}

// The runs left out of the list above, said in words: what they are, how far
// they reach, and what writes them.
function tableNotes(d, shown) {
  const notes = [];
  for (const group of d.params ?? []) {
    if (!group || group.repeat <= 1 || group.first < shown) continue;
    const last = group.first + group.repeat * group.nFields - 1;
    const fields = group.fields.map((f) => f?.name).filter(Boolean).join(', ');
    notes.push(`parameters ${group.first}..${last} are ${group.repeat} repeats of `
             + `${group.nFields} field${group.nFields === 1 ? '' : 's'}`
             + `${fields ? ` (${fields})` : ''} - the step or lane table, which "seq" writes for you`);
  }
  return notes;
}

function paramsSchema(device, d) {
  if (!d.nParams) return false;
  const shown = detailed(d);
  const prefixItems = [];
  for (let i = 0; i < shown; i++) prefixItems.push(paramSchema(device.describeParam(d.id, i)));
  const notes = ['this node’s parameter bytes, in order',
                 'every one of them takes 0 to mean "the default"',
                 'a trailing run of zeros may be left out'];
  notes.push(...tableNotes(d, shown));
  if (SEQ_FAMILY[d.name]) notes.push('"seq" is written after "params", so it wins where the two overlap');
  return {
    type: 'array', maxItems: d.nParams, prefixItems,
    items: { $ref: '#/$defs/param_byte' },
    description: notes.join('. '),
  };
}

// --- the sequencer sugar, per family ----------------------------------------
//
// `patchjson.js` packs a `seq` block into the parameter layout, and which
// layout is `SEQ_FAMILY`'s to say - so these follow that table rather than a
// list of names of their own. Where a field of the block *is* a parameter, its
// schema is that parameter's descriptor: "length" is bounded by whatever the
// firmware said the length parameter is bounded by.

const hitsSchema = (what) => ({
  type: 'string',
  maxLength: P.MAX_SEQUENCE_LEN,
  description: `${what}: one character per step - "x" is velocity 100, "X" 127, "o" 60, "1".."9" a `
             + 'ninth of full velocity each, and anything else (write ".") is a step that does not fire',
});

const directionSchema = {
  type: 'string', enum: [...STEP_DIRECTIONS],
  description: 'the order the steps are played in',
};

function noteSeqSchema(d, at) {
  const voices = d.name === 'PolySequencer' ? P.NOTE_SEQ_VOICES : 1;
  const degree = { type: 'integer', minimum: 0, maximum: 127,
                   description: 'a degree of the scale, counted from the root - not a MIDI note' };
  const velocity = { type: 'integer', minimum: 0, maximum: 127 };
  const many = (one, what) => (voices > 1
    ? { oneOf: [one, { type: 'array', maxItems: voices, items: one,
                       description: `up to ${voices} ${what}, one per voice` }] }
    : one);
  const step = {
    oneOf: [
      { ...degree, description: 'a degree, played at the default velocity' },
      { type: 'string', enum: ['-', '.', 'rest', '=', 'tie'],
        description: '"-" (or ".") is a rest, "=" holds the step before it' },
      { type: 'object', additionalProperties: false,
        properties: {
          deg: many(degree, 'degrees'),
          vel: many(velocity, 'velocities'),
          len: { type: 'integer', minimum: 1, maximum: 31, description: 'how many steps the note lasts' },
          rest: { type: 'boolean' },
          tie: { type: 'boolean' },
          accent: { type: 'boolean' },
          prob: { type: 'integer', minimum: 0, maximum: 100,
                  description: 'percent chance the step sounds; 0 is always' },
        } },
    ],
  };
  return {
    type: 'object', additionalProperties: false,
    description: 'the pattern in words, packed into this node’s parameters when the patch is loaded',
    properties: {
      length: paramSchema(at(0), { title: 'length', description: 'steps in the pattern; 0 takes it from "steps"' }),
      direction: directionSchema,
      gate: paramSchema(at(2), { title: 'gate' }),
      scale: { type: 'string', enum: SCALES.map((s) => s.label),
               description: 'the key this sequencer’s degrees are read in; "global" follows the module’s own' },
      root: paramSchema(at(5), { title: 'root' }),
      velScale: paramSchema(at(6), { title: 'velocity scale' }),
      velOffset: paramSchema(at(7), { title: 'velocity offset' }),
      channel: paramSchema(at(8), { title: 'channel' }),
      accent: paramSchema(at(9), { title: 'accent' }),
      stall: paramSchema(at(10), { title: 'stall' }),
      steps: { type: 'array', maxItems: P.MAX_SEQUENCE_LEN, items: step },
    },
  };
}

function drumSeqSchema(d, at) {
  const midi = d.name === 'DrumSeqMidi';
  // The lane block is a repeating parameter group, so its fields are read from
  // the descriptors at the first lane's own indices.
  const first = 16;
  const lane = {
    type: 'object', additionalProperties: false,
    properties: {
      hits: hitsSchema('the lane'),
      ...(midi
        ? { note: paramSchema(at(first), { title: 'note' }),
            channel: paramSchema(at(first + 1), { title: 'channel' }),
            length: paramSchema(at(first + 2), { title: 'lane length' }),
            prob: paramSchema(at(first + 3), { title: 'probability' }) }
        : { length: paramSchema(at(first + 4), { title: 'lane length' }),
            prob: paramSchema(at(first + 5), { title: 'probability' }) }),
    },
  };
  return {
    type: 'object', additionalProperties: false,
    description: 'the pattern in words, packed into this node’s parameters when the patch is loaded',
    properties: {
      length: paramSchema(at(0), { title: 'length' }),
      direction: directionSchema,
      gate: paramSchema(at(2), { title: 'gate' }),
      lanes: {
        type: 'array', maxItems: P.DRUM_SEQ_LANES,
        description: `up to ${P.DRUM_SEQ_LANES} lanes, in the order of this algorithm’s outlets`,
        items: { oneOf: [hitsSchema('the lane'), lane] },
      },
    },
  };
}

const metronomeSeqSchema = (d, at) => ({
  type: 'object', additionalProperties: false,
  description: 'the clock said the way a musician says it, rather than as a count of PPQN ticks',
  properties: {
    division: { type: 'string', enum: [...METRONOME_DIVISIONS], description: 'the note value it pulses at' },
    feel: { type: 'string', enum: [...METRONOME_FEELS], description: 'straight, dotted (×3/2) or triplet (×2/3)' },
    width: paramSchema(at(2), { title: 'width' }),
  },
});

function gateSeqSchema(d, at) {
  const own = {};
  if (d.name === 'StepSequencer') own.hits = hitsSchema('the pattern');
  if (d.name === 'EuclidianSequencer') {
    own.pulses = paramSchema(at(3), { title: 'pulses' });
    own.rotation = paramSchema(at(4), { title: 'rotation' });
  }
  if (d.name === 'RandomSequencer') {
    own.density = paramSchema(at(3), { title: 'density' });
    own.seed = paramSchema(at(4), { title: 'seed' });
  }
  return {
    type: 'object', additionalProperties: false,
    description: 'the pattern in words, packed into this node’s parameters when the patch is loaded',
    properties: {
      length: paramSchema(at(0), { title: 'length' }),
      direction: directionSchema,
      width: paramSchema(at(2), { title: 'width' }),
      ...own,
      prob: { type: 'array', maxItems: P.MAX_SEQUENCE_LEN, items: paramSchema(at(8)),
              description: 'percent chance each step fires, one per step; 0 is always' },
    },
  };
}

const SEQ_SCHEMA = {
  note: noteSeqSchema,
  drum: drumSeqSchema,
  metronome: metronomeSeqSchema,
  gate: gateSeqSchema,
};

// One algorithm, as a branch of the node schema: what its connections mean,
// what its parameters are, and whether it takes a `seq` block.
function algorithmBranch(device, d) {
  const family = SEQ_FAMILY[d.name];
  const at = (i) => device.describeParam(d.id, i);
  return {
    if: { properties: { algo: { const: d.name } }, required: ['algo'] },
    then: {
      title: d.name,
      description: [d.summary, `${d.nIn} in, ${d.nOut} out`, d.wantsTick ? 'runs off the master clock' : null]
        .filter(Boolean).join(' · '),
      properties: {
        in: portsSchema(d, 'in'),
        out: portsSchema(d, 'out'),
        params: paramsSchema(device, d),
        seq: family ? SEQ_SCHEMA[family](d, at) : false,
      },
    },
  };
}

const portNamesFor = () => [...MIDI_PORTS.map((p) => p.label), 'ALL'];

const valuesOf = (list) => list.map((e) => e.value);
const namedValues = (list) => list.map((e) => `${e.value} = ${e.label}`).join(', ');

function globalsSchema() {
  return {
    type: 'object', additionalProperties: false,
    description: 'the settings that belong to the module rather than to a node. Everything here is optional; '
               + 'what a patch leaves out keeps its default.',
    properties: {
      bpm: { type: 'integer', minimum: P.CLOCK_MIN_BPM, maximum: P.CLOCK_MAX_BPM,
             description: 'the internal tempo' },
      clockSource: { type: 'integer', enum: valuesOf(CLOCK_SOURCES),
                     description: namedValues(CLOCK_SOURCES) },
      cvPpqn: { type: 'integer', minimum: 1, maximum: 96,
                description: 'pulses per quarter note expected on the sync jack, when the clock source is CV' },
      scale: { type: 'string', enum: SCALES.filter((s) => s.label !== 'global').map((s) => s.label),
               description: 'the key every algorithm follows unless it names one of its own' },
      root: { type: 'integer', minimum: 0, maximum: 11,
              description: `the key’s root: 0 = ${PITCH_CLASSES[0]} … 11 = ${PITCH_CLASSES[11]}` },
      pcEnabled: { type: 'integer', minimum: 0, maximum: 1,
                   description: 'recall a preset slot on Program Change' },
      pcChannel: { $ref: '#/$defs/channel' },
      pcSourceMask: { $ref: '#/$defs/port_mask' },
      pcQuantise: { type: 'integer', enum: valuesOf(SWAP_TIMINGS),
                    description: `where a recall lands: ${namedValues(SWAP_TIMINGS)}` },
      nrpnEnabled: { type: 'integer', minimum: 0, maximum: 1,
                     description: 'accept NRPN parameter edits' },
      nrpnChannel: { $ref: '#/$defs/channel' },
      nrpnSourceMask: { $ref: '#/$defs/port_mask' },
    },
  };
}

const targetProperties = (caps) => ({
  targetKind: { type: 'integer', enum: valuesOf(CC_TARGET_KINDS),
                description: namedValues(CC_TARGET_KINDS) },
  targetIndex: { type: 'integer', minimum: 0, maximum: caps.nodes - 1,
                 description: 'which node in "nodes", counted from 0, when the target kind is a node parameter' },
  param: { type: 'integer', minimum: 0, maximum: caps.nParams - 1,
           description: 'the parameter index on that node; or, for the clock, '
                      + `${namedValues(CLOCK_TARGETS)}; for the transport, ${namedValues(TRANSPORT_TARGETS)}` },
  min: { $ref: '#/$defs/param_byte' },
  max: { $ref: '#/$defs/param_byte' },
});

const flagText = (group) => group.options.map((o) => `${o.value} ${o.label}`).join(', ');

function ccMapSchema(caps) {
  return {
    type: 'array', maxItems: caps.ccMappings,
    description: `up to ${caps.ccMappings} controller bindings: a CC on a MIDI port moving a parameter`,
    items: {
      type: 'object', additionalProperties: false,
      required: ['slot', 'sources', 'cc'],
      properties: {
        slot: { type: 'integer', minimum: 0, maximum: caps.ccMappings - 1,
                description: 'which binding slot this is; two bindings may not share one' },
        sources: { $ref: '#/$defs/midi_ports' },
        channel: { $ref: '#/$defs/channel' },
        cc: { type: 'integer', minimum: 0, maximum: 119,
              description: 'the controller number; 120..127 are channel mode messages and are refused' },
        ...targetProperties(caps),
        flags: { type: 'integer', minimum: 0, maximum: 255,
                 description: `a bitmask: takeover (${flagText(TAKEOVER)}) under ${TAKEOVER.mask}, `
                            + `how the knob sends (${flagText(RELATIVE)}) under ${RELATIVE.mask}, `
                            + `${FOURTEEN_BIT} for a 14-bit pair, ${PASS_THROUGH} to pass the CC on` },
      },
    },
  };
}

function modMapSchema(caps) {
  if (!caps.modRoutes) return false;
  return {
    type: 'array', maxItems: caps.modRoutes,
    description: `up to ${caps.modRoutes} modulation routes: a CV bus moving a parameter, at gate rate`,
    items: {
      type: 'object', additionalProperties: false,
      required: ['slot', 'bus'],
      properties: {
        slot: { type: 'integer', minimum: 0, maximum: caps.modRoutes - 1,
                description: 'which route slot this is; two routes may not share one' },
        bus: { $ref: '#/$defs/cv_bus' },
        ...targetProperties(caps),
        depth: { type: 'integer', minimum: 0, maximum: 255,
                 description: 'how much of the range the modulator covers; 255 is all of it' },
        flags: { type: 'integer', minimum: 0, maximum: 255,
                 description: `a bitmask: ${P.ModFlags.MOD_MODE_MASK} adds to the parameter instead of `
                            + `setting it, ${P.ModFlags.MOD_BIPOLAR} reads the bus as bipolar, `
                            + `${P.ModFlags.MOD_INVERT} inverts it` },
        target: { type: 'string',
                  description: 'the parameter’s name, for a reader. It is not resolved back: "param" is what '
                             + 'the module is told' },
      },
    },
  };
}

// The whole patch, as this module would have it.
export function patchSchema(device) {
  const caps = device?.capabilities;
  if (!caps) throw new Error('the schema is read from a module, and none is attached');
  const algorithms = (device.algorithms ?? []).filter(Boolean);
  if (!algorithms.length) throw new Error('the module reported no algorithms');

  const $defs = {
    param_byte: { type: 'integer', minimum: 0, maximum: 255,
                  description: 'a parameter byte; 0 means "the default"' },
    channel: { type: 'integer', minimum: 0, maximum: 16,
               description: 'a MIDI channel 1..16, or 0 for omni' },
    midi_ports: { type: 'array', minItems: 1, items: { type: 'string', enum: portNamesFor() },
                  description: 'MIDI endpoints by the name on the panel; "ALL" is every one of them. '
                             + 'The editor also reads them in any case or spacing, and by the firmware’s '
                             + 'own names, but these are the ones to write.' },
    port_mask: { type: 'integer', minimum: 0, maximum: 255,
                 description: `a bitmask of MIDI ports: ${MIDI_PORTS.map((p) => `${p.value} ${p.label}`).join(', ')}` },
  };
  DOMAIN_KEY.forEach((key, domain) => {
    const n = [caps.gateBuses, caps.noteBuses, caps.cvBuses][domain];
    $defs[`${key}_bus`] = { type: 'integer', minimum: 0, maximum: n - 1,
                            description: `one of this module’s ${n} ${key} buses` };
    $defs[`${key}_bus_or_null`] = { type: ['integer', 'null'], minimum: 0, maximum: n - 1,
                                    description: `a ${key} bus, or null for "not connected"` };
  });

  return {
    $schema: 'https://json-schema.org/draft/2020-12/schema',
    $id: 'https://mixedmode-fx.github.io/central/patch.schema.json',
    title: 'MixedMode Modular Central patch',
    description: [
      'A patch for the MMMC, a Eurorack CV & MIDI processor, in the JSON form its editor reads and writes.',
      `Generated from the attached module: protocol version ${P.SYSEX_PROTOCOL_VERSION}, patch format `
      + `${P.PATCH_FORMAT_VERSION}, ${algorithms.length} algorithms.`,
      'Nodes do not connect to each other: each reads and writes numbered buses, and a bus is where a '
      + 'writer and a reader meet. There are three kinds - gate (a trigger or a level), note (MIDI notes) '
      + 'and CV (a value) - and a connection is only ever a bus index of the right kind. Buses are '
      + 'double-buffered, so the order of "nodes" does not matter and feedback costs one pass rather than '
      + 'hanging.',
      `The module has ${caps.jacks} jacks, each either an input that drives a gate bus or an output driven `
      + `by one, ${caps.midiIn} MIDI input ports and ${caps.midiOut} MIDI output ports that read and write `
      + `note buses, and room for ${caps.nodes} nodes.`,
      `The master clock runs at ${caps.ppqn} PPQN. No algorithm divides it for itself: a Metronome or a `
      + 'ClockDiv writes a gate bus, and every sequencer advances on a rising edge at an inlet - so one '
      + 'divider drives as many sequencers as you like, in lock.',
    ].join(' '),
    type: 'object',
    additionalProperties: false,
    properties: {
      globals: globalsSchema(),
      gate_ports: {
        type: 'array', maxItems: caps.jacks,
        description: 'the jacks this patch uses. A jack left out is unused.',
        items: {
          type: 'object', additionalProperties: false, required: ['port', 'dir'],
          properties: {
            port: { type: 'integer', minimum: 1, maximum: caps.jacks,
                    description: 'the jack, numbered from 1 as on the panel' },
            dir: { type: 'string', enum: ['in', 'out', 'unused'],
                   description: '"in" drives a gate bus from the jack; "out" drives the jack from a gate bus' },
            bus: { $ref: '#/$defs/gate_bus_or_null' },
          },
        },
      },
      midi_in: {
        type: 'array', maxItems: caps.midiIn,
        description: 'MIDI coming in: each port filters by cable and channel and writes a note bus',
        items: {
          type: 'object', additionalProperties: false, required: ['sources', 'bus'],
          properties: {
            sources: { $ref: '#/$defs/midi_ports' },
            channel: { $ref: '#/$defs/channel' },
            bus: { $ref: '#/$defs/note_bus' },
          },
        },
      },
      midi_out: {
        type: 'array', maxItems: caps.midiOut,
        description: 'MIDI going out: each port reads a note bus and sends it to the cables named',
        items: {
          type: 'object', additionalProperties: false, required: ['targets', 'bus'],
          properties: {
            targets: { $ref: '#/$defs/midi_ports' },
            channel: { type: 'integer', minimum: 0, maximum: 16,
                       description: 'the channel to send on; 0 keeps each note’s own' },
            bus: { $ref: '#/$defs/note_bus' },
          },
        },
      },
      nodes: {
        type: 'array', maxItems: caps.nodes,
        description: 'the algorithms in this patch. Order is free: the buses decide what feeds what.',
        items: {
          type: 'object', additionalProperties: false, required: ['algo'],
          properties: {
            algo: { type: 'string', enum: algorithms.map((d) => d.name),
                    description: 'the algorithm, by the name this firmware knows it by' },
            in: { type: 'array' },
            out: { type: 'array' },
            params: { type: 'array' },
            seq: { type: 'object' },
          },
          allOf: algorithms.map((d) => algorithmBranch(device, d)),
        },
      },
      cc_map: ccMapSchema(caps),
      mod_map: modMapSchema(caps),
    },
    $defs,
  };
}

// **Not indented, and that is deliberate.** This text is read by a machine,
// never by a person - what a person reads about an algorithm is the panel on
// the patch tab - and indenting it costs more than the schema itself: two
// spaces per level and a line per brace more than doubled it, which for a
// schema this size is tens of thousands of tokens of a context window spent on
// nothing. A prompt that does not fit is not a prompt. Anyone who does want it
// laid out has a formatter one keystroke away.
export const schemaText = (device) => `${JSON.stringify(patchSchema(device))}\n`;

// --- the prompt --------------------------------------------------------------
//
// What a person actually needs from this page is not a schema: it is a message
// they can paste somewhere and get a patch back from. So the schema arrives
// inside one, with the rules that are about *this machine* rather than about
// JSON, a worked example, and - if they want it - the patch on screen as the
// thing to change.

export function promptText(device, { patch = null } = {}) {
  const caps = device.capabilities;
  const example = EXAMPLES[WORKED_EXAMPLE] ?? Object.values(EXAMPLES)[0];
  const lines = [
    'You are writing a patch for the MixedMode Modular Central (MMMC), a Eurorack CV and MIDI',
    'processor. Answer with **one JSON document** that validates against the JSON Schema at the end',
    'of this message, and give it to me as a file I can download, named something like `patch.json`.',
    '',
    'Rules for the file:',
    '',
    '- JSON only: no comments, no trailing commas, no markdown fence inside the file.',
    '- Use only algorithm names, parameters and bus numbers the schema allows. It was generated from',
    '  the firmware I am running, so anything outside it does not exist on my module.',
    '- Every connection is a bus index, and a bus only carries one kind of signal. Two nodes are',
    '  connected by naming the same bus: the writer names it in "out", the reader in "in".',
    '- A parameter left at 0 means that parameter\'s default, so leave out what you are not setting.',
    '- Prefer a "seq" block to raw "params" for the algorithms that take one.',
    '',
    'Then, outside the file, tell me in a few lines what the patch does, which jacks to plug in and',
    'what to expect to hear.',
    '',
    `This module: ${caps.jacks} jacks, ${caps.gateBuses} gate buses, ${caps.noteBuses} note buses, `
    + `${caps.cvBuses} CV buses, ${caps.nodes} nodes, ${caps.midiIn} MIDI in ports, `
    + `${caps.midiOut} MIDI out ports, master clock ${caps.ppqn} PPQN.`,
    '',
    `A worked example - "${WORKED_EXAMPLE}": ${example.about}`,
    '',
    '```json',
    JSON.stringify(example.patch, null, 2),
    '```',
    '',
  ];
  if (patch) {
    lines.push('The patch I have now, which is the one to change:', '', '```json', patch.trim(), '```', '');
  }
  lines.push('The schema:', '', '```json', schemaText(device).trim(), '```', '');
  return lines.join('\n');
}

// --- the page ----------------------------------------------------------------

async function copy(app, text, said) {
  try {
    await navigator.clipboard.writeText(text);
    app.status = said;
  } catch {
    app.status = 'this browser would not take it - select the text and copy it';
  }
  app.render();
}

const copyButton = (app, label, text, said, klass = '') =>
  el('button', { class: klass, onclick: () => copy(app, text, said) }, label);

export function schemaTab(app) {
  if (!app.device?.capabilities || !app.device.algorithms?.filter(Boolean).length) {
    return el('section', { class: 'panel' },
      el('h2', {}, 'the patch format'),
      el('p', { class: 'hint' },
        'the schema is read from a module, and none is attached — press “connect a module”, '
        + 'or wait for the built-in one to start'));
  }

  // Built once per render rather than once per button: the schema is read out
  // of the device's descriptors every time it is asked for, and three buttons
  // and two boxes on this page ask for it.
  const promptString = promptText(app.device, { patch: app.schemaWithPatch ? app.patchJson() : null });
  const schemaString = schemaText(app.device);
  const algorithms = app.device.algorithms.filter(Boolean).length;
  const size = (text) => `${Math.round(text.length / 1024)} kB`;

  // `wrap`, because the schema at the end of this is a single very long line
  // and a box that scrolls sideways for a kilometre shows nothing at all.
  const promptBox = el('textarea', { class: 'json wrap', spellcheck: 'false', rows: '14',
                                     'aria-label': 'the prompt to copy' }, promptString);
  const withPatch = el('input', { type: 'checkbox', class: 'switch',
                                  onchange: (e) => { app.schemaWithPatch = e.target.checked; app.render(); } });
  withPatch.checked = Boolean(app.schemaWithPatch);

  const schemaBox = el('textarea', { class: 'json wrap', spellcheck: 'false', rows: '12',
                                     'aria-label': 'the schema' }, schemaString);
  const schemaDetails = el('details', {},
    el('summary', {}, `the schema on its own (${algorithms} algorithms, ${size(schemaString)})`),
    schemaBox,
    el('div', { class: 'row' },
      copyButton(app, 'copy', schemaString, 'the schema is on the clipboard'),
      el('button', { onclick: () => {
        app.download('mmmc-patch.schema.json', schemaString, 'application/json');
        app.status = 'exported the schema';
        app.render();
      } }, 'download .json')));
  schemaDetails.open = app.isOpen('schema');
  schemaDetails.addEventListener('toggle', () => app.setOpen('schema', schemaDetails.open));

  const answer = el('textarea', { class: 'json', spellcheck: 'false', rows: '8',
                                  'aria-label': 'the JSON that came back' });

  return el('div', {},
    el('section', { class: 'panel' },
      el('h2', {}, 'ask for a patch'),
      el('p', { class: 'hint' },
        'the module has been asked what it can do, and the answer is below as a JSON Schema inside a '
        + `prompt — ${size(promptString)} of it, because it describes every one of this firmware’s `
        + `${algorithms} algorithms. Copy it, paste it wherever you ask, and bring the JSON back to `
        + 'the box at the bottom of this page.'),
      el('div', { class: 'row' },
        copyButton(app, 'copy the prompt', promptString, 'the prompt is on the clipboard', 'primary'),
        el('label', { class: 'bool' }, withPatch,
          el('span', {}, 'include the patch I have open'))),
      promptBox),
    el('section', { class: 'panel' },
      el('h2', {}, 'the schema'),
      el('p', { class: 'hint' },
        'read from the module itself: every algorithm this firmware has, what each inlet, outlet and '
        + 'parameter means, and how many buses, jacks and nodes there are. A patch that validates '
        + 'against it is one this editor can load.'),
      schemaDetails),
    el('section', { class: 'panel' },
      el('h2', {}, 'paste the answer'),
      el('p', { class: 'hint' },
        'the same door the library tab’s JSON box is: it is read, built, and sent to the module, which '
        + 'accepts it or says why not.'),
      answer,
      el('div', { class: 'row' },
        el('button', { class: 'primary', onclick: () => {
          app.loadJson(answer.value, 'the JSON above');
          if (!app.pendingError) app.tab = app.editingTab = 'patch';
          app.render();
        } }, 'load it'))));
}
