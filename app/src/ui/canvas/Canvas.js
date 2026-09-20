// The patch as blocks and arrows.
//
// **An arrow is drawn, not stored.** A patch has no cables in it: an outlet
// writes a bus, an inlet reads one, and an arrow is the observation that two
// ports are on the same bus (`connectionsOf`, core/graph.js). So the canvas
// is a *rendering of the patch itself*, never a second model beside it, and
// a bus changed from a selector in the inspector moves the arrow with it.
//
// Dragging from one socket to another therefore does not create anything: it
// puts two ports on one bus (`planConnection`), which is the same edit the
// selector makes and goes to the module as the same one-byte message.
//
// The keys are the accelerators for what the buttons already do - copy,
// paste, duplicate, delete - and never the only way to do any of it: most of
// this app is used on a phone, where there is no keyboard to press.

import * as P from '../../protocol/generated.js';
import { el, svg, classes } from '../dom.js';
import { IconButton } from '../components/IconButton.js';
import { Picker } from '../components/Picker.js';
import { openMenu, MenuItem, menuOpen } from '../components/Menu.js';
import { Domain, domainName, busCount } from '../../core/validate.js';
import {
  BlockKind, patchBlocks, connectionsOf, planConnection, planDisconnect, planClear, portOf,
  planModulation, modulationChoices, isModPort, busWords,
} from '../../core/graph.js';
import {
  BLOCK_W, HEAD_H, ROW_H, PAD_Y, blockHeight, socketPoint, layoutOf, worldSize, fitView, underBlock,
} from '../../core/layout.js';
import { catalogue, optionFor, optionsOf, ENDPOINTS } from '../../core/catalogue.js';
import { curve, arrowMarkers, drag } from './wires.js';
import './Canvas.css';

const MIN_ZOOM = 0.3;
const MAX_ZOOM = 1.8;
const MOVE_THRESHOLD = 4;      // a press this steady is a click, not a drag

// Everything drawn, computed once per render: the drag handlers, which run
// between renders, read it rather than rebuilding it.
export function geometry(app) {
  const blocks = patchBlocks(app.device, app.state.patch);
  const arrows = connectionsOf(blocks);
  const positions = layoutOf(blocks, arrows, app.state.ui.canvas.positions);
  const world = worldSize(blocks, positions);
  return { blocks, arrows, positions, world };
}

const select = (app, selection) => { app.state.ui.canvas.selected = selection; app.render(); };
const isSelected = (app, kind, id) =>
  app.state.ui.canvas.selected?.kind === kind && app.state.ui.canvas.selected.id === id;

// The disconnect of an arrow, planned against the patch as it is now.
const disconnect = (app, arrow) => {
  const fresh = geometry(app);
  const same = fresh.arrows.find((a) => a.id === arrow.id);
  app.editor.applyPlan(same ? planDisconnect(fresh.blocks, same) : { ok: false, why: 'already gone' });
};

// --- what a command acts on -------------------------------------------------
//
// Every command below re-reads the patch rather than closing over the
// geometry the page was drawn from: a key can be pressed between a render and
// the next one, and a block index from the drawing before last is a different
// node.
function selectedNow(app) {
  const selected = app.state.ui.canvas.selected;
  if (!selected) return null;
  const geom = geometry(app);
  if (selected.kind === 'arrow') {
    const arrow = geom.arrows.find((a) => a.id === selected.id);
    return arrow ? { geom, arrow } : null;
  }
  const block = geom.blocks.find((b) => b.id === selected.id);
  return block ? { geom, block } : null;
}

export const copySelected = (app) => {
  const found = selectedNow(app);
  if (found?.block) app.editor.copyBlock(found.block);
  else app.say(found ? 'an arrow is not something to copy — copy the block that draws it' : 'select a block first');
};

export const duplicateSelected = (app) => {
  const found = selectedNow(app);
  if (found?.block) app.editor.duplicateBlock(found.block, underBlock(found.geom.positions, found.block));
  else app.say(found ? 'an arrow is not something to duplicate' : 'select a block first');
};

