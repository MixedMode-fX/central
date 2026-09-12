// The patch as blocks and arrows.
//
// The list of node cards says what a patch *contains*. It cannot say what a
// patch *is*, because the shape of a patch is which thing feeds which - and
// under the bus model that shape is spread across a dozen selectors reading
// "gate bus 2". Following a signal meant reading every card and matching
// numbers. This view draws the matching.
//
// **An arrow is drawn, not stored.** A patch has no cables in it: an outlet
// writes a bus, an inlet reads one, and an arrow is the observation that two
// ports are on the same bus (`connectionsOf`, in graph.js). So the canvas is a
// *rendering of the patch itself*, never a second model beside it - there is
// nothing here that can drift out of step with what the module is running, and
// a bus changed from a selector in the inspector moves the arrow with it.
//
// Dragging from one socket to another therefore does not create anything: it
// puts two ports on one bus (`planConnection`), which is the same edit the
// selector makes and goes to the module as the same one-byte message. Which is
// also why the two views are not alternatives to be chosen between so much as
// two ways of reading one patch - the canvas for its shape, the card below it
// for the detail of whichever block is selected.

import * as P from './protocol.js';
import { el, nodeCard, jackCard, iconButton, closeMenu } from './views.js';
import { Domain, domainName, busCount } from './validate.js';
import { routeCard } from './midi.js';
import { nodeRollPanel } from './scope.js';
import {
  BlockKind, patchBlocks, connectionsOf, planConnection, planDisconnect, planClear, portOf,
  planModulation, modulationChoices, isModPort,
} from './graph.js';
import {
  BLOCK_W, HEAD_H, ROW_H, PAD_Y, blockHeight, socketPoint, layoutOf, worldSize, fitView,
} from './layout.js';
import { catalogue, richSelect, optionFor, optionsOf } from './picker.js';

const MIN_ZOOM = 0.3;
const MAX_ZOOM = 1.8;
const MOVE_THRESHOLD = 4;      // a press this steady is a click, not a drag

// Everything drawn, computed once per render and kept so the drag handlers -
// which run between renders - can answer "what is where" without rebuilding it.
export function geometry(app) {
  const blocks = patchBlocks(app.device, app.patch);
  const arrows = connectionsOf(blocks);
  const positions = layoutOf(blocks, arrows, app.canvasPositions);
  const world = worldSize(blocks, positions);
  return { blocks, arrows, positions, world };
}

// --- pointer plumbing -------------------------------------------------------

// One drag, on the window, so a pointer that leaves the element it started on
// keeps being followed - which is every drag that matters here, since a
// connection is made by leaving one block and arriving at another.
function drag(event, { threshold = 0, onStart, onMove, onEnd } = {}) {
  const from = { x: event.clientX, y: event.clientY };
  let started = threshold <= 0;
  if (started) onStart?.(from);

  const move = (e) => {
    const dx = e.clientX - from.x;
    const dy = e.clientY - from.y;
    if (!started) {
      if (Math.abs(dx) + Math.abs(dy) < threshold) return;
      started = true;
      onStart?.(from);
    }
    onMove?.({ dx, dy, x: e.clientX, y: e.clientY });
  };
  const up = (e) => {
    window.removeEventListener('pointermove', move);
    window.removeEventListener('pointerup', up);
    window.removeEventListener('pointercancel', up);
    onEnd?.({ moved: started, x: e.clientX, y: e.clientY });
  };
  window.addEventListener('pointermove', move);
  window.addEventListener('pointerup', up);
  window.addEventListener('pointercancel', up);
}

// --- the wires --------------------------------------------------------------

