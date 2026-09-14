// The mod matrix: everything that moves a parameter while nobody is touching
// it. A controller's CC is one half of it and a control signal off the CV bus
// is the other, and they are one thing here because they are one thing in
// the module - both end at `PatchManager::set_param`.
//
// **It is part of the patch, not part of MIDI.** A binding and a route
// travel in the patch image and are gone when the patch is replaced, so they
// are read under the graph they act on rather than beside the cables and the
// clock, which outlive any patch.

import * as P from '../../protocol/generated.js';
import { el, classes } from '../dom.js';
import { Panel, Hint, Row, Notice } from '../components/Panel.js';
import { Table } from '../components/Table.js';
import { Field, Fields } from '../components/Field.js';
import { Select } from '../components/Select.js';
import { NumberField } from '../components/NumberField.js';
import { Switch } from '../components/Switch.js';
import { Disclosure, remembered } from '../components/Disclosure.js';
import { ChannelSelect } from '../controls/ChannelSelect.js';
import { PortToggles } from '../controls/PortToggles.js';
import { isBinding, isRoute, knobParams } from '../../core/patch.js';
import { CC_MAX } from '../../core/graph.js';
import {
  ALL_MUSICAL, portNames, CC_TARGET_KINDS, CLOCK_TARGETS, TRANSPORT_TARGETS, KEY_TARGETS,
  TAKEOVER, RELATIVE, FOURTEEN_BIT, PASS_THROUGH, channelLabel,
} from '../../protocol/names.js';
import './ModMatrix.css';

const emptyMapping = () => ({
  sourceMask: 0, channel: 0, cc: 1, targetKind: P.CcTargetKind.CC_TARGET_NODE,
  targetIndex: 0, param: 0, min: 0, max: 0, flags: 0,
});

// The fields of every target that is not a node parameter, by kind.
const FIELDS_OF = {
  [P.CcTargetKind.CC_TARGET_CLOCK]: CLOCK_TARGETS,
  [P.CcTargetKind.CC_TARGET_TRANSPORT]: TRANSPORT_TARGETS,
  [P.CcTargetKind.CC_TARGET_KEY]: KEY_TARGETS,
};

const rangeText = (m) => (m.min === 0 && m.max === 0 ? 'full range' : `${m.min}–${m.max}`);

// Shared by a controller binding and a modulation route: they reach the same
// target space, so "what does this move" has to read the same for both.
export function describeTarget(app, m) {
  const named = (list, what) => `${what} · ${list.find((t) => t.value === m.param)?.label ?? m.param}`;
  switch (m.targetKind) {
    case P.CcTargetKind.CC_TARGET_NODE: {
      const node = app.state.patch.nodes[m.targetIndex];
      if (!node) return `node ${m.targetIndex} (not in this patch)`;
      const d = app.device?.byId.get(node.algorithmId);
      const pd = app.device?.describeParam(node.algorithmId, m.param);
      return `${d?.name ?? `algorithm ${node.algorithmId}`} ${m.targetIndex} · ${pd?.name ?? `parameter ${m.param}`}`;
    }
    case P.CcTargetKind.CC_TARGET_CLOCK: return named(CLOCK_TARGETS, 'clock');
    case P.CcTargetKind.CC_TARGET_TRANSPORT: return named(TRANSPORT_TARGETS, 'transport');
    case P.CcTargetKind.CC_TARGET_KEY: return named(KEY_TARGETS, 'key');
    // A macro is named by the patch, and the name is the whole reason it is a
    // macro rather than several bindings that share a number.
    case P.CcTargetKind.CC_TARGET_MACRO: {
      const macro = app.state.patch.macros?.[m.targetIndex];
      return `macro ${m.targetIndex + 1}${macro?.name ? ` · ${macro.name}` : ''}`;
    }
    default: return 'a target kind this firmware does not have';
  }
}

export function ModMatrix(app) {
  if (!app.device?.capabilities) return null;
  return Panel('mod matrix',
    Hint('what moves a parameter when your hands are somewhere else'),
    BindingTable(app),
    RouteTable(app));
}