// Delete is "take away what is selected", whichever kind of thing that is: a
// block leaves the patch, an arrow is the one bus the reader stops reading.
export const deleteSelected = (app) => {
  const found = selectedNow(app);
  if (found?.block) app.editor.removeBlock(found.block);
  else if (found?.arrow) app.editor.applyPlan(planDisconnect(found.geom.blocks, found.arrow));
  else app.say('select a block or an arrow first');
};

// The canvas with the whole window. The view is refitted on the way in and on
// the way out: the window has changed size under the patch, which is the
// other half of the rule in `mounted()` - a view left alone across that is a
// patch somewhere off the edge of a screen that is now a different shape.
export const toggleFull = (app) => {
  const canvas = app.state.ui.canvas;
  canvas.full = !canvas.full;
  canvas.fit = true;
  app.render();
};

// --- the keys ---------------------------------------------------------------

// Bound to the page once, not to the canvas: the tree is rebuilt on every
// edit, so a listener registered while the canvas is built would be added
// again on every render and would answer one press as many times as the page
// has been drawn. One listener for the life of the page, reading the state
// each time - and `canvasKey` is where the decision is, so it can be pressed
// in a test with no window in the room.
let bound = null;

export function bindCanvasKeys(app) {
  if (bound) { bound.app = app; return; }
  bound = { app };
  globalThis.addEventListener?.('keydown', (event) => canvasKey(bound.app, event));
}

// Somewhere a key is a character rather than a command.
const typingIn = (target) => {
  const tag = String(target?.tagName ?? '').toLowerCase();
  return target?.isContentEditable === true || tag === 'input' || tag === 'textarea' || tag === 'select';
};

// The shortcut, if this press is one. Returns what it did, for the tests and
// for nothing else.
export function canvasKey(app, event) {
  // The canvas is the patch tab's, and only what is on screen answers a key.
  if (app.state.ui.tab !== 'patch' || event.repeat || typingIn(event.target)) return null;
  const mod = event.metaKey || event.ctrlKey;
  const key = String(event.key ?? '').toLowerCase();

  if (key === 'escape') {
    // A menu is the innermost thing on screen, so Escape is the menu's first.
    if (menuOpen()) return null;
    if (app.state.ui.canvas.full) { event.preventDefault?.(); toggleFull(app); return 'full'; }
    if (!app.state.ui.canvas.selected) return null;
    event.preventDefault?.();
    app.state.ui.canvas.selected = null;
    app.render();
    return 'deselect';
  }
  if (!mod && (key === 'delete' || key === 'backspace')) {
    // Backspace is "go back" in a browser that has nothing else to do with it.
    event.preventDefault?.();
    deleteSelected(app);
    return 'delete';
  }
  if (!mod && key === 'f') { event.preventDefault?.(); toggleFull(app); return 'full'; }
  if (!mod || event.altKey) return null;
  if (key === 'c') { event.preventDefault?.(); copySelected(app); return 'copy'; }
  if (key === 'v') { event.preventDefault?.(); app.editor.paste(); return 'paste'; }
  // The browser's own ⌘D is a bookmark, which is never what was meant here.
  if (key === 'd') { event.preventDefault?.(); duplicateSelected(app); return 'duplicate'; }
  return null;
}

// ⌘ on a Mac and Ctrl everywhere else: a button whose tooltip names the wrong
// key is worse than one that names none.
const APPLE = /mac|iphone|ipad|ipod/i.test(
  globalThis.navigator?.userAgentData?.platform ?? globalThis.navigator?.platform ?? '');
export const shortcut = (key) => (APPLE ? `\u2318${key.toUpperCase()}` : `Ctrl+${key.toUpperCase()}`);

// --- the canvas -----------------------------------------------------------------

