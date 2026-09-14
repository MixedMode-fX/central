// The box a pattern lives in. On a phone its lanes wrap and there is nothing
// here to scroll; on a screen wide enough to hold the whole pattern on one
// line they do that instead, and the leftover scrolls inside this box rather
// than widening the page. Where it was scrolled to is remembered, because the
// page is rebuilt wholesale on every edit: without that, toggling step 20
// would scroll the pattern back to step 1.

import { el } from '../dom.js';

export function Scroller(app, key, ...children) {
  const { scrolled } = app.state.ui;
  const box = el('div', {
    class: 'scroll-x',
    onscroll: (e) => scrolled.set(key, e.target.scrollLeft),
  }, ...children);
  const at = scrolled.get(key);
  if (at) app.live?.onMount(() => { box.scrollLeft = at; });
  return box;
}