const clearButton = (onclick) => el('button', { class: 'ghost danger', onclick }, 'clear');

// What a knob moves, as a table a user can read without turning every knob on
// the desk. Every field of every binding is editable below it.
export function BindingTable(app) {
  const slots = app.state.patch.ccMap;
  const used = slots.map((m, slot) => ({ m, slot })).filter(({ m }) => isBinding(m));
  const learn = app.editor?.learnTarget;

  return el('div', { class: 'matrix-half' },
    el('h3', {}, 'controllers'),
    Hint(`${used.length}/${slots.length} slots`),
    used.length
      ? Table({
          head: ['slot', 'controller', 'moves', 'over', ''],
          rows: used.map(({ m, slot }) => [
            String(slot),
            // Every musical cable is what a binding made from a parameter's
            // menu starts with, and naming all seven says nothing.
            `CC ${m.cc}, ${channelLabel(m.channel)}, from `
              + `${m.sourceMask === ALL_MUSICAL ? 'any cable' : portNames(m.sourceMask).join(', ')}`,
            describeTarget(app, m),
            rangeText(m),
            clearButton(() => app.editor.clearMapping(slot)),
          ]),
        })
      : Hint('nothing bound'),
    learn ? Notice({ kind: 'notes', title: 'waiting for a controller',
                     text: [el('p', {}, describeTarget(app, {
                       targetKind: P.CcTargetKind.CC_TARGET_NODE, targetIndex: learn.nodeIndex, param: learn.param,
                     })), el('button', { onclick: () => app.editor.cancelLearn() }, 'cancel')] })
          : null,
    Disclosure({ summary: `all slots (${slots.length})`, class: 'bindings-detail', ...remembered(app.state.ui, 'bindings') },
      el('div', { class: 'binding-list' }, slots.map((m, slot) => MappingEditor(app, slot, m)))));
}

// Every route in one table, including the ones that have nowhere to be drawn:
// a route to the clock's tempo reaches something that is not a node.
export function RouteTable(app) {
  const slots = app.state.patch.modMap ?? [];
  const limit = app.device?.capabilities?.modRoutes;
  const used = slots.map((r, slot) => ({ r, slot })).filter(({ r }) => isRoute(r));
  if (!used.length && !limit) return null;

  return el('div', { class: 'matrix-half' },
    el('h3', {}, 'modulation'),
    Hint(`${used.length}/${limit ?? slots.length} routes`),
    used.length
      ? Table({
          head: ['slot', 'signal', 'moves', 'how', ''],
          rows: used.map(({ r, slot }) => [
            String(slot),
            `CV bus ${r.bus}`,
            describeTarget(app, r),
            [
              (r.flags & P.ModFlags.MOD_MODE_MASK) === P.ModMode.MOD_OFFSET ? 'offset' : 'absolute',
              `${Math.round((r.depth ?? 255) * 100 / 255)} %`,
              r.flags & P.ModFlags.MOD_BIPOLAR ? 'bipolar' : 'unipolar',
              r.flags & P.ModFlags.MOD_INVERT ? 'inverted' : null,
              rangeText(r),
            ].filter(Boolean).join(' · '),
            clearButton(() => app.editor.clearModRoute(slot)),
          ]),
        })
      : Hint('nothing modulated'));
}