// An arrow leaves an outlet heading right and arrives at an inlet heading
// right, always: the handles are horizontal, so the curve says which way the
// signal goes before the arrowhead does. A connection that runs backwards up
// the patch - a sequencer resetting the divider that advances it - bows out
// further rather than doubling back through the blocks between.
function curve(from, to) {
  const dx = to.x - from.x;
  const handle = dx >= 0
    ? Math.max(40, Math.min(dx * 0.5, 160))
    : Math.max(90, Math.min(-dx * 0.35 + 90, 220));
  const end = { x: to.x - 9, y: to.y };      // room for the arrowhead
  return {
    d: `M ${from.x} ${from.y} C ${from.x + handle} ${from.y}, `
     + `${end.x - handle} ${end.y}, ${end.x} ${end.y}`,
    // The cubic at t = 0.5, which is where the arrow's own controls go: the
    // straight-line midpoint is off the curve wherever it bends.
    mid: {
      x: (from.x + 3 * (from.x + handle) + 3 * (end.x - handle) + end.x) / 8,
      y: (from.y + 3 * from.y + 3 * end.y + end.y) / 8,
    },
  };
}

const svgEl = (tag, attrs = {}) => {
  const node = document.createElementNS('http://www.w3.org/2000/svg', tag);
  for (const [key, value] of Object.entries(attrs)) {
    if (value !== null && value !== undefined) node.setAttribute(key, String(value));
  }
  return node;
};

function arrowMarkers() {
  const defs = svgEl('defs');
  for (const [name, colour] of [['gate', 'var(--gate)'], ['note', 'var(--note)'], ['CV', 'var(--cv)']]) {
    const marker = svgEl('marker', {
      id: `arrow-${name}`, viewBox: '0 0 10 10', refX: 10, refY: 5,
      markerWidth: 6, markerHeight: 6, orient: 'auto-start-reverse',
    });
    marker.append(svgEl('path', { d: 'M 0 0 L 10 5 L 0 10 z', fill: colour }));
    defs.append(marker);
  }
  return defs;
}

// --- a block ----------------------------------------------------------------

function socketRow(app, block, port, isOutlet) {
  const domain = domainName(port.domain);
  const connected = port.bus !== P.NO_BUS;
  const modulated = isModPort(port);
  const where = connected ? `${domain} bus ${port.bus}` : 'not connected';
  const socket = el('button', {
    class: `socket dom-${domain}${connected ? ' on' : ''}${modulated ? ' mod' : ''}`
         + `${port.required && !connected ? ' needed' : ''}`,
    'data-block': block.id, 'data-at': String(port.at), 'data-outlet': isOutlet ? '1' : '',
    title: `${port.name} — ${where}`,
    'aria-label': `${block.title} ${port.name}, `
                + `${modulated ? `modulated from ${where}` : where}`,
    oncontextmenu: (e) => {
      e.preventDefault();
      app.applyPlan(planClear(geometry(app).blocks, { blockId: block.id, at: port.at, isOutlet }));
    },
    onpointerdown: (e) => startLink(app, e, { blockId: block.id, at: port.at, isOutlet }),
  }, el('span', { class: 'dot' }));

  return el('div', { class: `blk-port ${isOutlet ? 'out' : 'in'}${modulated ? ' mod' : ''}` },
    socket,
    el('span', { class: 'blk-port-name' }, port.name,
      port.required && !connected ? el('span', { class: 'required' }, '*') : null),
    connected ? el('span', { class: `blk-port-bus dom-${domain}` }, port.bus) : null);
}

function blockEl(app, block, position) {
  const selected = app.canvas.selected?.kind === 'block' && app.canvas.selected.id === block.id;
  const node = el('div', {
    class: `blk kind-${block.kind}${selected ? ' selected' : ''}${block.bad ? ' bad' : ''}`,
    id: `blk-${block.id}`, 'data-block': block.id,
    style: `left:${position.x}px; top:${position.y}px; height:${blockHeight(block)}px`,
    onpointerdown: (e) => startMove(app, e, block),
  },
    el('div', { class: 'blk-head' },
      block.kind === BlockKind.Node ? el('span', { class: 'blk-index' }, block.index) : null,
      el('span', { class: 'blk-title' }, block.title),
      block.clocked ? el('span', { class: 'blk-clocked', title: 'runs from the master clock' }, '◴') : null,
      iconButton({
        icon: 'close', class: 'blk-remove', label: `remove ${block.title}`,
        onpointerdown: (e) => e.stopPropagation(),
        onclick: (e) => { e.stopPropagation(); app.removeBlock(block); },
      })),
    el('div', { class: 'blk-body' },
      el('div', { class: 'blk-col' }, block.inlets.map((p) => socketRow(app, block, p, false))),
      el('div', { class: 'blk-col right' }, block.outlets.map((p) => socketRow(app, block, p, true)))));
  return node;
}

