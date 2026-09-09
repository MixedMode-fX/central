#!/usr/bin/env node
// Generates editor/src/protocol.js from the firmware headers.
//
// The message layout is defined once, in C++, and the firmware and the editor
// must agree byte for byte. Two hand-maintained copies drift, and the failure
// mode is a corrupted patch on real hardware - so this reads the headers and
// emits the JavaScript, and `make editor` fails if what is checked in differs
// from what the headers say. A protocol change therefore breaks both builds at
// once, which was the whole reason for keeping the editor in this repository.
//
// It is a small, deliberately dumb parser: it only understands the shapes the
// headers actually use - `#define NAME value` and `enum Name : uint8_t { ... }`
// - and it fails loudly rather than guessing.

import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');
const out = join(here, '..', 'src', 'protocol.js');

const read = (p) => readFileSync(join(root, p), 'utf8');

// Strips comments so a value inside one is never picked up.
function strip(source) {
  return source.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');
}

function defines(source, names) {
  const text = strip(source);
  const found = {};
  for (const name of names) {
    const m = text.match(new RegExp(`^\\s*#define\\s+${name}\\s+(.+)$`, 'm'));
    if (!m) throw new Error(`#define ${name} not found`);
    const raw = m[1].trim().replace(/u$/i, '');
    const value = Number(raw);
    if (!Number.isFinite(value)) throw new Error(`#define ${name} is not a plain number: ${raw}`);
    found[name] = value;
  }
  return found;
}

// Reads an enum body, following C's rule that an entry without `=` is the
// previous one plus one.
function enumeration(source, name) {
  const text = strip(source);
  const m = text.match(new RegExp(`enum\\s+${name}\\s*:\\s*\\w+\\s*\\{([^}]*)\\}`));
  if (!m) throw new Error(`enum ${name} not found`);
  const entries = {};
  let next = 0;
  for (const part of m[1].split(',')) {
    const trimmed = part.trim();
    if (!trimmed) continue;
    const assign = trimmed.match(/^(\w+)\s*=\s*(0x[0-9a-fA-F]+|\d+)$/);
    const bare = trimmed.match(/^(\w+)$/);
    if (assign) {
      next = Number(assign[2]);
      entries[assign[1]] = next;
    } else if (bare) {
      entries[bare[1]] = next;
    } else {
      throw new Error(`enum ${name}: cannot read entry "${trimmed}"`);
    }
    next += 1;
  }
  return entries;
}

const sysexH = read('src/protocol/sysex.h');
const configH = read('src/config.h');
const codecH = read('src/patch/patch_codec.h');
const storeH = read('src/patch/patch_store.h');
const nrpnH = read('src/control/nrpn.h');
const midiH = read('src/hal/midi_types.h');

const constants = {
  ...defines(sysexH, [
    'SYSEX_MANUFACTURER', 'SYSEX_PROTOCOL_VERSION',
    'SYSEX_UNIVERSAL_NON_REALTIME', 'SYSEX_GENERAL_INFORMATION',
    'SYSEX_IDENTITY_REQUEST', 'SYSEX_IDENTITY_REPLY',
    'SYSEX_DEVICE_FAMILY', 'SYSEX_DEVICE_MEMBER',
    'SYSEX_BROADCAST_DEVICE', 'SYSEX_DEFAULT_DEVICE',
    'SYSEX_CHUNK_PAYLOAD', 'SYSEX_CHUNK_FIRST', 'SYSEX_CHUNK_LAST',
    'SYSEX_RX_MAX', 'SYSEX_TX_MAX',
  ]),
  ...defines(configH, [
    'GPIO_N', 'N_GATE_BUS', 'N_NOTE_BUS', 'N_CV_BUS', 'N_NODE',
    'MAX_IN', 'MAX_OUT', 'N_PARAM', 'N_MIDI_IN_NODES', 'N_MIDI_OUT_NODES',
    'N_CC_MAP', 'MASTER_PPQN', 'CLOCK_MIN_BPM', 'CLOCK_MAX_BPM',
    'CLOCK_DEFAULT_BPM', 'MAX_SEQUENCE_LEN', 'NOTE_SEQ_VOICES', 'DRUM_SEQ_LANES',
  ]),
  ...defines(codecH, ['PATCH_FORMAT_VERSION']),
  ...defines(storeH, ['EEPROM_BYTES', 'PATCH_SLOTS']),
  ...defines(nrpnH, ['NRPN_CLOCK_BASE', 'NRPN_TRANSPORT_BASE', 'NRPN_RESERVED_BASE']),
};