// One canvas per render: the DOM, the geometry it was drawn from, and the
// view transform, held together while the pointer works on it.
class CanvasView {
  constructor(app, geom) {
    this.app = app;
    this.geom = geom;
    this.paths = new Map();
    this.world = el('div', { class: 'canvas-world' });
    this.svg = svg('svg', {
      class: 'wires', width: geom.world.width, height: geom.world.height,
      viewBox: `0 0 ${geom.world.width} ${geom.world.height}`,
    }, arrowMarkers());
    this.ghost = svg('path', { class: 'wire ghost', fill: 'none', d: '' });
    this.viewport = el('div', { class: 'canvas-view', role: 'application',
                                'aria-label': 'the patch as blocks and arrows' }, this.world);
    this.build();
    this.bindViewport();
  }

  get view() { return this.app.state.ui.canvas.view; }
  set view(v) { this.app.state.ui.canvas.view = v; }

  build() {
    const { geom, world } = this;
    const cuts = [];
    for (const arrow of geom.arrows) cuts.push(...this.arrow(arrow));
    this.svg.append(this.ghost);
    world.append(this.svg);
    for (const block of geom.blocks) {
      const at = geom.positions.get(block.id);
      if (at) world.append(this.block(block, at));
    }
    world.append(...cuts);
    world.style.width = `${geom.world.width}px`;
    world.style.height = `${geom.world.height}px`;
  }

  // Where a client point falls in the canvas's own coordinates.
  toWorld(x, y) {
    const box = this.viewport.getBoundingClientRect();
    const v = this.view;
    return { x: (x - box.left - v.x) / v.k, y: (y - box.top - v.y) / v.k };
  }

  apply() {
    const v = this.view;
    this.world.style.transform = `translate(${v.x}px, ${v.y}px) scale(${v.k})`;
  }

  zoomAbout(px, py, factor) {
    const v = this.view;
    const k = Math.max(MIN_ZOOM, Math.min(MAX_ZOOM, v.k * factor));
    if (k === v.k) return;
    this.view = { k, x: px - (px - v.x) * (k / v.k), y: py - (py - v.y) * (k / v.k) };
    this.apply();
  }

  zoomFromButton(factor) {
    this.zoomAbout(this.viewport.clientWidth / 2, this.viewport.clientHeight / 2, factor);
  }

  // The view is fitted to the patch when the patch changed under it - loaded,
  // imported, dumped off a module - and left exactly alone otherwise: a
  // canvas that re-centres itself every time a parameter is touched is
  // unusable.
  mounted() {
    const canvas = this.app.state.ui.canvas;
    if (canvas.fit) {
      canvas.fit = false;
      this.view = fitView(this.geom.world, { width: this.viewport.clientWidth, height: this.viewport.clientHeight });
    }
    this.apply();
    this.redrawArrows();
  }

  // --- the arrows ----------------------------------------------------------

  arrow(arrow) {
    const { app } = this;
    const domain = domainName(arrow.domain);
    const selected = isSelected(app, 'arrow', arrow.id);
    // Two sources on one bus is a merge, worth seeing without counting
    // arrowheads. Fan-out is the ordinary case and is drawn plainly.
    const line = svg('path', {
      class: classes('wire', `dom-${domain}`, selected && 'selected', arrow.writers > 1 && 'merged'),
      'marker-end': `url(#arrow-${domain})`, fill: 'none',
    });
    // A four-pixel curve is not something a finger can hit, so every arrow
    // has an invisible one under it that is.
    const hit = svg('path', {
      class: 'wire-hit', fill: 'none',
      onpointerdown: (e) => e.stopPropagation(),
      onclick: () => select(app, { kind: 'arrow', id: arrow.id }),
    }, svg('title', {}, `${domain} bus ${arrow.bus}`
      + (arrow.writers > 1 || arrow.readers > 1 ? ` — ${arrow.writers} writing, ${arrow.readers} reading` : '')));
    const cut = selected ? IconButton({
      icon: 'cut', class: 'wire-cut', label: 'disconnect this arrow',
      onpointerdown: (e) => e.stopPropagation(),
      onclick: (e) => { e.stopPropagation(); disconnect(app, arrow); },
    }) : null;
    this.svg.append(line, hit);
    this.paths.set(arrow.id, { arrow, line, hit, cut });
    return cut ? [cut] : [];
  }

