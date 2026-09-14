// The macro bench: where a macro is shaped.
//
// A destination carries a window over the macro's travel, a signed depth and
// a target. Those are numbers, not gestures, which is why the performance
// surface stops at a one-gesture learn and this is a panel beside the mod
// matrix - under the graph it acts on, like everything else that travels in
// the patch image.
//
// **The picture is the point.** Four rows of numbers do not say what a macro
// does; a band across its travel, rising where the destination acts and
// holding past the top of its window, does. It is what Massive's arc around a
// knob and Yamaha's curve display are both for, and it is the difference
// between a macro you can use and a table you have to simulate in your head.
//
// Three states a destination can be in look identical in a table and are
// three different problems: **silent** (nobody has moved the macro yet - a
// macro holds no position across a load, so this is correct rather than
// broken), **clipped** (it is writing, and the sum did not fit the target's
// range), and working. The module reports all three (SYSEX_MACRO_STATE) and
// they are drawn apart here.

import * as P from '../../protocol/generated.js';
import { el, classes, svg } from '../dom.js';
import { Panel, Hint, Row } from '../components/Panel.js';
import { Field, Fields } from '../components/Field.js';
import { NumberField } from '../components/NumberField.js';
import { Disclosure, remembered } from '../components/Disclosure.js';
import { describeTarget, TargetFields } from './ModMatrix.js';
import { isMacro, isDest, destsOfMacro, knobParams, paramDescriptorOf } from '../../core/patch.js';
import { CC_TARGET_KINDS } from '../../protocol/names.js';
import './Macros.css';

// The travel a macro has, and the travel a destination's window is cut from.
const TRAVEL = 255;

// A destination may not reach a macro - the firmware refuses it, so it is not
// offered - and may not press the transport, which is momentary and has
// nothing for a swept window to set.
const DEST_KINDS = CC_TARGET_KINDS.filter(
  (k) => k.value !== P.CcTargetKind.CC_TARGET_MACRO
      && k.value !== P.CcTargetKind.CC_TARGET_TRANSPORT);

// A new destination arrives pointed at something: the first parameter in the
// patch a knob could reach, over the macro's whole travel, pushing a third of
// that parameter's range. A destination that arrived needing three fields set
// before it did anything is a destination nobody made - the same reason a new
// node arrives connected (core/graph.js).
function emptyDest(app, macro) {
  const dest = {
    macro, targetKind: P.CcTargetKind.CC_TARGET_NODE, targetIndex: 0, param: 0,
    srcLo: 0, srcHi: TRAVEL, depth: 32, flags: 0,
  };
  for (const [index, node] of (app.state.patch.nodes ?? []).entries()) {
    const [first] = knobParams(app.device?.byId.get(node.algorithmId));
    if (!first) continue;
    dest.targetIndex = index;
    dest.param = first.at;
    dest.depth = Math.max(1, Math.round((first.pd.max - first.pd.min) / 3));
    break;
  }
  return dest;
}

export function Macros(app) {
  const caps = app.device?.capabilities;
  if (!caps) return null;
  const patch = app.state.patch;
  const macros = patch.macros ?? [];
  const pool = patch.macroDest ?? [];
  const used = pool.filter(isDest).length;
  const named = macros.filter(isMacro).length;

  return Panel('macros',
    Hint('one control, several parameters, each over its own window'),
    // The budget, from the module rather than from eight and thirty-two
    // written into this file: adding a destination has to fail here, in
    // advance, rather than on the wire.
    Hint(`${named}/${caps.macros} macros · ${used}/${caps.macroDests} destinations in the pool`
       + ` · ${caps.macroDestsPerMacro} per macro · each macro also spends one of the`
       + ` ${caps.ccMappings} bindings to be driven`),
    el('div', { class: 'macro-list' },
      macros.map((macro, index) => MacroCard(app, index, macro)).slice(0, caps.macros)));
}