// The target pickers: what kind of thing, which one of them, and which of
// its fields. Shared by a controller binding and a macro destination, because
// they reach the same target space - and a destination is refused some of it,
// which is what `kinds` narrows.
export function TargetFields(app, entry, push, kinds = CC_TARGET_KINDS) {
  const fields = [Select({
    options: kinds, value: entry.targetKind, 'aria-label': 'what kind of target',
    onChange: (v) => { entry.targetKind = v; entry.param = 0; entry.targetIndex = 0; push(); },
  })];
  if (entry.targetKind === P.CcTargetKind.CC_TARGET_NODE) {
    const nodes = app.state.patch.nodes;
    fields.push(Select({
      options: nodes.length
        ? nodes.map((node, index) => ({ value: index, label: `${index} · ${app.device?.byId.get(node.algorithmId)?.name ?? node.algorithmId}` }))
        : [{ value: 0, label: 'no nodes in this patch' }],
      value: entry.targetIndex, 'aria-label': 'which node',
      onChange: (v) => { entry.targetIndex = v; entry.param = 0; push(); },
    }));
    const node = nodes[entry.targetIndex];
    // Tables are not knob targets: only the header parameters are offered.
    fields.push(Select({
      options: knobParams(node ? app.device?.byId.get(node.algorithmId) : null)
        .map(({ at, pd }) => ({ value: at, label: `${pd.name} (${pd.min}–${pd.max})` })),
      value: entry.param, 'aria-label': 'which parameter',
      onChange: (v) => { entry.param = v; push(); },
    }));
  } else {
    // Everything that is not a node is one short list of fields. A kind with
    // no fields - the reserved port one - offers nothing, which is the
    // honest thing for a target that does not exist yet.
    fields.push(Select({
      options: FIELDS_OF[entry.targetKind] ?? [], value: entry.param, 'aria-label': 'which field',
      onChange: (v) => { entry.param = v; push(); },
    }));
  }
  return fields;
}

function MappingEditor(app, slot, existing) {
  const m = existing ?? emptyMapping();
  const active = isBinding(existing);
  const push = () => app.editor.setCcMap(slot, m);
  const number = (key, min, max, label) => NumberField({
    value: m[key], min, max, wide: true, 'aria-label': `binding ${slot} ${label}`,
    onChange: (v) => { m[key] = v; push(); },
  });
  const flagSelect = (group) => Select({
    options: group.options, value: m.flags & group.mask,
    onChange: (v) => { m.flags = (m.flags & ~group.mask) | v; push(); },
  });
  const flagSwitch = (bit, label) => Switch({
    checked: (m.flags & bit) !== 0, label,
    onChange: (on) => { m.flags = on ? (m.flags | bit) : (m.flags & ~bit); push(); },
  });

  const [kind, ...targetFields] = TargetFields(app, m, push);

  return Disclosure({
    class: classes('binding', active && 'active'), ...remembered(app.state.ui, `binding-${slot}`),
    summary: [el('span', { class: 'index' }, slot),
              active ? el('span', {}, `CC ${m.cc} ${channelLabel(m.channel)} → ${describeTarget(app, m)}`)
                     : el('span', { class: 'hint' }, 'empty')],
  },
    Fields(
      Field({ label: 'CC number' }, number('cc', 0, CC_MAX, 'CC number')),
      Field({ label: 'channel' }, ChannelSelect({ value: m.channel, onChange: (c) => { m.channel = c; push(); } })),
      Field({ label: 'controls' }, kind, ...targetFields)),
    Fields(
      Field({ label: 'listens on' }, PortToggles({
        mask: m.sourceMask, label: `binding ${slot} source ports`,
        onChange: (mask) => { m.sourceMask = mask; push(); },
      }))),
    Fields(
      Field({ label: 'sweeps from' }, number('min', 0, 16383, 'low end')),
      Field({ label: 'to', hint: 'both zero: full range' }, number('max', 0, 16383, 'high end')),
      Field({ label: 'knob catches up by' }, flagSelect(TAKEOVER)),
      Field({ label: 'the controller sends' }, flagSelect(RELATIVE))),
    Fields(
      Field({}, flagSwitch(FOURTEEN_BIT, '14-bit')),
      Field({}, flagSwitch(PASS_THROUGH, 'pass the CC on'))),
    Row(
      el('button', { onclick: () => app.editor.learnInto(m, slot) }, 'learn'),
      active ? el('button', { class: 'danger', onclick: () => app.editor.clearMapping(slot) }, 'clear') : null,
      m.sourceMask ? null : el('span', { class: 'hint' }, 'no source port: off')));
}
