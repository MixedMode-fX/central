// Everything the page shows, in one object. The patch and its globals are
// the module's; `current` says where the patch came from; `ui` is how the
// page is being looked at, none of which is part of a patch.
//
// The state is mutated in place and the page is rebuilt after: a node's
// parameters are a byte array the firmware's layout defines, and a plan from
// `core/graph.js` writes straight into it. What keeps that honest is that
// only the services in this directory write state, and every write ends in
// `render()`.

import * as codec from '../protocol/codec.js';
import * as P from '../protocol/generated.js';

export const UNTITLED = 'untitled';

export const noPatch = () => ({ id: null, name: UNTITLED, dirty: false, savedAt: 0 });

export function createState() {
  return {
    patch: codec.emptyPatch(),
    globals: codec.emptyGlobals(),
    // The patch being edited: its name, and where it came from in the library.
    current: noPatch(),
    // The image as last saved, for the dirty mark.
    savedImage: null,
    status: 'starting the module…',
    // Something that went wrong, shown until dismissed.
    error: null,
    // The module has not got this patch. An incremental edit addresses a node
    // by index, so once the two disagree about the graph's shape every one of
    // them is a message about a node the module does not have. See
    // `Editor.edit`.
    diverged: false,
    ui: {
      tab: 'patch',
      // Where "play" came from, so leaving it goes back rather than guessing.
      editingTab: 'patch',
      // Which disclosure sections are open. The page is rebuilt wholesale on
      // every edit, so anything the DOM remembers by itself is lost unless
      // the app remembers it. The details panel starts open.
      opened: new Set(['details']),
      // Where each horizontal scroller had been scrolled to, by key.
      scrolled: new Map(),
      // Traces put away for a moment, by pressing their chip in a legend.
      scopeAll: false,
      scopeHidden: new Set(),
      rollHidden: new Set(),
      // Per harmony node: which chord the arrows come from, and which of the
      // two pictures is up.
      harmony: new Map(),
      // What the add bar and the example list are set to.
      addPick: null,
      example: null,
      // Whether the prompt on the schema tab carries the patch on screen.
      schemaWithPatch: true,
      // What the on-screen keyboard sends.
      play: { port: P.MidiPort.mmMIDI_USB_0, channel: 1, velocity: 100, octave: 4, cc: 74, ccValue: 64 },
      // The performance surface: whether it is in edit mode, which control is
      // being assigned, which one is waiting for the module's learn, and
      // whether the keyboard is summoned over it. None of it is the surface
      // *document* - that is the performer's and lives in localStorage
      // (services/surface.js).
      surface: { edit: false, editing: null, armed: null, keyboard: false },
      // The canvas: where it is looked at from, what is selected on it, and
      // whether the view should be fitted to the patch on the next render.
      canvas: { view: { x: 0, y: 0, k: 1 }, selected: null, fit: true, positions: {} },
    },
  };
}

// A named block on the canvas, selected.
export const selectBlock = (id) => ({ kind: 'block', id });
