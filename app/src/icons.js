// The icons: one inline SVG each, drawn in `currentColor` so a button's state
// is a colour on the button and nothing more.
//
// A word on a button is read; an icon is recognised. On a phone the difference
// is the width of the row: "disconnect", "zoom out" and "remove" are each a
// third of the screen, and three of them do not fit beside the thing they act
// on. Every icon button still carries its word - in `title` for a pointer and
// `aria-label` for a screen reader - so nothing is lost but the pixels.
//
// This module builds SVG directly rather than through `el` in views.js, which
// imports it: the bundler refuses a cycle, and it is right to.

const SVG_NS = 'http://www.w3.org/2000/svg';

// Stroked paths unless `fill` says otherwise, 24 units square.
const ICONS = {
  play: { d: 'M8 5v14l11-7z', fill: true },
  stop: { d: 'M6 6h12v12H6z', fill: true },
  resume: { d: 'M6 5v14 M10 5l10 7-10 7z' },
  edit: { d: 'M4 20h4L18 10l-4-4L4 16z M13 7l4 4' },
  plug: { d: 'M9 3v5 M15 3v5 M6 8h12v3a6 6 0 0 1-12 0z M12 17v4' },
  zoomIn: { d: 'M9 3a6 6 0 1 1 0 12 6 6 0 0 1 0-12z M14 14l6 6 M9 6v6 M6 9h6' },
  zoomOut: { d: 'M9 3a6 6 0 1 1 0 12 6 6 0 0 1 0-12z M14 14l6 6 M6 9h6' },
  fit: { d: 'M4 9V4h5 M15 4h5v5 M20 15v5h-5 M9 20H4v-5' },
  trash: { d: 'M4 7h16 M9 7V4h6v3 M6 7l1 13h10l1-13 M10 11v6 M14 11v6' },
  cut: { d: 'M6 3a3 3 0 1 1 0 6 3 3 0 0 1 0-6z M6 15a3 3 0 1 1 0 6 3 3 0 0 1 0-6z '
         + 'M8.5 8.5L20 20 M8.5 15.5L20 4' },
  plus: { d: 'M12 5v14 M5 12h14' },
  close: { d: 'M6 6l12 12 M18 6L6 18' },
  down: { d: 'M6 9l6 6 6-6' },
  up: { d: 'M6 15l6-6 6 6' },
  cv: { d: 'M3 12c2-7 4-7 6 0s4 7 6 0 4-7 6 0' },
  patch: { d: 'M3 5h6v5H3z M15 14h6v5h-6z M9 7.5h3v9h3' },
  key: { d: 'M9 17V5l10-2v12 M9 17a2 2 0 1 1-4 0 2 2 0 0 1 4 0 M19 15a2 2 0 1 1-4 0 2 2 0 0 1 4 0' },
  library: { d: 'M4 4h12a2 2 0 0 1 2 2v14H6a2 2 0 0 1-2-2z M4 17a2 2 0 0 1 2-2h12' },
  schema: { d: 'M8 4c-2 0-3 1-3 3v3c0 1-1 2-2 2 1 0 2 1 2 2v3c0 2 1 3 3 3 '
             + 'M16 4c2 0 3 1 3 3v3c0 1 1 2 2 2-1 0-2 1-2 2v3c0 2-1 3-3 3' },
  copy: { d: 'M9 9h11v11H9z M5 15V5h10' },
  download: { d: 'M12 4v11 M7 10l5 5 5-5 M4 19h16' },
  upload: { d: 'M12 15V4 M7 9l5-5 5 5 M4 19h16' },
  clock: { d: 'M12 4a8 8 0 1 1 0 16 8 8 0 0 1 0-16z M12 8v4l3 2' },
  erase: { d: 'M4 15l9-9 6 6-6 6H8z M4 21h16' },
  clear: { d: 'M4 12l6-6h9v12h-9z M12 9l4 6 M16 9l-4 6' },
};

export function icon(name) {
  if (name === 'midi') return midiIcon();
  const spec = ICONS[name];
  if (!spec) throw new Error(`no icon called ${name}`);
  const node = document.createElementNS(SVG_NS, 'svg');
  node.setAttribute('viewBox', '0 0 24 24');
  node.setAttribute('class', 'icon');
  node.setAttribute('aria-hidden', 'true');
  node.setAttribute('focusable', 'false');
  const path = document.createElementNS(SVG_NS, 'path');
  path.setAttribute('d', spec.d);
  if (spec.fill) {
    path.setAttribute('fill', 'currentColor');
  } else {
    path.setAttribute('fill', 'none');
    path.setAttribute('stroke', 'currentColor');
    path.setAttribute('stroke-width', '1.8');
    path.setAttribute('stroke-linecap', 'round');
    path.setAttribute('stroke-linejoin', 'round');
  }
  node.append(path);
  return node;
}

// A five-pin DIN, which is the one glyph a musician already reads as "a
// controller plugs in here": the learn button.
export function midiIcon() {
  const node = document.createElementNS(SVG_NS, 'svg');
  node.setAttribute('viewBox', '0 0 24 24');
  node.setAttribute('class', 'icon');
  node.setAttribute('aria-hidden', 'true');
  node.setAttribute('focusable', 'false');
  const shape = (tag, attrs) => {
    const part = document.createElementNS(SVG_NS, tag);
    for (const [key, value] of Object.entries(attrs)) part.setAttribute(key, String(value));
    node.append(part);
  };
  shape('circle', { cx: 12, cy: 12, r: 9, fill: 'none', stroke: 'currentColor', 'stroke-width': 1.6 });
  for (const [cx, cy] of [[6.2, 12], [7.9, 7.9], [12, 6.2], [16.1, 7.9], [17.8, 12]]) {
    shape('circle', { cx, cy, r: 1.6, fill: 'currentColor' });
  }
  shape('rect', { x: 9.8, y: 14.6, width: 4.4, height: 2.6, rx: 1.3, fill: 'currentColor' });
  return node;
}

export const ICON_NAMES = [...Object.keys(ICONS), 'midi'];
