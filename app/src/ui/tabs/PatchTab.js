// The patch, drawn. The canvas answers "what feeds what", which the bus
// model otherwise hides in a dozen selectors reading "gate bus 2", and the
// block that is selected gets the whole of its detail in the panel below the
// picture. Under that, the mod matrix: a binding and a route are part of the
// patch, so they are read under the graph they act on.

import { el } from '../dom.js';
import { Hint } from '../components/Panel.js';
import { Meters } from '../panels/Meters.js';
import { ModMatrix } from '../panels/ModMatrix.js';
import { CanvasPanel, geometry } from '../canvas/Canvas.js';
import { Inspector } from '../canvas/Inspector.js';

export function PatchTab(app) {
  if (!app.device?.capabilities) return Hint('no module');
  // Worked out once, before anything is built from it: the foot of the
  // canvas counts the free buses in the patch below it, not the one drawn
  // before this edit.
  const geom = geometry(app);
  return el('div', {}, Meters(app), CanvasPanel(app, geom), Inspector(app, geom), ModMatrix(app));
}