// PATCH_MAGIC and PATCH_SLOT_BYTES are expressions, not plain numbers.
constants.PATCH_MAGIC = 0x4d4d4d43;
constants.PATCH_SLOT_BYTES = Math.floor(constants.EEPROM_BYTES / constants.PATCH_SLOTS);
constants.NO_BUS = 0xff;

const enums = {
  SysexCommand: enumeration(sysexH, 'SysexCommand'),
  SysexError: enumeration(sysexH, 'SysexError'),
  SysexEvent: enumeration(sysexH, 'SysexEvent'),
  MidiPort: enumeration(midiH, 'MidiPort'),
  CcTargetKind: enumeration(read('src/node/patch.h'), 'CcTargetKind'),
  CcClockTarget: enumeration(read('src/node/patch.h'), 'CcClockTarget'),
  CcTransportTarget: enumeration(read('src/node/patch.h'), 'CcTransportTarget'),
  CcFlags: enumeration(read('src/node/patch.h'), 'CcFlags'),
  GatePortDirection: enumeration(read('src/node/patch.h'), 'GatePortDirection'),
  ParamKind: enumeration(read('src/node/param.h'), 'ParamKind'),
  ConfigError: enumeration(read('src/node/registry.h'), 'ConfigError'),
  AlgorithmId: enumeration(read('src/node/registry.h'), 'AlgorithmId'),
};

// MIDI_CONTROL_PORT is a constexpr, not a #define.
const controlPort = read('src/hal/midi_types.h').match(
  /constexpr\s+uint8_t\s+MIDI_CONTROL_PORT\s*=\s*(\w+)\s*;/);
if (!controlPort) throw new Error('MIDI_CONTROL_PORT not found');
constants.MIDI_CONTROL_PORT = enums.MidiPort[controlPort[1]];
if (constants.MIDI_CONTROL_PORT === undefined) {
  throw new Error(`MIDI_CONTROL_PORT names ${controlPort[1]}, which is not a MidiPort`);
}

const lines = [];
lines.push('// GENERATED by editor/tools/generate-protocol.mjs - do not edit.');
lines.push('//');
lines.push('// The message layout is defined once, in the firmware headers, and this file');
lines.push('// is derived from them. `make editor` regenerates it and fails if the checked-in');
lines.push('// copy differs, so a protocol change breaks both builds at once instead of');
lines.push('// silently corrupting a patch on real hardware.');
lines.push('');
for (const [name, value] of Object.entries(constants).sort(([a], [b]) => a.localeCompare(b))) {
  lines.push(`export const ${name} = ${value};`);
}
lines.push('');
for (const [name, entries] of Object.entries(enums)) {
  lines.push(`export const ${name} = Object.freeze({`);
  for (const [key, value] of Object.entries(entries)) lines.push(`  ${key}: ${value},`);
  lines.push('});');
  lines.push('');
}
// A reverse lookup for the error codes, so a NAK reads as words in the UI.
lines.push('export const SysexErrorName = Object.freeze(Object.fromEntries(');
lines.push('  Object.entries(SysexError).map(([name, code]) => [code, name])));');
lines.push('export const ConfigErrorName = Object.freeze(Object.fromEntries(');
lines.push('  Object.entries(ConfigError).map(([name, code]) => [code, name])));');
lines.push('');

const text = lines.join('\n');
if (process.argv.includes('--check')) {
  const existing = readFileSync(out, 'utf8');
  if (existing !== text) {
    console.error('editor/src/protocol.js is out of date with the firmware headers.');
    console.error('Run: node editor/tools/generate-protocol.mjs');
    process.exit(1);
  }
  console.log('ok - editor/src/protocol.js matches the firmware headers');
} else {
  writeFileSync(out, text);
  console.log(`wrote ${out}`);
}
