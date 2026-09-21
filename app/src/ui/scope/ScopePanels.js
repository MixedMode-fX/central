// The scope and the piano rolls as panels: a canvas, its legend, and a
// painter that draws it every frame.

import { el } from '../dom.js';
import { Panel, Hint, Row } from '../components/Panel.js';
import { Legend, toggleIn } from '../components/Legend.js';
import { Switch } from '../components/Switch.js';
import { IconButton } from '../components/IconButton.js';
import { scopeRows, rollSources, blockSignals, drawScope, drawRoll, ROLL_H, NODE_ROLL_H } from './scope.js';
import './Scope.css';

const rollCanvas = (label) => el('canvas', { class: 'roll', 'aria-label': label });

export function ScopePanel(app) {
  const { ui, patch } = app.state;
  const rows = scopeRows({ patch, device: app.device, scopeAll: ui.scopeAll });
  const shown = rows.filter((row) => !ui.scopeHidden.has(row.key));
  const canvas = el('canvas', { class: 'scope',
                                'aria-label': 'jack, gate bus and CV bus levels over the last four seconds' });
  app.live.paint(({ module }) => drawScope(canvas, module, shown));
  return Panel('scope',
    rows.length ? el('div', { class: 'scope-wrap' }, canvas) : Hint('no jack, no gate bus, no CV bus'),
    rows.length ? Legend({ items: rows, hidden: ui.scopeHidden,
                           onToggle: (key) => { toggleIn(ui.scopeHidden, key); app.render(); } }) : null,
    Row(Switch({ checked: ui.scopeAll, label: 'every jack and bus',
                 onChange: (on) => { ui.scopeAll = on; app.render(); } })));
}

export function RollPanel(app) {
  const { ui } = app.state;
  const sources = rollSources(app.watchedBuses);
  const shown = sources.filter((source) => !ui.rollHidden.has(source.key));
  const canvas = rollCanvas('the notes of the last eight seconds');
  app.live.paint(({ module }) => drawRoll(canvas, module, shown, ROLL_H));
  return Panel('piano roll',
    el('div', { class: 'scope-wrap' }, canvas),
    el('div', { class: 'row legend-row' },
      Legend({ items: sources, hidden: ui.rollHidden,
               onToggle: (key) => { toggleIn(ui.rollHidden, key); app.render(); } }),
      IconButton({ icon: 'clear', label: 'clear the roll', class: 'ghost',
                   onclick: () => app.module.clearNotes() })));
}

// The signals under one block - a node, a jack, a MIDI port - in the shades
// the buses have on the module tab's scope and roll, so the two views agree.
// Its gate and control ports are rows of a scope, its note ports are sources
// of a roll, and one legend lists all of them: a chip pressed puts that
// trace away, as it does on the module tab. A block on no bus says so rather
// than drawing an empty grid, because an empty scope looks like a broken one.
export function BlockSignalsPanel(app, { kind, index }) {
  if (!app.module || !app.session.usingModule) return null;
  const { rows, sources } = blockSignals({ patch: app.state.patch, device: app.device }, { kind, index });
  const { ui } = app.state;
  const prefix = `${kind}:${index}:`;
  const hiddenKey = (key) => prefix + key;
  const isShown = (item) => !ui.traceHidden.has(hiddenKey(item.key));
  const shownRows = rows.filter(isShown);
  const shownSources = sources.filter(isShown);
  const items = [...rows, ...sources].map((item) => ({ ...item, key: hiddenKey(item.key) }));

  const scope = shownRows.length ? el('canvas', { class: 'scope',
    'aria-label': 'the gate and control signals this block read and wrote in the last four seconds' }) : null;
  if (scope) app.live.paint(({ module }) => drawScope(scope, module, shownRows));
  const roll = shownSources.length
    ? rollCanvas('the notes this block read and wrote in the last eight seconds') : null;
  if (roll) app.live.paint(({ module }) => drawRoll(roll, module, shownSources, NODE_ROLL_H));

  return el('div', { class: 'grid block-signals' },
    el('div', { class: 'grid-title' }, 'signals'),
    !items.length ? Hint('on no bus — patch it on the canvas to see what it carries') : null,
    scope ? el('div', { class: 'scope-wrap' }, scope) : null,
    roll ? el('div', { class: 'scope-wrap' }, roll) : null,
    items.length ? Legend({ items, hidden: ui.traceHidden,
                            onToggle: (key) => { toggleIn(ui.traceHidden, key); app.render(); } }) : null);
}
