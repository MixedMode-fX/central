// Rebuilding the page, and painting what moves without rebuilding it.
//
// The page is rebuilt wholesale after every edit, on a microtask. Replacing
// the tree removes whatever has focus, and removing a focused input fires
// `change` during the replacement - which used to call render() again from
// inside the render that was already running. So a render asked for while one
// is running is deferred rather than run inside it.
//
// The lights, the playheads, the scope and the meters are written straight
// into the DOM once per animation frame instead: re-rendering the page for a
// blinking LED would fight every open <select> and every held key. A view
// that has something live registers a painter while it is built, and the
// registry is emptied when the page is rebuilt, so a painter never outlives
// the elements it closed over.

export class Live {
  constructor() {
    this.painters = [];
    this.mounted = [];
    // The modulation slots with a meter on screen: what the session polls.
    this.modSlots = new Set();
  }

  reset() {
    this.painters.length = 0;
    this.mounted.length = 0;
    this.modSlots.clear();
  }

  // Called every animation frame, and once after every render, with what the
  // module has done since the last paint.
  paint(fn) { this.painters.push(fn); }

  // Called once, after the tree it was registered from is in the document.
  onMount(fn) { this.mounted.push(fn); }

  watchMod(slot) { this.modSlots.add(slot); }

  tick(frame) {
    for (const painter of this.painters) painter(frame);
  }
}

export class Renderer {
  constructor({ root, view, live, onPainted = () => {} }) {
    this.root = root;
    this.view = view;
    this.live = live;
    this.onPainted = onPainted;
    this.scheduled = false;
  }

  render() {
    if (this.scheduled) return;
    this.scheduled = true;
    queueMicrotask(() => {
      this.scheduled = false;
      this.paint();
    });
  }

  paint() {
    this.live.reset();
    this.root.replaceChildren(this.view());
    for (const fn of this.live.mounted) fn();
    this.onPainted();
  }
}
