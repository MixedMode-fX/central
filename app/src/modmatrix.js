// The mod matrix: everything that moves a parameter while nobody is touching
// it. A controller's CC is one half of it and a control signal off the CV bus
// is the other, and they are one thing here because they are one thing in the
// module - both end at `PatchManager::set_param`, both reach the same target
// space, and the two tables read the same because "what does this move" has
// one answer.
//
// **It is part of the patch, not part of MIDI.** A binding travels in the
// patch image, a route travels in the patch image, and both are gone when the
// patch is replaced. Filing them under MIDI put them beside the cables and
// the clock - settings about the room, which outlive any patch - so a patch
// exported and loaded somewhere else appeared to lose them. They belong on
// the patch tab, under the graph they act on.

import * as P from './protocol.js';
import { el } from './views.js';
import { channelSelect, portToggles } from './midi.js';
import {
  ALL_MUSICAL, portNames, CC_TARGET_KINDS, CLOCK_TARGETS, TRANSPORT_TARGETS,
  TAKEOVER, RELATIVE, FOURTEEN_BIT, PASS_THROUGH, channelLabel,
} from './names.js';

// A disclosure whose open state outlives a re-render: the app holds it, keyed,
// because the DOM is rebuilt under it on every edit.
function disclosure(app, key, summaryContent, ...children) {
  const node = el('details', { class: 'binding' }, el('summary', {}, summaryContent), ...children);
  node.open = app.isOpen(key);
  node.addEventListener('toggle', () => app.setOpen(key, node.open));
  return node;
}

const emptyMapping = () => ({
  sourceMask: 0, channel: 0, cc: 1, targetKind: P.CcTargetKind.CC_TARGET_NODE,
  targetIndex: 0, param: 0, min: 0, max: 0, flags: 0,
});

// --- the panel -------------------------------------------------------------

// The two tables under one heading, below the graph they act on: what a
// controller moves, and what the patch moves itself.
export function modMatrixPanel(app) {
  if (!app.device?.capabilities) return null;
  return el('section', { class: 'panel' },
    el('h2', {}, 'mod matrix'),
    el('p', { class: 'hint' }, 'what moves a parameter when your hands are somewhere else'),
    bindingTable(app),
    routeTable(app));
}

// --- controller bindings ---------------------------------------------------

// One binding in words. This is the summary that did not exist: a table of
// bindings a user can read without turning every knob to find out what moves.
export function describeMapping(app, m) {
  if (!m || !m.sourceMask) return null;                 // no cable: not a binding
  // Every musical cable is what a binding made from a parameter's menu starts
  // with, and naming all seven of them says nothing a reader can use.
  const where = m.sourceMask === ALL_MUSICAL ? 'any cable' : portNames(m.sourceMask).join(', ');
  const from = `CC ${m.cc}, ${channelLabel(m.channel)}, from ${where}`;
  return { from, to: describeTarget(app, m), range: describeRange(app, m) };
}

// Shared by a controller binding and a modulation route: they reach the same
// target space, so "what does this move" has to read the same for both.
export function describeTarget(app, m) {
  switch (m.targetKind) {
    case P.CcTargetKind.CC_TARGET_NODE: {
      const node = app.patch.nodes[m.targetIndex];
      if (!node) return `node ${m.targetIndex} (not in this patch)`;
      const d = app.device?.byId.get(node.algorithmId);
      const pd = app.device?.describeParam(node.algorithmId, m.param);
      return `${d?.name ?? `algorithm ${node.algorithmId}`} ${m.targetIndex} · ${pd?.name ?? `parameter ${m.param}`}`;
    }
    case P.CcTargetKind.CC_TARGET_CLOCK:
      return `clock · ${CLOCK_TARGETS.find((t) => t.value === m.param)?.label ?? m.param}`;
    case P.CcTargetKind.CC_TARGET_TRANSPORT:
      return `transport · ${TRANSPORT_TARGETS.find((t) => t.value === m.param)?.label ?? m.param}`;
    default:
      return 'a target kind this firmware does not have';
  }
}

function describeRange(app, m) {
  if (m.min === 0 && m.max === 0) return 'full range';
  return `${m.min}–${m.max}`;
}

// --- controllers ------------------------------------------------------------

