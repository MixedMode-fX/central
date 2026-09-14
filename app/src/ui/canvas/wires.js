// The arrows: their shape, and the markers that end them.

import { svg } from '../dom.js';

// An arrow leaves an outlet heading right and arrives at an inlet heading
// right, always: the handles are horizontal, so the curve says which way the
// signal goes before the arrowhead does. A connection that runs backwards up
// the patch bows out further rather than doubling back through the blocks.
export function curve(from, to) {
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

// One arrowhead per domain, in the domain's colour.
export function arrowMarkers() {
  return svg('defs', {}, [['gate', 'var(--gate)'], ['note', 'var(--note)'], ['CV', 'var(--cv)']]
    .map(([name, colour]) => svg('marker', {
      id: `arrow-${name}`, viewBox: '0 0 10 10', refX: 10, refY: 5,
      markerWidth: 6, markerHeight: 6, orient: 'auto-start-reverse',
    }, svg('path', { d: 'M 0 0 L 10 5 L 0 10 z', fill: colour }))));
}

// One drag, on the window, so a pointer that leaves the element it started
// on keeps being followed - which is every drag that matters here, since a
// connection is made by leaving one block and arriving at another.
export function drag(event, { threshold = 0, onStart, onMove, onEnd } = {}) {
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
