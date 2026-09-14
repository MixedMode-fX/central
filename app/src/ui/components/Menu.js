// A popup menu, one at a time, whichever button opened it: the learn and CV
// menus beside a parameter, and the dropdown that finishes a modulation drag
// on the canvas. Closes on a choice, on Escape, or on the next press anywhere
// else - none of which leaves anything behind.

import { el, classes } from '../dom.js';
import './Menu.css';

const ID = 'menu';

// One row: what it does, what it is on now, and the press.
export function MenuItem({ label, hint = null, onPick, class: klass = '' }) {
  return el('button', {
    type: 'button', class: classes('menu-item', klass), role: 'option',
    onclick: () => { closeMenu(); onPick(); },
  }, el('span', { class: 'menu-name' }, label),
     hint ? el('span', { class: 'menu-hint' }, hint) : null);
}

// A row that is a control rather than a choice.
export const MenuField = (label, control) => el('div', { class: 'menu-field' },
  el('span', { class: 'menu-name' }, label), control);

// `at` is a client point or a rectangle: a menu opens under the control that
// asked for it, or at the pointer. The rectangle is passed in rather than
// measured here because a menu that arms something has to know where its
// button was before the arming re-drew the page.
export function openMenu({ at, kind = '', head, items }) {
  closeMenu();
  const box = at.bottom !== undefined ? { x: at.left, y: at.bottom + 4 } : at;
  const width = globalThis.innerWidth ?? 9999;
  const menu = el('div', {
    class: classes('menu', kind), id: ID, role: 'listbox',
    style: `left:${Math.max(8, Math.min(box.x, width - 300))}px; top:${box.y}px`,
  },
    el('div', { class: 'menu-head' }, head),
    el('div', { class: 'menu-list' }, ...items));
  document.body.append(menu);
  menu.querySelector('.menu-item')?.focus();

  const dismiss = (e) => {
    if (e.type === 'keydown' && e.key !== 'Escape') return;
    if (e.type === 'pointerdown' && menu.contains(e.target)) return;
    closeMenu();
  };
  menu.dismiss = dismiss;
  // Deferred, so the press that opened this does not immediately close it.
  // Through `globalThis`, which is not a window where the panels are tested.
  setTimeout(() => {
    globalThis.addEventListener?.('pointerdown', dismiss);
    globalThis.addEventListener?.('keydown', dismiss);
  }, 0);
  return menu;
}

export function closeMenu() {
  const menu = document.getElementById(ID);
  if (!menu) return;
  if (menu.dismiss) {
    globalThis.removeEventListener?.('pointerdown', menu.dismiss);
    globalThis.removeEventListener?.('keydown', menu.dismiss);
  }
  menu.remove();
}
