// The scope and the piano rolls as panels: a canvas, its legend, and a
// painter that draws it every frame.

import { el } from '../dom.js';
import { Panel, Hint, Row } from '../components/Panel.js';
import { Legend, toggleIn } from '../components/Legend.js';
import { Switch } from '../components/Switch.js';
import { IconButton } from '../components/IconButton.js';
import { scopeRows, rollSources, nodeRollSources, drawScope, drawRoll, ROLL_H, NODE_ROLL_H } from './scope.js';
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

// The roll under one node, in the shades the buses have on the play tab's
// roll, so the two views agree.
export function NodeRollPanel(app, index) {
  if (!app.module || !app.session.usingModule) return null;
  const sources = nodeRollSources({ patch: app.state.patch, device: app.device }, index);
  if (!sources.length) return null;
  const { ui } = app.state;
  const hiddenKey = (key) => `node:${index}:${key}`;
  const shown = sources.filter((source) => !ui.rollHidden.has(hiddenKey(source.key)));
  const canvas = rollCanvas('the notes this node read and wrote in the last eight seconds');
  app.live.paint(({ module }) => drawRoll(canvas, module, shown, NODE_ROLL_H));
  return el('div', { class: 'grid node-roll' },
    el('div', { class: 'grid-title' }, 'piano roll'),
    el('div', { class: 'scope-wrap' }, canvas),
    Legend({ items: sources.map((source) => ({ ...source, key: hiddenKey(source.key) })), hidden: ui.rollHidden,
             onToggle: (key) => { toggleIn(ui.rollHidden, key); app.render(); } }));
}
