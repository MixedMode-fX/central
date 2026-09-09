// Where a block sits, and where its sockets are.
//
// **A position is not part of a patch.** The image the module stores, the
// `.syx` file and the JSON dialect all describe a graph and say nothing about
// a canvas, and adding a coordinate to any of them would be a change to the
// firmware's format for the benefit of one view in one app. So the canvas lays
// a patch out from its *shape* - which is what makes a patch imported from a
// file, dumped off a module or pasted from a chat readable the moment it
// opens - and a block dragged somewhere by hand is remembered beside the
// patch, in this browser, as a preference about looking at it.
//
// The geometry is arithmetic rather than measurement: a socket's position is
// computed from the same constants the stylesheet is given, so an arrow is
// drawn where its socket is without reading anything back out of the DOM. That
// is what lets the layout be tested with no browser in the room, and it is why
// the numbers below are exported rather than written twice.

const NO_COLUMN = -1;

export const BLOCK_W = 210;   // a block's width
export const HEAD_H = 40;     // its title bar
export const ROW_H = 26;      // one socket row
export const PAD_Y = 10;      // above the first row and below the last
export const GAP_X = 92;      // between columns: room for an arrow to bend in
export const GAP_Y = 26;      // between blocks in a column
export const MARGIN = 28;     // around the whole thing

export function blockHeight(block) {
  const rows = Math.max(block.inlets.length, block.outlets.length, 1);
  return HEAD_H + PAD_Y * 2 + rows * ROW_H;
}

// The point an arrow leaves from or arrives at: the outer edge of the block,
// level with the middle of the socket's own row.
export function socketPoint(position, at, isOutlet) {
  return {
    x: position.x + (isOutlet ? BLOCK_W : 0),
    y: position.y + HEAD_H + PAD_Y + at * ROW_H + ROW_H / 2,
  };
}

// Signal flows left to right: a block sits one column to the right of the
// furthest-right thing that writes to it. Everything with nothing feeding it -
// a MIDI input, a jack, a clock, a node whose inlets are all empty - starts at
// the left edge.
//
// A patch may contain a cycle: a bus is read and written once per pass, so a
// sequencer resetting the divider that advances it is a legal and useful
// patch, not a mistake to refuse. A cycle has no "furthest-right" answer, so
// the walk stops when it meets a block it is already inside and takes what the
// rest of the path gives - which puts the loop's members side by side instead
// of hanging the layout.
function columnOf(blocks, incoming) {
  const column = new Map();
  const visiting = new Set();

  const walk = (id) => {
    if (column.has(id)) return column.get(id);
    if (visiting.has(id)) return NO_COLUMN;        // a cycle: this edge tells us nothing
    visiting.add(id);
    let deepest = NO_COLUMN;
    for (const from of incoming.get(id) ?? []) deepest = Math.max(deepest, walk(from));
    visiting.delete(id);
    const at = deepest + 1;
    column.set(id, at);
    return at;
  };

  for (const block of blocks) walk(block.id);
  return column;
}

// Down a column, blocks sit under the things that feed them: each one is
// ordered by the average position of its sources in the column to its left, so
// two chains running side by side stay side by side instead of crossing. A
// block with no sources keeps the order the patch stores it in, which is the
// order it was added.
export function autoLayout(blocks, arrows) {
  const incoming = new Map();
  for (const block of blocks) incoming.set(block.id, []);
  for (const arrow of arrows) {
    if (arrow.from.blockId === arrow.to.blockId) continue;
    incoming.get(arrow.to.blockId)?.push(arrow.from.blockId);
  }

  const column = columnOf(blocks, incoming);
  const columns = [];
  blocks.forEach((block, order) => {
    const at = column.get(block.id) ?? 0;
    (columns[at] ??= []).push({ block, order });
  });

  const positions = new Map();
  const rank = new Map();               // where a block ended up in its column
  columns.forEach((members, at) => {
    if (at > 0) {
      members.sort((a, b) => {
        const pull = (entry) => {
          const sources = (incoming.get(entry.block.id) ?? [])
            .map((id) => rank.get(id)).filter((r) => r !== undefined);
          return sources.length ? sources.reduce((s, r) => s + r, 0) / sources.length : Infinity;
        };
        return (pull(a) - pull(b)) || (a.order - b.order);
      });
    }
    let y = MARGIN;
    members.forEach((entry, row) => {
      positions.set(entry.block.id, { x: MARGIN + at * (BLOCK_W + GAP_X), y });
      rank.set(entry.block.id, row);
      y += blockHeight(entry.block) + GAP_Y;
    });
  });
  return positions;
}

// The automatic layout, with anything a user has dragged put back where they
// put it. A saved position for a block that is no longer in the patch is
// ignored rather than deleted: undoing a deletion by loading the patch again
// should not also lose where everything was.
export function layoutOf(blocks, arrows, saved) {
  const positions = autoLayout(blocks, arrows);
  if (!saved) return positions;
  for (const block of blocks) {
    const at = saved[block.id];
    if (Array.isArray(at) && Number.isFinite(at[0]) && Number.isFinite(at[1])) {
      positions.set(block.id, { x: at[0], y: at[1] });
    }
  }
  return positions;
}

// How big the canvas has to be to hold all of it.
export function worldSize(blocks, positions) {
  let width = 0;
  let height = 0;
  for (const block of blocks) {
    const at = positions.get(block.id);
    if (!at) continue;
    width = Math.max(width, at.x + BLOCK_W);
    height = Math.max(height, at.y + blockHeight(block));
  }
  return { width: width + MARGIN, height: height + MARGIN };
}

// The pan and zoom that puts the whole patch in the window, never magnified
// past life size - a two-node patch blown up to fill a laptop screen looks
// broken, not welcoming.
export function fitView(world, viewport, { max = 1 } = {}) {
  if (!world.width || !world.height || !viewport.width || !viewport.height) {
    return { x: 0, y: 0, k: 1 };
  }
  const k = Math.min(max, viewport.width / world.width, viewport.height / world.height);
  return {
    k,
    x: (viewport.width - world.width * k) / 2,
    y: (viewport.height - world.height * k) / 2,
  };
}

// Removing node `index` renumbers every node after it, so the positions have
// to move with them or the patch rearranges itself when a block is deleted.
// Exactly the reason `App.removeNode` renumbers the controller bindings.
export function forgetNode(saved, index) {
  const moved = {};
  for (const [id, at] of Object.entries(saved ?? {})) {
    const match = /^node:(\d+)$/.exec(id);
    if (!match) { moved[id] = at; continue; }
    const was = Number(match[1]);
    if (was === index) continue;
    moved[was > index ? `node:${was - 1}` : id] = at;
  }
  return moved;
}