// What a knob moves, as a table a user can read without turning every knob on
// the desk to find out. Every field of every binding is editable below it, so
// a binding can be built, narrowed to a range, moved to another parameter or
// deleted with no controller in the room.
export function bindingTable(app) {
  const slots = app.patch.ccMap;
  const used = slots.map((m, slot) => ({ m, slot })).filter(({ m }) => m && m.sourceMask);

  const summary = used.length
    ? el('div', { class: 'table-scroll' }, el('table', { class: 'bindings' },
        el('thead', {}, el('tr', {},
          el('th', {}, 'slot'), el('th', {}, 'controller'), el('th', {}, 'moves'), el('th', {}, 'over'), el('th', {}, ''))),
        el('tbody', {}, used.map(({ m, slot }) => {
          const described = describeMapping(app, m);
          return el('tr', {},
            el('td', {}, String(slot)),
            el('td', {}, described.from),
            el('td', {}, described.to),
            el('td', {}, described.range),
            el('td', {}, el('button', { class: 'ghost danger', onclick: () => app.clearMapping(slot) }, 'clear')));
        }))))
    : el('p', { class: 'hint' }, 'nothing bound');

  const editors = slots.map((m, slot) => mappingEditor(app, slot, m)).filter(Boolean);

  return el('div', { class: 'matrix-half' },
    el('h3', {}, 'controllers'),
    el('p', { class: 'hint' }, `${used.length}/${slots.length} slots`),
    summary,
    app.learnTarget
      ? el('div', { class: 'notes' },
          el('h4', {}, 'waiting for a controller'),
          el('p', {}, describeTarget(app, {
            targetKind: P.CcTargetKind.CC_TARGET_NODE,
            targetIndex: app.learnTarget.nodeIndex,
            param: app.learnTarget.param,
          })),
          el('button', { onclick: () => app.cancelLearn() }, 'cancel'))
      : null,
    (() => {
      const all = el('details', { class: 'bindings-detail' },
        el('summary', {}, `all slots (${slots.length})`),
        el('div', { class: 'binding-list' }, editors));
      all.open = app.isOpen('bindings');
      all.addEventListener('toggle', () => app.setOpen('bindings', all.open));
      return all;
    })());
}

// --- modulation routes ------------------------------------------------------
//
// Routes are made on the canvas and their depth and mode are edited beside the
// parameter they move, which is where somebody asking "what is happening to
// this control" is looking. This is the other half: every route in one table,
// including the ones that have nowhere to be drawn - a route to the clock's
// tempo reaches something that is not a node, so no block carries it, and
// without this it would be in the patch and invisible.
export function routeTable(app) {
  const slots = app.patch.modMap ?? [];
  const limit = app.device?.capabilities?.modRoutes;
  const used = slots.map((r, slot) => ({ r, slot })).filter(({ r }) => r && r.bus !== P.NO_BUS);
  if (!used.length && !limit) return null;

  const summary = used.length
    ? el('div', { class: 'table-scroll' }, el('table', { class: 'bindings' },
        el('thead', {}, el('tr', {},
          el('th', {}, 'slot'), el('th', {}, 'signal'), el('th', {}, 'moves'),
          el('th', {}, 'how'), el('th', {}, ''))),
        el('tbody', {}, used.map(({ r, slot }) => el('tr', {},
          el('td', {}, String(slot)),
          el('td', {}, `CV bus ${r.bus}`),
          el('td', {}, describeTarget(app, r)),
          el('td', {}, [
            (r.flags & P.ModFlags.MOD_MODE_MASK) === P.ModMode.MOD_OFFSET ? 'offset' : 'absolute',
            `${Math.round((r.depth ?? 255) * 100 / 255)} %`,
            r.flags & P.ModFlags.MOD_BIPOLAR ? 'bipolar' : 'unipolar',
            r.flags & P.ModFlags.MOD_INVERT ? 'inverted' : null,
            r.min === 0 && r.max === 0 ? 'full range' : `${r.min}–${r.max}`,
          ].filter(Boolean).join(' · ')),
          el('td', {}, el('button', {
            class: 'ghost danger', onclick: () => app.clearModRoute(slot),
          }, 'clear')))))))
    : el('p', { class: 'hint' }, 'nothing modulated');

  return el('div', { class: 'matrix-half' },
    el('h3', {}, 'modulation'),
    el('p', { class: 'hint' }, `${used.length}/${limit ?? slots.length} routes`),
    summary);
}

