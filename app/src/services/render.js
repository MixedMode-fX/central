// Rebuilding the page, and painting what moves without rebuilding it.
//
// The page is rebuilt wholesale after every edit, on a microtask. Replacing
// the tree removes whatever has focus, and removing a focused input fires
// `change` during the replacement - which used to call render() again from
// inside the render that was already running. So a render asked for while one
// is running is deferred rather than run inside it.
//
// The lights, the playheads, the scope and the meters are written straight
// into the DOM once per frame the module reports instead
// (`runtime/monitor.js`): re-rendering the page for a blinking LED would
// fight every open <select> and every held key. A view that has something
// live registers a painter while it is built, and the registry is emptied
// when the page is rebuilt, so a painter never outlives the elements it
// closed over.
//
// A rebuild is the longest thing this page does on its main thread, and the
// module runs on the same thread: for as long as the rebuild takes, no pass
// runs. So before one starts, the module is run ahead by what the last few
// rebuilds cost (`before`), and the passes the rebuild would have held up have
// already happened - with their notes scheduled where they belong, because a
// note is heard at its simulated time and not at the moment it was computed.
// That is what makes an edit inaudible.

export class Live {
  constructor() {
    this.painters = [];
    this.mounted = [];
    // The modulation slots with a meter on screen: what the session polls.
    this.modSlots = new Set();
    // The macros with a control on screen, for the same reason: a macro's
    // position is not in the patch, so the only way to draw a pot where the
    // module has it is to ask - and only about the ones being looked at.
    this.macroSlots = new Set();
    // The globals with an indicator on screen, by the name the settings give
    // them (protocol/names.js, LIVE_GLOBALS): the key a Key node has moved
    // the patch to, the tempo a tap has changed. Same rule again - a setting
    // nobody is looking at is a round trip nobody needs.
    this.globalNames = new Set();
    // The nodes with a playhead on screen: what the monitor request asks
    // the position of. A card that is closed is a node nobody is watching.
    this.nodeSlots = new Set();
  }

  reset() {
    this.painters.length = 0;
    this.mounted.length = 0;
    this.modSlots.clear();
    this.macroSlots.clear();
    this.globalNames.clear();
    this.nodeSlots.clear();
  }

  // Called every animation frame, and once after every render, with what the
  // module has done since the last paint.
  paint(fn) { this.painters.push(fn); }

  // Called once, after the tree it was registered from is in the document.
  onMount(fn) { this.mounted.push(fn); }

  watchMod(slot) { this.modSlots.add(slot); }

  watchMacro(index) { this.macroSlots.add(index); }

  watchGlobal(name) { this.globalNames.add(name); }

  watchNode(index) { this.nodeSlots.add(index); }

  tick(frame) {
    for (const painter of this.painters) painter(frame);
  }
}

// The margin a rebuild is run ahead by, over what the last ones cost: the
// browser lays the new tree out after this code has measured itself, and the
// estimate is a decaying maximum rather than a mean because the cost to cover
// is the next rebuild's, and the next one is as likely as not the expensive
// tab. Bounded by the module (`EmbeddedModule.runAhead`).
const COST_MARGIN = 1.5;
const COST_FLOOR_MS = 8;
const COST_DECAY = 0.8;

export class Renderer {
  constructor({ root, view, live, onPainted = () => {}, before = () => {} }) {
    this.root = root;
    this.view = view;
    this.live = live;
    this.onPainted = onPainted;
    this.before = before;
    this.scheduled = false;
    // What a rebuild has been costing, in milliseconds of this thread.
    this.cost = 0;
  }

  render() {
    if (this.scheduled) return;
    this.scheduled = true;
    queueMicrotask(() => {
      this.scheduled = false;
      this.paint();
    });
  }

  // How far ahead the module is run before the next rebuild.
  runAheadMs() { return this.cost ? this.cost * COST_MARGIN + COST_FLOOR_MS : 0; }

  paint() {
    this.before(this.runAheadMs());
    const t0 = performance.now();
    this.live.reset();
    this.root.replaceChildren(this.view());
    for (const fn of this.live.mounted) fn();
    this.onPainted();
    this.cost = Math.max(performance.now() - t0, this.cost * COST_DECAY);
  }
}
