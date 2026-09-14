// Where the blocks sit on the canvas, for the patch being edited.
//
// A position is not part of a patch (core/layout.js): a patch with none is
// laid out from its own shape, and a block somebody dragged is remembered in
// this browser, beside the library, under the patch's identity.

import { forgetNode } from '../core/layout.js';

// A patch nobody has named yet is "working", the same identity the autosave
// uses, and its arrangement follows it into the library when it is saved.
const WORKING = 'working';

export class Arrangement {
  constructor({ state, library }) {
    this.state = state;
    this.library = library;
  }

  get positions() { return this.state.ui.canvas.positions; }

  key() { return this.state.current.id ?? WORKING; }

  // The patch on screen was replaced by one nobody typed - loaded, imported,
  // dumped off a module. Take that patch's own arrangement, put the view back
  // over the whole of it, and select nothing.
  reset() {
    const canvas = this.state.ui.canvas;
    canvas.selected = null;
    canvas.fit = true;
    canvas.positions = this.library.layoutFor(this.key()) ?? {};
  }

  remember(positions) {
    this.state.ui.canvas.positions = positions;
    this.library.saveLayout(this.key(), positions);
  }

  // A block dragged somewhere.
  place(id, at) {
    this.remember({ ...this.positions, [id]: [Math.round(at.x), Math.round(at.y)] });
  }

  // Node `index` is gone, and the nodes after it are renumbered.
  forgetNode(index) {
    this.remember(forgetNode(this.positions, index));
  }

  // The patch has just been given an identity in the library.
  follow(id) { this.library.moveLayout(this.key(), id); }

  drop(id) { this.library.dropLayout(id); }
}