function MacroCard(app, index, macro) {
  const live = app.session.macroLive.get(index) ?? null;
  const dests = destsOfMacro(app.state.patch, index);
  const exists = isMacro(macro);
  // Only a macro on screen is asked about, and only one that exists can
  // answer: the poll is for the controls being looked at (services/render.js).
  if (exists) app.live.watchMacro(index);

  const position = el('span', { class: 'macro-live' }, '');
  if (exists) {
    app.live.paint(() => {
      const state = app.session.macroLive.get(index);
      position.textContent = !state ? ''
        : state.engaged ? `at ${state.position}` : 'untouched';
      position.classList.toggle('engaged', Boolean(state?.engaged));
    });
  }

  return Disclosure({
    class: classes('macro', exists && 'active'),
    ...remembered(app.state.ui, `macro-${index}`),
    summary: [
      el('span', { class: 'index' }, index + 1),
      exists ? el('span', { class: 'macro-title' }, macro.name) : el('span', { class: 'hint' }, 'empty'),
      exists ? el('span', { class: 'hint' }, `${dests.length} destination${dests.length === 1 ? '' : 's'}`) : null,
      position,
    ],
  },
    NameField(app, index, macro),
    exists ? Windows(app, index, dests, live) : null,
    exists ? el('div', { class: 'dest-list' }, dests.map(({ slot, dest }) => DestRow(app, slot, dest, live))) : null,
    exists ? AddDest(app, index, dests) : null);
}

function NameField(app, index, macro) {
  const limit = app.device?.capabilities?.macroNameBytes ?? P.MACRO_NAME_BYTES;
  return el('div', {}, Fields(
    Field({ label: 'called', hint: `${limit} characters — the patch holds no more` },
      el('input', {
        type: 'text', class: 'grow', value: macro?.name ?? '', maxlength: String(limit),
        placeholder: `macro ${index + 1}`, 'aria-label': `the name of macro ${index + 1}`,
        // The field stops taking keystrokes at the limit rather than
        // accepting them and cutting them on the way out: the limit is real,
        // fixed-width in the patch image, and a name that vanished on save
        // would be a lie about what the module holds.
        onchange: (e) => app.editor.setMacro(index, e.target.value.trim()),
      }))),
    isMacro(macro)
      ? Row(el('button', {
          class: 'ghost danger',
          onclick: () => {
            for (const { slot } of destsOfMacro(app.state.patch, index)) app.editor.clearMacroDest(slot);
            app.editor.setMacro(index, '');
          },
        }, 'remove this macro'))
      : null);
}

// --- the picture ---------------------------------------------------------------

// One lane per destination, across the macro's 0..255 travel. The line is
// what that destination asks for at each position: nothing below `srcLo`,
// rising to its whole depth at `srcHi`, and **held** past it rather than
// falling back - which is why sweeping a macro up builds, and why a
// destination that rises and falls back is two lanes with opposite depths.
function Windows(app, index, dests, live) {
  const LANE = 34;
  const height = Math.max(LANE, dests.length * LANE) + 14;
  const lanes = [];
  const marks = [];

  dests.forEach(({ slot, dest }, row) => {
    const top = row * LANE;
    const mid = top + LANE / 2;
    const pd = paramDescriptorOf(app.device, app.state.patch.nodes[dest.targetIndex]?.algorithmId, dest.param);
    const range = Math.max(1, (pd?.max ?? 255) - (pd?.min ?? 0));
    // Relative to the target's own range, so two destinations on different
    // parameters are comparable, with a floor: a depth of one on a range of
    // 255 is still a destination and a flat line says it is not.
    const reach = Math.max(3, Math.min(1, Math.abs(dest.depth) / range) * (LANE / 2 - 4));
    const to = dest.depth < 0 ? mid + reach : mid - reach;
    const lo = Math.min(dest.srcLo, TRAVEL);
    const hi = Math.max(lo, Math.min(dest.srcHi, TRAVEL));
    const status = statusOf(live, slot);
    lanes.push(svg('g', { class: classes('lane', `is-${status}`) },
      svg('line', { class: 'lane-base', x1: 0, y1: mid, x2: TRAVEL, y2: mid }),
      // Where the window is, on the axis the window is cut from: the band a
      // reader looks for first is "from here to here".
      svg('rect', { class: 'lane-window', x: lo, y: top + 2, width: Math.max(1, hi - lo), height: LANE - 4 }),
      svg('path', {
        class: 'lane-fill',
        d: `M0 ${mid} L${lo} ${mid} L${hi} ${to} L${TRAVEL} ${to} L${TRAVEL} ${mid} Z`,
      }),
      svg('path', {
        class: 'lane-line',
        d: `M0 ${mid} L${lo} ${mid} L${hi} ${to} L${TRAVEL} ${to}`,
      })));
  });

  // Where the module says the macro actually is. Drawn on the same axis as
  // the windows, because "what is this doing right now" and "what did I ask
  // it to do" are the same question asked twice.
  const now = svg('line', { class: 'macro-now', x1: 0, y1: 0, x2: 0, y2: height - 14 });
  app.live.paint(() => {
    const state = app.session.macroLive.get(index);
    const at = state?.engaged ? state.position : null;
    now.setAttribute('x1', String(at ?? 0));
    now.setAttribute('x2', String(at ?? 0));
    now.style.opacity = at === null ? '0' : '1';
  });

  return el('div', { class: 'windows' },
    // The lanes keep their own height whatever the panel's width: stretched
    // to the width, a shallow ramp reads as a flat line.
    svg('svg', { viewBox: `0 0 ${TRAVEL} ${height}`, preserveAspectRatio: 'none',
                 style: `height: ${height}px`,
                 role: 'img', 'aria-label': 'where each destination acts across the macro’s travel' },
      lanes, marks, now,
      svg('line', { class: 'axis', x1: 0, y1: height - 12, x2: TRAVEL, y2: height - 12 })),
    el('div', { class: 'windows-scale hint' },
      el('span', {}, '0'), el('span', {}, 'the macro’s travel'), el('span', {}, String(TRAVEL))));
}