  // The arrows, from the positions as they are now. Called on every frame of
  // a block being dragged, so it writes into the paths that are already there.
  redrawArrows() {
    const { positions } = this.geom;
    for (const { arrow, line, hit, cut } of this.paths.values()) {
      const from = positions.get(arrow.from.blockId);
      const to = positions.get(arrow.to.blockId);
      if (!from || !to) continue;
      const shape = curve(socketPoint(from, arrow.from.at, true), socketPoint(to, arrow.to.at, false));
      line.setAttribute('d', shape.d);
      hit.setAttribute('d', shape.d);
      if (cut) { cut.style.left = `${shape.mid.x}px`; cut.style.top = `${shape.mid.y}px`; }
    }
  }

  // A gate bus is high or it is not, once per pass, and an arrow carrying
  // one lights when it is.
  paintLive(activity) {
    for (const { arrow, line } of this.paths.values()) {
      if (arrow.domain !== Domain.Gate) continue;
      line.classList.toggle('lit', (activity.gate & (1 << arrow.bus)) !== 0);
    }
  }

  // --- a block --------------------------------------------------------------

  block(block, position) {
    const { app } = this;
    const node = el('div', {
      class: classes('blk', `kind-${block.kind}`, isSelected(app, 'block', block.id) && 'selected', block.bad && 'bad'),
      'data-block': block.id,
      style: `left:${position.x}px; top:${position.y}px; height:${blockHeight(block)}px`,
      onpointerdown: (e) => this.startMove(e, block),
    },
      el('div', { class: 'blk-head' },
        block.kind === BlockKind.Node ? el('span', { class: 'index' }, block.index) : null,
        el('span', { class: 'blk-title' }, block.title),
        block.clocked ? el('span', { class: 'blk-clocked', title: 'runs from the master clock' }, '◴') : null,
        IconButton({
          icon: 'close', class: 'blk-remove', label: `remove ${block.title}`,
          onpointerdown: (e) => e.stopPropagation(),
          onclick: (e) => { e.stopPropagation(); app.editor.removeBlock(block); },
        })),
      el('div', { class: 'blk-body' },
        el('div', { class: 'blk-col' }, block.inlets.map((p) => this.socketRow(block, p, false))),
        el('div', { class: 'blk-col right' }, block.outlets.map((p) => this.socketRow(block, p, true)))));
    return node;
  }

  socketRow(block, port, isOutlet) {
    const domain = domainName(port.domain);
    const connected = port.buses.length > 0;
    const modulated = isModPort(port);
    const where = connected ? `${domain} ${busWords(port.buses)}` : 'not connected';
    const ref = { blockId: block.id, at: port.at, isOutlet };
    const socket = el('button', {
      class: classes('socket', `dom-${domain}`, connected && 'on', modulated && 'mod',
                     port.required && !connected && 'needed'),
      'data-block': block.id, 'data-at': String(port.at), 'data-outlet': isOutlet ? '1' : '',
      title: `${port.name} — ${where}`,
      'aria-label': `${block.title} ${port.name}, ${modulated ? `modulated from ${where}` : where}`,
      oncontextmenu: (e) => {
        e.preventDefault();
        this.app.editor.applyPlan(planClear(geometry(this.app).blocks, ref));
      },
      onpointerdown: (e) => this.startLink(e, ref),
    }, el('span', { class: 'dot' }));

    return el('div', { class: classes('blk-port', isOutlet ? 'out' : 'in', modulated && 'mod') },
      socket,
      el('span', { class: 'blk-port-name' }, port.name,
        port.required && !connected ? el('span', { class: 'required' }, '*') : null),
      connected ? el('span', { class: `blk-port-bus dom-${domain}` }, port.buses.join(',')) : null);
  }

  // --- interaction ----------------------------------------------------------