// --- interaction ------------------------------------------------------------

// Where a client point falls in the canvas's own coordinates, which is what
// every position here is in: undo the pan and the zoom.
function toWorld(app, x, y) {
  const box = app.canvas.viewport?.getBoundingClientRect();
  const view = app.canvas.view;
  if (!box) return { x, y };
  return { x: (x - box.left - view.x) / view.k, y: (y - box.top - view.y) / view.k };
}

// Moving a block. The DOM is written directly while the pointer is down and
// the arrows are redrawn from the moved position, because a render per frame
// would rebuild every card in the page - and would take the focus out of
// whatever was being typed into.
function startMove(app, event, block) {
  if (event.button !== 0 && event.pointerType === 'mouse') return;
  event.stopPropagation();
  const geom = app.canvas.geom;
  const start = geom.positions.get(block.id);
  if (!start) return;
  const element = event.currentTarget;

  drag(event, {
    threshold: MOVE_THRESHOLD,
    onStart: () => element.classList.add('moving'),
    onMove: ({ dx, dy }) => {
      const at = {
        x: Math.max(0, start.x + dx / app.canvas.view.k),
        y: Math.max(0, start.y + dy / app.canvas.view.k),
      };
      geom.positions.set(block.id, at);
      element.style.left = `${at.x}px`;
      element.style.top = `${at.y}px`;
      redrawArrows(app);
    },
    onEnd: ({ moved }) => {
      element.classList.remove('moving');
      if (!moved) { app.select({ kind: 'block', id: block.id }); return; }
      const at = geom.positions.get(block.id);
      app.placeBlock(block.id, at);
    },
  });
}

// Dragging a connection. The ghost follows the pointer; what it lands on is
// whatever socket is under it when it is let go, which is the same hit test a
// finger makes and needs no drop targets registered in advance.
function startLink(app, event, ref) {
  if (event.button !== 0 && event.pointerType === 'mouse') return;
  event.stopPropagation();
  const geom = app.canvas.geom;
  const position = geom.positions.get(ref.blockId);
  const found = portOf(geom.blocks, ref);
  if (!position || !found) return;

  const anchor = socketPoint(position, ref.at, ref.isOutlet);
  const ghost = app.canvas.ghost;
  // While a connection is being dragged the sockets it could land on are the
  // only ones lit: a domain is not a suggestion, the module refuses anything
  // else, and finding that out on release is finding it out too late.
  const viewport = app.canvas.viewport;
  viewport?.classList.add('linking', `only-${domainName(found.port.domain)}`);

  drag(event, {
    onMove: ({ x, y }) => {
      const to = toWorld(app, x, y);
      const line = ref.isOutlet ? curve(anchor, { x: to.x + 9, y: to.y }) : curve(to, anchor);
      ghost.setAttribute('d', line.d);
      ghost.setAttribute('class', `wire ghost dom-${domainName(found.port.domain)}`);
      const over = socketUnder(x, y);
      ghost.classList.toggle('over', Boolean(over) && over.blockId !== ref.blockId);
    },
    onEnd: ({ moved, x, y }) => {
      ghost.setAttribute('d', '');
      viewport?.classList.remove('linking', 'only-gate', 'only-note', 'only-CV');
      if (!moved) { app.select({ kind: 'block', id: ref.blockId }); return; }
      const target = socketUnder(x, y);
      if (target) {
        app.applyPlan(planConnection(geometry(app).blocks, app.device.capabilities, ref, target));
        return;
      }
      // A control signal let go over a *block* rather than a socket is the
      // gesture that makes a modulation route. It cannot be finished here:
      // the block has parameters, not ports, so the editor has to ask which
      // one - which is the whole reason a parameter does not get a socket
      // until something is modulating it (see graph.js).
      const onBlock = blockUnder(x, y);
      if (onBlock && ref.isOutlet && found.port.domain === Domain.CV) {
        askForParameter(app, ref, onBlock, { x, y });
        return;
      }
      app.say(onBlock ? 'only a control signal modulates a parameter' : 'drop on a socket');
    },
  });
}

