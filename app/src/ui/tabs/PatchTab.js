// The patch, drawn. The canvas answers "what feeds what", which the bus
// model otherwise hides in a dozen selectors reading "gate bus 2", and the
// block that is selected gets the whole of its detail in the panel below the
// picture. Under that, the mod matrix and the macro bench: a binding, a route
// and a macro are all part of the patch, so they are read under the graph
// they act on.
//
// **Full screen is the same two panels and nothing else.** A patch big enough
// to want the whole window is one whose picture is the work, so the meters,
// the matrix and the bench are left out rather than scrolled past - and the
// inspector comes along, because a block you cannot open is a block you can
// only look at.

import { el } from '../dom.js';
import { Hint } from '../components/Panel.js';
import { Meters } from '../panels/Meters.js';
import { ModMatrix } from '../panels/ModMatrix.js';
import { Macros } from '../panels/Macros.js';
import { CanvasPanel, geometry } from '../canvas/Canvas.js';
import { Inspector } from '../canvas/Inspector.js';

export function PatchTab(app) {
  if (!app.device?.capabilities) return Hint('no module');
  // Worked out once, before anything is built from it: the foot of the
  // canvas counts the free buses in the patch below it, not the one drawn
  // before this edit.
  const geom = geometry(app);
  if (app.state.ui.canvas.full) {
    return el('div', { class: 'canvas-full' }, CanvasPanel(app, geom), Inspector(app, geom));
  }
  return el('div', {}, Meters(app), CanvasPanel(app, geom), Inspector(app, geom),
    ModMatrix(app), Macros(app));
}
