// A button that is an icon. Its word goes in the tooltip and the accessible
// name, and `text` puts it beside the icon too where there is room for it.
//
// A word on a button is read; an icon is recognised. On a phone the
// difference is the width of the row: "disconnect", "zoom out" and "remove"
// are each a third of the screen, and three of them do not fit beside the
// thing they act on.

import { el, classes } from '../dom.js';
import { icon } from './icons.js';

export function IconButton({ icon: name, label, text = null, class: klass = '', ...attrs }) {
  return el('button', {
    type: 'button', class: classes('icon-btn', text && 'with-text', klass),
    title: label, 'aria-label': label, ...attrs,
  }, icon(name), text ? el('span', {}, text) : null);
}