  // Moving a block. The DOM is written directly while the pointer is down
  // and the arrows are redrawn from the moved position: a render per frame
  // would rebuild every card in the page and take the focus with it.
  startMove(event, block) {
    if (event.button !== 0 && event.pointerType === 'mouse') return;
    event.stopPropagation();
    const start = this.geom.positions.get(block.id);
    if (!start) return;
    const element = event.currentTarget;
    drag(event, {
      threshold: MOVE_THRESHOLD,
      onStart: () => element.classList.add('moving'),
      onMove: ({ dx, dy }) => {
        const at = { x: Math.max(0, start.x + dx / this.view.k), y: Math.max(0, start.y + dy / this.view.k) };
        this.geom.positions.set(block.id, at);
        element.style.left = `${at.x}px`;
        element.style.top = `${at.y}px`;
        this.redrawArrows();
      },
      onEnd: ({ moved }) => {
        element.classList.remove('moving');
        if (!moved) { select(this.app, { kind: 'block', id: block.id }); return; }
        // No render: the element is already there, and rebuilding the page
        // under a pointer that has only just been let go takes the selection
        // and the scroll with it.
        this.app.arrangement.place(block.id, this.geom.positions.get(block.id));
      },
    });
  }

  // Dragging a connection. The ghost follows the pointer; what it lands on is
  // whatever socket is under it when it is let go, which is the same hit
  // test a finger makes.
  startLink(event, ref) {
    if (event.button !== 0 && event.pointerType === 'mouse') return;
    event.stopPropagation();
    const { app, geom, ghost, viewport } = this;
    const position = geom.positions.get(ref.blockId);
    const found = portOf(geom.blocks, ref);
    if (!position || !found) return;
    const anchor = socketPoint(position, ref.at, ref.isOutlet);
    const domain = domainName(found.port.domain);
    // While a connection is being dragged the sockets it could land on are
    // the only ones lit: the module refuses a domain mismatch, and finding
    // that out on release is finding it out too late.
    viewport.classList.add('linking', `only-${domain}`);

    drag(event, {
      onMove: ({ x, y }) => {
        const to = this.toWorld(x, y);
        const line = ref.isOutlet ? curve(anchor, { x: to.x + 9, y: to.y }) : curve(to, anchor);
        ghost.setAttribute('d', line.d);
        ghost.setAttribute('class', `wire ghost dom-${domain}`);
        const over = socketUnder(x, y);
        ghost.classList.toggle('over', Boolean(over) && over.blockId !== ref.blockId);
      },
      onEnd: ({ moved, x, y }) => {
        ghost.setAttribute('d', '');
        viewport.classList.remove('linking', 'only-gate', 'only-note', 'only-CV');
        if (!moved) { select(app, { kind: 'block', id: ref.blockId }); return; }
        const target = socketUnder(x, y);
        if (target) {
          app.editor.applyPlan(planConnection(geometry(app).blocks, app.device.capabilities, ref, target));
          return;
        }
        // A control signal let go over a *block* rather than a socket is the
        // gesture that makes a modulation route. The block has parameters,
        // not ports, so the editor has to ask which one.
        const onBlock = blockUnder(x, y);
        if (onBlock && ref.isOutlet && found.port.domain === Domain.CV) {
          askForParameter(app, ref, onBlock, { x, y });
          return;
        }
        app.say(onBlock ? 'only a control signal modulates a parameter' : 'drop on a socket');
      },
    });
  }