function blockUnder(x, y) {
  return document.elementFromPoint(x, y)?.closest?.('.blk')?.dataset?.block ?? null;
}

// The dropdown that finishes a modulation drag: every parameter of the block
// it landed on that is not already modulated, in the order the module
// describes them. Closes on a choice, on Escape, or on the next click
// anywhere else - none of which leaves a route behind.
function askForParameter(app, ref, blockId, at) {
  const [kind, where] = String(blockId).split(':');
  if (kind !== BlockKind.Node) {
    app.say('only a node has parameters to modulate');
    return;
  }
  const index = Number(where);
  const choices = modulationChoices(app.device, app.patch, index);
  if (!choices.length) {
    app.say(app.device?.byId.get(app.patch.nodes[index]?.algorithmId)?.params
      ? 'every parameter is already modulated'
      : 'still reading its parameters');
    return;
  }

  closeMenu();
  const menu = el('div', {
    class: 'param-menu', id: 'param-menu', role: 'listbox',
    style: `left:${at.x}px; top:${at.y}px`,
  },
    el('div', { class: 'param-menu-head' }, 'modulate which parameter?'),
    el('div', { class: 'param-menu-list' }, choices.map((choice) => el('button', {
      class: 'param-menu-item', role: 'option',
      onclick: () => {
        closeMenu();
        app.applyPlan(planModulation(geometry(app).blocks, app.patch, app.device.capabilities,
                                     ref, blockId, choice.param, { device: app.device }));
      },
    }, el('span', { class: 'param-menu-name' }, choice.label),
       el('span', { class: 'param-menu-range' }, `${choice.pd.min}–${choice.pd.max}`)))));
  document.body.append(menu);
  menu.querySelector('.param-menu-item')?.focus();

  const dismiss = (e) => {
    if (e.type === 'keydown' && e.key !== 'Escape') return;
    if (e.type === 'pointerdown' && menu.contains(e.target)) return;
    closeMenu();
  };
  menu.dismiss = dismiss;
  // Deferred, so the pointerup that opened this does not immediately close it.
  setTimeout(() => {
    window.addEventListener('pointerdown', dismiss);
    window.addEventListener('keydown', dismiss);
  }, 0);
}

function socketUnder(x, y) {
  const element = document.elementFromPoint(x, y)?.closest?.('.socket');
  if (!element) return null;
  return {
    blockId: element.dataset.block,
    at: Number(element.dataset.at),
    isOutlet: element.dataset.outlet === '1',
  };
}