// What the module last said about this destination. Silent is not broken: a
// macro nobody has moved holds no position, so its destinations contribute
// nothing until it is first touched.
function statusOf(live, slot) {
  const found = live?.dests?.find((d) => d.slot === slot);
  if (!found) return 'unknown';
  switch (found.status) {
    case P.ModStatus.MOD_STATUS_CLIPPED: return 'clipped';
    case P.ModStatus.MOD_STATUS_SILENT: return 'silent';
    case P.ModStatus.MOD_STATUS_NO_TARGET: return 'no-target';
    default: return 'active';
  }
}

const SAID = {
  clipped: 'pinned against the end of its range',
  silent: 'silent until the macro is moved',
  'no-target': 'nothing is writing that target',
  active: 'writing',
  unknown: '',
};

// --- one destination ------------------------------------------------------------

function DestRow(app, slot, dest, live) {
  const d = { ...dest };
  const push = () => app.editor.setMacroDest(slot, d);
  const status = statusOf(live, slot);
  const pd = paramDescriptorOf(app.device, app.state.patch.nodes[d.targetIndex]?.algorithmId, d.param);
  const span = (pd?.max ?? 255) - (pd?.min ?? 0);

  return el('div', { class: classes('dest', `is-${status}`) },
    el('div', { class: 'dest-head' },
      el('span', { class: 'dest-what' }, describeTarget(app, d)),
      el('span', { class: 'hint' }, SAID[status]),
      el('button', { class: 'ghost danger', onclick: () => app.editor.clearMacroDest(slot) }, 'clear')),
    Fields(Field({ label: 'moves' }, ...TargetFields(app, d, push, DEST_KINDS))),
    Fields(
      Field({ label: 'from' }, NumberField({
        value: d.srcLo, min: 0, max: TRAVEL, wide: true, 'aria-label': 'the bottom of the window',
        onChange: (v) => { d.srcLo = v; push(); },
      })),
      Field({ label: 'to', hint: 'past here it holds' }, NumberField({
        value: d.srcHi, min: 0, max: TRAVEL, wide: true, 'aria-label': 'the top of the window',
        onChange: (v) => { d.srcHi = v; push(); },
      })),
      // Signed, and in the target's own units exactly as a binding's min and
      // max are: one macro opening a filter while closing a delay is the move
      // macros exist for.
      Field({ label: 'by', hint: pd ? `${pd.name}: ${pd.min}–${pd.max}` : 'in the target’s own units' },
        NumberField({
          value: d.depth, min: -span || -255, max: span || 255, wide: true,
          'aria-label': 'how far it pushes at the top of its window',
          onChange: (v) => { d.depth = v; push(); },
        }))));
}

function AddDest(app, index, dests) {
  const caps = app.device?.capabilities;
  const pool = app.state.patch.macroDest ?? [];
  const used = pool.filter(isDest).length;
  const full = used >= (caps?.macroDests ?? P.N_MACRO_DEST);
  const deep = dests.length >= (caps?.macroDestsPerMacro ?? P.N_MACRO_DEST_PER_MACRO);
  // Refused here rather than on the wire: the pool is shared, so "add a
  // destination" fails for a reason that is not about the destination, and a
  // NAK is no way to learn that.
  const why = deep ? `this macro holds ${caps?.macroDestsPerMacro} destinations`
    : full ? 'the shared pool is full' : null;
  return Row(
    el('button', {
      disabled: why ? 'disabled' : null,
      onclick: () => app.editor.addMacroDest(emptyDest(app, index)),
    }, 'add a destination'),
    why ? el('span', { class: 'hint' }, why) : null);
}