  // Pan with one pointer, zoom with two - and with the wheel, which is what
  // a trackpad's pinch arrives as.
  bindViewport() {
    const { viewport, app } = this;
    const pointers = new Map();
    let pan = null;
    let pinch = null;

    viewport.addEventListener('pointerdown', (e) => {
      if (e.target.closest('.blk') || e.target.closest('button')) return;
      viewport.setPointerCapture?.(e.pointerId);
      pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
      if (pointers.size === 1) {
        pan = { x: e.clientX, y: e.clientY, view: { ...this.view }, moved: false };
      } else if (pointers.size === 2) {
        pan = null;
        const [a, b] = [...pointers.values()];
        pinch = { spread: Math.hypot(a.x - b.x, a.y - b.y) };
      }
    });
    viewport.addEventListener('pointermove', (e) => {
      if (!pointers.has(e.pointerId)) return;
      pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
      if (pinch && pointers.size >= 2) {
        const [a, b] = [...pointers.values()];
        const spread = Math.hypot(a.x - b.x, a.y - b.y);
        if (pinch.spread > 0) {
          const box = viewport.getBoundingClientRect();
          this.zoomAbout((a.x + b.x) / 2 - box.left, (a.y + b.y) / 2 - box.top, spread / pinch.spread);
        }
        pinch.spread = spread;
        return;
      }
      if (!pan) return;
      const dx = e.clientX - pan.x;
      const dy = e.clientY - pan.y;
      if (!pan.moved && Math.abs(dx) + Math.abs(dy) < MOVE_THRESHOLD) return;
      pan.moved = true;
      viewport.classList.add('panning');
      this.view = { ...this.view, x: pan.view.x + dx, y: pan.view.y + dy };
      this.apply();
    });
    const release = (e) => {
      if (!pointers.has(e.pointerId)) return;
      pointers.delete(e.pointerId);
      if (pointers.size < 2) pinch = null;
      if (!pointers.size) {
        viewport.classList.remove('panning');
        // A press on the background that never moved is "nothing is
        // selected", which is how the inspector is put away.
        if (pan && !pan.moved && app.state.ui.canvas.selected) select(app, null);
        pan = null;
      }
    };
    viewport.addEventListener('pointerup', release);
    viewport.addEventListener('pointercancel', release);
    viewport.addEventListener('wheel', (e) => {
      e.preventDefault();
      const box = viewport.getBoundingClientRect();
      this.zoomAbout(e.clientX - box.left, e.clientY - box.top, Math.exp(-e.deltaY * 0.002));
    }, { passive: false });
  }
}

const blockUnder = (x, y) => document.elementFromPoint(x, y)?.closest?.('.blk')?.dataset?.block ?? null;

function socketUnder(x, y) {
  const element = document.elementFromPoint(x, y)?.closest?.('.socket');
  if (!element) return null;
  return { blockId: element.dataset.block, at: Number(element.dataset.at), isOutlet: element.dataset.outlet === '1' };
}

// The dropdown that finishes a modulation drag: every parameter of the block
// it landed on that is not already modulated, in the order the module
// describes them.
function askForParameter(app, ref, blockId, at) {
  const [kind, where] = String(blockId).split(':');
  if (kind !== BlockKind.Node) { app.say('only a node has parameters to modulate'); return; }
  const index = Number(where);
  const choices = modulationChoices(app.device, app.state.patch, index);
  if (!choices.length) {
    app.say(app.device?.byId.get(app.state.patch.nodes[index]?.algorithmId)?.params
      ? 'every parameter is already modulated'
      : 'still reading its parameters');
    return;
  }
  openMenu({
    at, head: 'modulate which parameter?',
    items: choices.map((choice) => MenuItem({
      label: choice.label, hint: `${choice.pd.min}–${choice.pd.max}`,
      onPick: () => app.editor.applyPlan(planModulation(geometry(app).blocks, app.state.patch,
                                                        app.device.capabilities, ref, blockId, choice.param,
                                                        { device: app.device })),
    })),
  });
}

// --- the panel ------------------------------------------------------------------