function mappingEditor(app, slot, existing) {
  const m = existing ?? emptyMapping();
  const active = Boolean(existing && existing.sourceMask);
  const push = () => {
    app.patch.ccMap[slot] = m;
    app.edit(() => app.device.setCcMap(slot, m), 'binding');
    app.render();
  };

  const number = (value, min, max, label, onChange) => el('input', {
    type: 'number', class: 'number wide', min: String(min), max: String(max),
    value: String(value), inputmode: 'numeric', 'aria-label': label,
    onchange: (e) => onChange(Math.max(min, Math.min(max, Number(e.target.value) || 0))),
  });

  const kind = el('select', { onchange: (e) => {
    m.targetKind = Number(e.target.value);
    m.param = 0;
    m.targetIndex = 0;
    push();
  } });
  for (const k of CC_TARGET_KINDS) {
    const option = el('option', { value: String(k.value) }, k.label);
    if (k.value === m.targetKind) option.selected = true;
    kind.append(option);
  }

  const targetFields = [];
  if (m.targetKind === P.CcTargetKind.CC_TARGET_NODE) {
    const nodePick = el('select', { onchange: (e) => { m.targetIndex = Number(e.target.value); m.param = 0; push(); } });
    app.patch.nodes.forEach((node, index) => {
      const d = app.device?.byId.get(node.algorithmId);
      const option = el('option', { value: String(index) }, `${index} · ${d?.name ?? node.algorithmId}`);
      if (index === m.targetIndex) option.selected = true;
      nodePick.append(option);
    });
    if (!app.patch.nodes.length) nodePick.append(el('option', {}, 'no nodes in this patch'));

    const paramPick = el('select', { onchange: (e) => { m.param = Number(e.target.value); push(); } });
    const node = app.patch.nodes[m.targetIndex];
    const d = node ? app.device?.byId.get(node.algorithmId) : null;
    for (const group of d?.params ?? []) {
      if (!group || group.repeat > 1) continue;              // tables are not knob targets
      for (let f = 0; f < group.nFields; f++) {
        const pd = group.fields[f];
        if (!pd || (pd.min === 0 && pd.max === 0)) continue;
        const at = group.first + f;
        const option = el('option', { value: String(at) }, `${pd.name} (${pd.min}–${pd.max})`);
        if (at === m.param) option.selected = true;
        paramPick.append(option);
      }
    }
    targetFields.push(nodePick, paramPick);
  } else if (m.targetKind === P.CcTargetKind.CC_TARGET_CLOCK
          || m.targetKind === P.CcTargetKind.CC_TARGET_TRANSPORT) {
    const list = m.targetKind === P.CcTargetKind.CC_TARGET_CLOCK ? CLOCK_TARGETS : TRANSPORT_TARGETS;
    const pick = el('select', { onchange: (e) => { m.param = Number(e.target.value); push(); } });
    for (const t of list) {
      const option = el('option', { value: String(t.value) }, t.label);
      if (t.value === m.param) option.selected = true;
      pick.append(option);
    }
    targetFields.push(pick);
  }

  const takeover = el('select', { onchange: (e) => {
    m.flags = (m.flags & ~TAKEOVER.mask) | Number(e.target.value);
    push();
  } });
  for (const t of TAKEOVER.options) {
    const option = el('option', { value: String(t.value) }, t.label);
    if ((m.flags & TAKEOVER.mask) === t.value) option.selected = true;
    takeover.append(option);
  }

  const relative = el('select', { onchange: (e) => {
    m.flags = (m.flags & ~RELATIVE.mask) | Number(e.target.value);
    push();
  } });
  for (const t of RELATIVE.options) {
    const option = el('option', { value: String(t.value) }, t.label);
    if ((m.flags & RELATIVE.mask) === t.value) option.selected = true;
    relative.append(option);
  }

  const toggle = (bit, label) => {
    const box = el('input', { type: 'checkbox', class: 'switch',
      onchange: (e) => { m.flags = e.target.checked ? (m.flags | bit) : (m.flags & ~bit); push(); } });
    box.checked = (m.flags & bit) !== 0;
    return el('label', { class: 'bool' }, box, el('span', {}, label));
  };

  const field = (name, ...controls) => el('div', { class: 'field' },
    el('span', { class: 'field-name' }, name), ...controls);

  const editor = disclosure(app, `binding-${slot}`,
    [el('span', { class: 'slot-index' }, slot),
     active
       ? el('span', {}, `CC ${m.cc} ${channelLabel(m.channel)} → ${describeTarget(app, m)}`)
       : el('span', { class: 'hint' }, 'empty')],
    el('div', { class: 'fields' },
      field('CC number', number(m.cc, 0, 119, `binding ${slot} CC number`, (v) => { m.cc = v; push(); })),
      field('channel', channelSelect(m.channel, (c) => { m.channel = c; push(); })),
      field('controls', kind, ...targetFields)),
    el('div', { class: 'fields' },
      field('listens on', portToggles(m.sourceMask, (mask) => { m.sourceMask = mask; push(); },
        { label: `binding ${slot} source ports` }))),
    el('div', { class: 'fields' },
      field('sweeps from', number(m.min, 0, 16383, `binding ${slot} low end`, (v) => { m.min = v; push(); })),
      field('to', number(m.max, 0, 16383, `binding ${slot} high end`, (v) => { m.max = v; push(); }),
        el('span', { class: 'hint' }, 'both zero: full range')),
      field('knob catches up by', takeover),
      field('the controller sends', relative)),
    el('div', { class: 'fields' },
      field('', toggle(FOURTEEN_BIT, '14-bit')),
      field('', toggle(PASS_THROUGH, 'pass the CC on'))),
    el('div', { class: 'row' },
      el('button', { onclick: () => app.learnInto(slot, m) }, 'learn'),
      active ? el('button', { class: 'danger', onclick: () => app.clearMapping(slot) }, 'clear') : null,
      !m.sourceMask
        ? el('span', { class: 'hint' }, 'no source port: off')
        : null));
  if (active) editor.classList.add('active');
  return editor;
}