// Pan with one pointer, zoom with two - and with the wheel, which is what a
// trackpad's pinch arrives as.
function bindViewport(app, viewport, world) {
  const pointers = new Map();
  let pan = null;
  let pinch = null;

  const apply = () => {
    const v = app.canvas.view;
    world.style.transform = `translate(${v.x}px, ${v.y}px) scale(${v.k})`;
  };
  app.canvas.applyView = apply;

  const zoomAbout = (px, py, factor) => {
    const v = app.canvas.view;
    const k = Math.max(MIN_ZOOM, Math.min(MAX_ZOOM, v.k * factor));
    if (k === v.k) return;
    app.canvas.view = { k, x: px - (px - v.x) * (k / v.k), y: py - (py - v.y) * (k / v.k) };
    apply();
  };
  app.canvas.zoomAbout = zoomAbout;

  viewport.addEventListener('pointerdown', (e) => {
    if (e.target.closest('.blk') || e.target.closest('button')) return;
    viewport.setPointerCapture?.(e.pointerId);
    pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
    if (pointers.size === 1) {
      pan = { x: e.clientX, y: e.clientY, view: { ...app.canvas.view }, moved: false };
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
        zoomAbout((a.x + b.x) / 2 - box.left, (a.y + b.y) / 2 - box.top, spread / pinch.spread);
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
    app.canvas.view = { ...app.canvas.view, x: pan.view.x + dx, y: pan.view.y + dy };
    apply();
  });

  const release = (e) => {
    if (!pointers.has(e.pointerId)) return;
    pointers.delete(e.pointerId);
    if (pointers.size < 2) pinch = null;
    if (!pointers.size) {
      viewport.classList.remove('panning');
      // A press on the background that never moved is "nothing is selected",
      // which is how the inspector is put away.
      if (pan && !pan.moved && app.canvas.selected) app.select(null);
      pan = null;
    }
  };
  viewport.addEventListener('pointerup', release);
  viewport.addEventListener('pointercancel', release);

  viewport.addEventListener('wheel', (e) => {
    e.preventDefault();
    const box = viewport.getBoundingClientRect();
    zoomAbout(e.clientX - box.left, e.clientY - box.top, Math.exp(-e.deltaY * 0.002));
  }, { passive: false });
}

// --- drawing ----------------------------------------------------------------

// The arrows, from the positions as they are now. Called on every render and
// on every frame of a block being dragged, so it writes into the paths that
// are already there rather than replacing them.
function redrawArrows(app) {
  const { arrows, positions } = app.canvas.geom;
  for (const arrow of arrows) {
    const path = app.canvas.paths.get(arrow.id);
    if (!path) continue;
    const from = positions.get(arrow.from.blockId);
    const to = positions.get(arrow.to.blockId);
    if (!from || !to) continue;
    const line = curve(socketPoint(from, arrow.from.at, true), socketPoint(to, arrow.to.at, false));
    path.line.setAttribute('d', line.d);
    path.hit.setAttribute('d', line.d);
    if (path.cut) {
      path.cut.style.left = `${line.mid.x}px`;
      path.cut.style.top = `${line.mid.y}px`;
    }
  }
}

function arrowLayer(app, geom) {
  const svg = svgEl('svg', {
    class: 'wires', width: geom.world.width, height: geom.world.height,
    viewBox: `0 0 ${geom.world.width} ${geom.world.height}`,
  });
  svg.append(arrowMarkers());
  app.canvas.svg = svg;
  app.canvas.paths = new Map();

  const cuts = [];
  for (const arrow of geom.arrows) {
    const domain = domainName(arrow.domain);
    const selected = app.canvas.selected?.kind === 'arrow' && app.canvas.selected.id === arrow.id;
    // Two sources on one bus is a merge, which is worth seeing without
    // counting arrowheads. Fan-out - one source, several readers - is the
    // ordinary case and is drawn plainly.
    const merged = arrow.writers > 1;
    const line = svgEl('path', {
      class: `wire dom-${domain}${selected ? ' selected' : ''}${merged ? ' merged' : ''}`,
      'marker-end': `url(#arrow-${domain})`,
      'data-domain': arrow.domain, 'data-bus': arrow.bus, fill: 'none',
    });
    // A four-pixel curve is not something a finger can hit, so every arrow
    // has an invisible one under it that is.
    const hit = svgEl('path', { class: 'wire-hit', fill: 'none' });
    hit.addEventListener('pointerdown', (e) => e.stopPropagation());
    hit.addEventListener('click', () => app.select({ kind: 'arrow', id: arrow.id }));
    const title = svgEl('title');
    title.textContent = `${domain} bus ${arrow.bus}`
      + (arrow.writers > 1 || arrow.readers > 1
        ? ` — ${arrow.writers} writing, ${arrow.readers} reading` : '');
    hit.append(title);

    let cut = null;
    if (selected) {
      cut = iconButton({
        icon: 'cut', class: 'wire-cut', label: 'disconnect this arrow',
        onpointerdown: (e) => e.stopPropagation(),
        onclick: (e) => {
          e.stopPropagation();
          const fresh = geometry(app);
          const same = fresh.arrows.find((a) => a.id === arrow.id);
          app.applyPlan(same ? planDisconnect(fresh.blocks, same) : { ok: false, why: 'already gone' });
        },
      });
      cuts.push(cut);
    }
    svg.append(line, hit);
    app.canvas.paths.set(arrow.id, { line, hit, cut });
  }
  return { svg, cuts };
}

// --- the panel --------------------------------------------------------------

export function canvasPanel(app, computed = null) {
  if (!app.device?.capabilities) {
    return el('p', { class: 'hint' }, 'no module');
  }
  // The patch tab works the geometry out before it builds anything, so that
  // what the bar above the canvas says about the buses is about the patch
  // being drawn below it rather than the one drawn last time.
  const geom = computed ?? geometry(app);
  app.canvas.geom = geom;

  const world = el('div', { class: 'canvas-world' });
  const { svg, cuts } = arrowLayer(app, geom);
  world.append(svg);
  for (const block of geom.blocks) {
    const at = geom.positions.get(block.id);
    if (at) world.append(blockEl(app, block, at));
  }
  world.append(...cuts);
  world.style.width = `${geom.world.width}px`;
  world.style.height = `${geom.world.height}px`;

  const ghost = svgEl('path', { class: 'wire ghost', fill: 'none', d: '' });
  svg.append(ghost);
  app.canvas.ghost = ghost;

  const viewport = el('div', { class: 'canvas-view', role: 'application',
                               'aria-label': 'the patch as blocks and arrows' }, world);
  app.canvas.viewport = viewport;
  bindViewport(app, viewport, world);

  // The view is fitted to the patch when the patch changed under it - loaded,
  // imported, dumped off a module - and left exactly alone otherwise: a canvas
  // that re-centres itself every time a parameter is touched is unusable.
  requestAnimationFrame(() => {
    if (!viewport.isConnected) return;
    if (app.canvas.fit) {
      app.canvas.fit = false;
      app.canvas.view = fitView(geom.world,
        { width: viewport.clientWidth, height: viewport.clientHeight });
    }
    app.canvas.applyView?.();
    redrawArrows(app);
  });

  return el('section', {
    class: 'canvas-panel',
    // The stylesheet is given the same numbers the arrows are drawn from, so a
    // block's rows and its sockets cannot end up in different places.
    style: `--blk-w:${BLOCK_W}px; --blk-head:${HEAD_H}px; `
         + `--blk-row:${ROW_H}px; --blk-pad:${PAD_Y}px`,
  },
    // The bar above the picture is where a patch grows: what to add, and
    // the button that adds it, beside the zoom. It used to be a panel at the
    // foot of the tab, under the detail of whatever was selected, which on a
    // phone was a screen and a half away from the canvas it added to.
    el('div', { class: 'canvas-bar' },
      addBar(app),
      el('div', { class: 'canvas-zoom' },
        iconButton({ icon: 'zoomOut', label: 'zoom out', class: 'ghost',
                     onclick: () => zoomFromButton(app, 1 / 1.25) }),
        iconButton({ icon: 'zoomIn', label: 'zoom in', class: 'ghost',
                     onclick: () => zoomFromButton(app, 1.25) }),
        iconButton({ icon: 'fit', label: 'fit the whole patch in the window', class: 'ghost',
                     onclick: () => { app.canvas.fit = true; app.render(); } }))),
    viewport,
    el('div', { class: 'canvas-foot' },
      geom.blocks.length ? busLegend(app, geom) : el('span', { class: 'hint' }, 'an empty patch'),
      el('span', { class: 'hint' }, capacityLine(app, geom))));
}

// What the module can hold and how much of it this patch has taken, read from
// the device rather than assumed: an app that lets you build a patch the
// module will reject is worse than no app.
function capacityLine(app, geom) {
  const c = app.device.capabilities;
  return `${app.patch.nodes.length}/${c.nodes} nodes · free buses: ${busCapacity(app, geom)}`;
}

function zoomFromButton(app, factor) {
  const viewport = app.canvas.viewport;
  if (!viewport) return;
  app.canvas.zoomAbout?.(viewport.clientWidth / 2, viewport.clientHeight / 2, factor);
}

// Which buses this patch is on, and what each one carries. A bus with two
// writers is a merge - legal, occasionally deliberate, and the thing a patch
// that "plays two sequences at once" usually turns out to be - so it is named
// here rather than left to be inferred from two arrows arriving together.
function busLegend(app, geom) {
  const used = new Map();
  for (const arrow of geom.arrows) {
    const k = `${arrow.domain}:${arrow.bus}`;
    if (!used.has(k)) used.set(k, arrow);
  }
  if (!used.size) return el('span', { class: 'hint' }, 'nothing connected yet');
  const chips = [...used.values()]
    .sort((a, b) => a.domain - b.domain || a.bus - b.bus)
    .map((arrow) => el('span', {
      class: `chip dom-${domainName(arrow.domain)}`,
    }, `${domainName(arrow.domain)} ${arrow.bus}`,
      arrow.writers > 1 ? el('span', { class: 'merge' }, ` ×${arrow.writers}` ) : null));
  return el('div', { class: 'canvas-legend' }, chips);
}

// --- the details panel --------------------------------------------------------

// What was clicked, in full. A node gets the card it always had - every
// parameter, its sequencer grid, its bus selectors - and under it the roll of
// what it read and wrote, because the canvas is a way of *seeing* a patch and
// this is where it is edited in detail.
//
// The panel folds. Its bar carries the block's name, its remove button, a
// chevron that folds the body away and a cross that puts the panel away by
// deselecting - so on a phone, where the panel is below a canvas that fills
// the screen, the card can be folded to a bar without losing the selection
// it belongs to. Whether it is folded is remembered across renders like every
// other disclosure (`App.opened`).
export function canvasInspector(app) {
  const selected = app.canvas.selected;
  if (!selected) return null;
  const open = app.isOpen('details');
  const bar = (title, { before = [], after = [] }) => el('div', { class: 'details-bar' },
    iconButton({ icon: open ? 'down' : 'up', class: 'ghost details-fold',
                 label: open ? 'fold the details away' : 'unfold the details',
                 'aria-expanded': open ? 'true' : 'false',
                 onclick: () => { app.setOpen('details', !open); app.render(); } }),
    ...before,
    el('h2', { class: 'details-title' }, title),
    ...after,
    iconButton({ icon: 'close', class: 'ghost details-close', label: 'close the details',
                 onclick: () => app.select(null) }));
  const panel = (title, extra, ...body) => el('section', {
    class: `panel details ${open ? '' : 'folded'}`,
  }, bar(title, extra), open ? body : null);

  if (selected.kind === 'arrow') {
    const arrow = app.canvas.geom?.arrows.find((a) => a.id === selected.id);
    if (!arrow) return el('p', { class: 'hint' }, 'gone');
    const from = app.canvas.geom.blocks.find((b) => b.id === arrow.from.blockId);
    const to = app.canvas.geom.blocks.find((b) => b.id === arrow.to.blockId);
    const domain = domainName(arrow.domain);
    const disconnect = () => {
      const fresh = geometry(app);
      const same = fresh.arrows.find((a) => a.id === arrow.id);
      app.applyPlan(same ? planDisconnect(fresh.blocks, same) : { ok: false, why: 'already gone' });
    };
    return panel(`${domain} bus ${arrow.bus}`,
      { before: [el('span', { class: `chip dom-${domain}` }, domain)],
        after: [iconButton({ icon: 'cut', label: 'disconnect this arrow', class: 'ghost danger',
                             onclick: disconnect })] },
      el('p', {}, `${from?.title ?? '?'} ${from ? portLabel(from, arrow.from.at, true) : ''} → `
                + `${to?.title ?? '?'} ${to ? portLabel(to, arrow.to.at, false) : ''}`),
      arrow.writers > 1 || arrow.readers > 1
        ? el('p', { class: 'hint' }, `${arrow.writers} writing · ${arrow.readers} reading`)
        : null);
  }

  const block = app.canvas.geom?.blocks.find((b) => b.id === selected.id);
  if (!block) return el('p', { class: 'hint' }, 'gone');
  const remove = iconButton({ icon: 'trash', label: `remove ${block.title}`, class: 'ghost danger',
                              onclick: () => app.removeBlock(block) });
  if (block.kind === BlockKind.Node) {
    const d = app.device.byId.get(app.patch.nodes[block.index]?.algorithmId);
    return panel(block.title,
      { before: [el('span', { class: 'node-index' }, block.index)],
        after: [d?.wantsTick ? el('span', { class: 'tag' }, 'clocked') : null, remove] },
      el('div', { class: 'nodes' }, nodeCard(app, block.index, { header: false })),
      nodeRollPanel(app, block.index));
  }
  if (block.kind === BlockKind.Jack) return panel(block.title, { after: [remove] }, jackCard(app, block.index));
  // The port's own head is left out: the panel's bar already carries its name
  // and its remove button, and a card drawing them again would put two remove
  // buttons on one port.
  return panel(block.title, { after: [remove] },
               routeCard(app, block.index, block.kind === BlockKind.MidiOut, { header: false }));
}

const portLabel = (block, at, isOutlet) =>
  (isOutlet ? block.outlets : block.inlets).find((p) => p.at === at)?.name ?? '';

// --- what is live -----------------------------------------------------------

// A gate bus is high or it is not, once per pass, and an arrow carrying one
// lights when it is. Same sampling the gate dots and the scope use: read at
// the paint, a trigger is missed about forty-nine times in fifty.
export function refreshCanvasLive(app, live) {
  if (app.tab !== 'patch' || !app.canvas?.paths?.size) return;
  for (const [, path] of app.canvas.paths) {
    const domain = Number(path.line.getAttribute('data-domain'));
    if (domain !== Domain.Gate) continue;
    const bus = Number(path.line.getAttribute('data-bus'));
    path.line.classList.toggle('lit', (live.gate & (1 << bus)) !== 0);
  }
}

// --- adding ----------------------------------------------------------------

// What can be put on the canvas besides an algorithm: the two things a patch
// has that are not one. These are the patch's edges, and a canvas that could
// not add them would send you to another tab to finish a patch you started
// here.
//
// **A direction is not a kind of block.** There used to be four entries here -
// a jack each way and a MIDI port each way - so "in or out" was a decision you
// made in the list of things to add, before you had the thing, and changing
// your mind meant deleting a block and adding its opposite. A jack is a jack;
// which way it faces is one of its settings, and it is set where its other
// settings are (`jackCard`, `routeCard`). What is added is the port, pointing
// the way most patches want it: a jack listens, a MIDI port plays.
export const ENDPOINTS = [
  { key: 'jack', label: 'jack', kind: BlockKind.Jack,
    direction: P.GatePortDirection.GATE_PORT_IN, note: 'gate',
    hint: 'One of the module’s gate jacks. In or out is a toggle on the jack itself.' },
  { key: 'midi', label: 'MIDI port', kind: BlockKind.MidiIn, note: 'notes',
    hint: 'A cable’s worth of notes, on a note bus. In or out is a toggle on the port itself.' },
];

export function addBar(app) {
  if (!app.device?.algorithms?.length) {
    return el('div', { class: 'hint' }, 'no module');
  }
  // Shelved by what each algorithm *is*, which the module says itself - so a
  // list of thirty is six short lists of the kind of thing you came looking
  // for, and an algorithm added to the firmware still arrives on a shelf.
  const groups = catalogue(app.device.algorithms, ENDPOINTS);
  if (!optionFor(groups, app.addPick)) app.addPick = optionsOf(groups)[0]?.value ?? null;

  return el('div', { class: 'add-row' },
    el('div', { class: 'grow' }, richSelect({
      value: app.addPick, groups, label: 'what to add',
      onPick: (value) => { app.addPick = value; app.render(); },
    })),
    iconButton({ icon: 'plus', label: 'add it to the patch', class: 'primary',
                 onclick: () => app.add(app.addPick) }));
}

// The free buses left, so "add" and a drag both stop being possible for a
// reason a user can see coming.
export function busCapacity(app, geom) {
  const caps = app.device?.capabilities;
  if (!caps) return null;
  const parts = [];
  for (const domain of [Domain.Gate, Domain.Note, Domain.CV]) {
    const written = new Set();
    for (const block of geom.blocks) {
      for (const port of block.outlets) {
        if (port.domain === domain && port.bus !== P.NO_BUS) written.add(port.bus);
      }
    }
    parts.push(`${domainName(domain)} ${busCount(caps, domain) - written.size}/${busCount(caps, domain)}`);
  }
  return parts.join(' · ');
}
