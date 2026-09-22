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
  app.live.paint(({ monitor }) => drawScope(canvas, monitor, shown));
  return Panel('scope',
    rows.length ? el('div', { class: 'scope-wrap' }, canvas) : Hint('no jack, no gate bus, no CV bus'),
    rows.length ? Legend({ items: rows, hidden: ui.scopeHidden,
                           onToggle: (key) => { toggleIn(ui.scopeHidden, key); app.render(); } }) : null,
    Row(Switch({ checked: ui.scopeAll, label: 'every jack and bus',
                 onChange: (on) => { ui.scopeAll = on; app.render(); } })));
}

export function RollPanel(app) {
  const { ui } = app.state;
  const sources = rollSources(app.monitor.watchedBuses);
  const shown = sources.filter((source) => !ui.rollHidden.has(source.key));
  const canvas = rollCanvas('the notes of the last eight seconds');
  app.live.paint(({ monitor }) => drawRoll(canvas, monitor, shown, ROLL_H));
  return Panel('piano roll',
    el('div', { class: 'scope-wrap' }, canvas),
    el('div', { class: 'row legend-row' },
      Legend({ items: sources, hidden: ui.rollHidden,
               onToggle: (key) => { toggleIn(ui.rollHidden, key); app.render(); } }),
      IconButton({ icon: 'clear', label: 'clear the roll', class: 'ghost',
                   onclick: () => app.monitor.clearNotes() })));
}

// The signals under one block - a node, a jack, a MIDI port - in the shades
// the buses have on the monitor tab's scope and roll, so the two views agree.
// Its gate and control ports are rows of a scope, its note ports are sources
// of a roll, and one legend lists all of them: a chip pressed puts that
// trace away, as it does on the monitor tab. A block on no bus says so rather
// than drawing an empty grid, because an empty scope looks like a broken one.
//
// **Inputs first, then outputs**, and within each the scope above the roll:
// a converter that reads notes and writes a control signal shows its roll
// first, because that is the order the signal went through it.
export function BlockSignalsPanel(app, { kind, index }) {
  const { rows, sources } = blockSignals({ patch: app.state.patch, device: app.device }, { kind, index });
  const { ui } = app.state;
  const prefix = `${kind}:${index}:`;
  const hiddenKey = (key) => prefix + key;
  const isShown = (item) => !ui.traceHidden.has(hiddenKey(item.key));
  const items = [];
  const pictures = [];

  for (const role of ['in', 'out']) {
    const ofRole = (list) => list.filter((item) => item.role === role);
    const shownRows = ofRole(rows).filter(isShown);
    const shownSources = ofRole(sources).filter(isShown);
    items.push(...ofRole(rows), ...ofRole(sources));
    const way = role === 'in' ? 'read' : 'wrote';
    if (shownRows.length) {
      const scope = el('canvas', { class: 'scope',
        'aria-label': `the gate and control signals this block ${way} in the last four seconds` });
      app.live.paint(({ monitor }) => drawScope(scope, monitor, shownRows));
      pictures.push(el('div', { class: 'scope-wrap' }, scope));
    }
    if (shownSources.length) {
      const roll = rollCanvas(`the notes this block ${way} in the last eight seconds`);
      app.live.paint(({ monitor }) => drawRoll(roll, monitor, shownSources, NODE_ROLL_H));
      pictures.push(el('div', { class: 'scope-wrap' }, roll));
    }
  }

  return el('div', { class: 'grid block-signals' },
    el('div', { class: 'grid-title' }, 'signals'),
    !items.length ? Hint('on no bus — patch it on the canvas to see what it carries') : null,
    ...pictures,
    items.length ? Legend({ items: items.map((item) => ({ ...item, key: hiddenKey(item.key) })),
                            hidden: ui.traceHidden,
                            onToggle: (key) => { toggleIn(ui.traceHidden, key); app.render(); } }) : null);
}
