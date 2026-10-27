// The Launchpad, drawn: the grid, the buttons around it and the logo, lit as
// the module has lit them, and pressable.
//
// **It is a picture of the wire.** Every colour here is a palette index the
// module sent in a SysEx frame (runtime/launchpad.js), never a guess at what
// a page should look like: the note page's roots are pink because the
// firmware painted them pink, and a fader follows a parameter because the
// module redrew it. A press sends what a pad sends in programmer mode, and
// what the module makes of it comes back as LEDs. So the drawing on this tab
// and a Launchpad on the module's socket show the same thing, and when a
// real one is bridged in they are the same bytes.
//
// Painted per frame, like the lamps and the scope (services/render.js): a
// hundred LEDs, each a background colour, is cheaper than rebuilding
// anything, and a render would drop a finger mid-press.

import * as P from '../../protocol/generated.js';
import { el, classes } from '../dom.js';
import { MODELS, gridId, topId, sideId, leftId, bottomId, LOGO_ID, isGridId } from '../../runtime/launchpad.js';
import './Launchpad.css';

const GRID = 8;
// A Pro's upper bottom row picks a rack slot, one button each.
const RACK_SLOTS = P.N_RACK_SLOT;

// What a button around the grid is for, as the firmware lays it out
// (src/surface/launchpad.h): named here only so a screen reader and a
// tooltip can say it. The lights say the rest.
const TOP = ['up', 'down', 'left', 'right', 'key page', 'note page', 'widgets page', 'tempo page'];
const SIDE = ['start / stop', 'continue', 'tap tempo', 'panic', 'audio page', '', '', 'empty the slot (audio page)'];

export function LaunchpadView(app, launchpad) {
  const spec = MODELS[launchpad.model];
  const cells = [];
  const cell = (id, { kind, label }) => {
    const node = el('button', {
      type: 'button', class: classes('lp-cell', kind), 'data-id': String(id),
      'aria-label': label, title: label, disabled: kind === 'inert' ? 'true' : null,
    });
    if (kind !== 'inert') gesture(node, id, launchpad);
    cells.push([id, node]);
    return node;
  };
  const columns = GRID + 1 + (spec.left ? 1 : 0);

  const rows = [];
  // The top row: the arrows and the pages, and the logo in the corner.
  rows.push(
    spec.left ? el('span', { class: 'lp-gap' }) : null,
    Array.from({ length: GRID }, (_, c) => cell(topId(c), { kind: 'round', label: TOP[c] })),
    cell(LOGO_ID, { kind: 'logo', label: 'the logo: lit while the module has the Launchpad' }));
  // The grid, top row first, each with the right column's button beside it.
  for (let row = GRID - 1; row >= 0; row--) {
    rows.push(
      spec.left ? cell(leftId(row), { kind: 'inert', label: 'a Pro button the module does not use' }) : null,
      Array.from({ length: GRID }, (_, c) => cell(gridId(row, c), { kind: 'grid', label: `pad row ${row + 1}, column ${c + 1}` })),
      cell(sideId(row), { kind: 'round', label: SIDE[7 - row] || `right column ${row + 1}` }));
  }
  if (spec.bottom) {
    rows.push(
      el('span', { class: 'lp-gap' }),
      Array.from({ length: GRID }, (_, c) => cell(bottomId(c), c < RACK_SLOTS
        ? { kind: 'round', label: `audio slot ${c + 1}` }
        : { kind: 'inert', label: 'a Pro button the module does not use' })),
      el('span', { class: 'lp-gap' }));
  }

  const board = el('div', {
    class: classes('lp-board', `lp-${launchpad.model}`, !launchpad.programmer && 'lp-live'),
    style: `--lp-columns: ${columns}`, role: 'group', 'aria-label': spec.label,
  }, rows);

  // Every LED, every frame, from what the module last sent.
  app.live.paint(() => {
    board.classList.toggle('lp-live', !launchpad.programmer);
    for (const [id, node] of cells) {
      const lit = launchpad.lit(id);
      node.style.setProperty('--led', lit ? launchpad.colour(id) : 'transparent');
      node.classList.toggle('lit', lit);
    }
  });

  return board;
}

// Press to play, release to stop: a pad held is a note held, as on the
// device. A click with no pointer - a key - presses and lets go, so a toggle
// still toggles and a note does not hang.
function gesture(node, id, launchpad) {
  let down = false;
  node.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    if (down) return;
    down = true;
    node.classList.add('down');
    launchpad.press(id, isGridId(id) ? 100 : 127);
  });
  const up = () => {
    if (!down) return;
    down = false;
    node.classList.remove('down');
    launchpad.release(id);
  };
  for (const type of ['pointerup', 'pointercancel', 'pointerleave']) node.addEventListener(type, up);
  node.addEventListener('contextmenu', (e) => e.preventDefault());
  node.addEventListener('click', (e) => {
    if (e.detail !== 0) return;
    launchpad.press(id, isGridId(id) ? 100 : 127);
    setTimeout(() => launchpad.release(id), 120);
  });
}