export function CanvasPanel(app, geom) {
  const canvas = new CanvasView(app, geom);
  const { full, clipboard } = app.state.ui.canvas;
  bindCanvasKeys(app);
  app.live.onMount(() => { if (canvas.viewport.isConnected) canvas.mounted(); });
  app.live.paint(({ activity }) => canvas.paintLive(activity));

  return el('section', {
    class: classes('canvas-panel', full && 'full'),
    // The stylesheet is given the same numbers the arrows are drawn from, so
    // a block's rows and its sockets cannot end up in different places.
    style: `--blk-w:${BLOCK_W}px; --blk-head:${HEAD_H}px; --blk-row:${ROW_H}px; --blk-pad:${PAD_Y}px`,
  },
    // The bar above the picture is where a patch grows: what to add, and the
    // button that adds it, beside the paste, the zoom and the window.
    el('div', { class: 'canvas-bar' },
      AddBar(app),
      el('div', { class: 'canvas-zoom' },
        IconButton({ icon: 'paste', label: `paste the copied block (${shortcut('v')})`, class: 'ghost',
                     disabled: !clipboard, onclick: () => app.editor.paste() }),
        IconButton({ icon: 'zoomOut', label: 'zoom out', class: 'ghost canvas-zoom-step',
                     onclick: () => canvas.zoomFromButton(1 / 1.25) }),
        IconButton({ icon: 'zoomIn', label: 'zoom in', class: 'ghost canvas-zoom-step',
                     onclick: () => canvas.zoomFromButton(1.25) }),
        IconButton({ icon: 'fit', label: 'fit the whole patch in the window', class: 'ghost',
                     onclick: () => { app.state.ui.canvas.fit = true; app.render(); } }),
        IconButton({ icon: full ? 'shrink' : 'expand', class: classes('ghost', full && 'active'),
                     label: full ? 'leave full screen (Esc)' : 'the canvas on the whole screen (F)',
                     'aria-pressed': full ? 'true' : 'false',
                     onclick: () => toggleFull(app) }))),
    canvas.viewport,
    el('div', { class: 'canvas-foot' },
      geom.blocks.length ? BusLegend(geom) : el('span', { class: 'hint' }, 'an empty patch'),
      el('span', { class: 'hint' }, capacityLine(app, geom))));
}

// Shelved by what each algorithm *is*, which the module says itself - so a
// list of thirty is six short lists of the kind of thing you came looking
// for, and an algorithm added to the firmware still arrives on a shelf.
function AddBar(app) {
  const { ui, patch } = app.state;
  const groups = catalogue(app.device.algorithms, ENDPOINTS, patch.nodes.map((n) => n.algorithmId));
  if (!optionFor(groups, ui.addPick)) ui.addPick = optionsOf(groups)[0]?.value ?? null;
  return el('div', { class: 'add-row' },
    el('div', { class: 'grow' }, Picker({
      value: ui.addPick, groups, label: 'what to add',
      onPick: (value) => { ui.addPick = value; app.render(); },
    })),
    IconButton({ icon: 'plus', label: 'add it to the patch', class: 'primary',
                 onclick: () => app.editor.add(ui.addPick) }));
}

// Which buses this patch is on, and what each one carries. A bus with two
// writers is a merge, named here rather than left to be inferred.
function BusLegend(geom) {
  const used = new Map();
  for (const arrow of geom.arrows) {
    const k = `${arrow.domain}:${arrow.bus}`;
    if (!used.has(k)) used.set(k, arrow);
  }
  if (!used.size) return el('span', { class: 'hint' }, 'nothing connected yet');
  return el('div', { class: 'canvas-legend' }, [...used.values()]
    .sort((a, b) => a.domain - b.domain || a.bus - b.bus)
    .map((arrow) => el('span', { class: `chip dom-${domainName(arrow.domain)}` },
      `${domainName(arrow.domain)} ${arrow.bus}`,
      arrow.writers > 1 ? el('span', { class: 'merge' }, ` ×${arrow.writers}`) : null)));
}

// What the module can hold and how much of it this patch has taken, read
// from the device rather than assumed.
function capacityLine(app, geom) {
  const caps = app.device.capabilities;
  const free = [Domain.Gate, Domain.Note, Domain.CV].map((domain) => {
    const written = new Set();
    for (const block of geom.blocks) {
      for (const port of block.outlets) {
        if (port.domain === domain) for (const bus of port.buses) written.add(bus);
      }
    }
    return `${domainName(domain)} ${busCount(caps, domain) - written.size}/${busCount(caps, domain)}`;
  });
  return `${app.state.patch.nodes.length}/${caps.nodes} nodes · free buses: ${free.join(' · ')}`;
}
