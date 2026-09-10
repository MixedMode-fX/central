// A dropdown you can read, and the catalogue of what goes in it.
//
// The thing being chosen from is a list of thirty algorithms. A native
// `<select>` renders that as thirty lines of one font, and the only thing it
// can say about each is its label - so the add bar had been putting the whole
// description into the label ("EuclidianSequencer — 2 in, 1 out") and still
// could not say what the algorithm *does*. On a phone the browser then draws
// that list as a full-screen wheel of truncated strings. The registry has
// carried a one-line summary for every algorithm since the port names landed,
// and none of it could be shown.
//
// So the list is built rather than borrowed. Each row is a name, what it costs
// in connections, and the algorithm's own summary underneath; the rows are
// shelved by the category the firmware reports (`AlgorithmCategory`); and a
// search box narrows thirty rows to the two you meant. What is *not* borrowed
// is the accessibility a `<select>` gives away for free, so the same keyboard
// and the same roles are built back: a button that says what is chosen, a
// listbox of options, arrows and Enter and Escape, and `aria-activedescendant`
// so the search box keeps the keys while the list keeps the highlight.
//
// Two halves, deliberately apart:
//
//   * `catalogue()` and `filterGroups()` are arithmetic on the device's own
//     descriptors. No DOM, so they are tested with no browser in the room.
//   * `richSelect()` is the widget, and knows nothing about algorithms.

import * as P from './protocol.js';
import { el } from './views.js';
import { ALGORITHM_CATEGORIES } from './names.js';

// How many options it takes before a search box earns its place. Below this
// the whole list is on screen and a box to narrow it is one more thing to
// look past.
const SEARCH_AT = 10;

// --- what can be added ------------------------------------------------------

// Every algorithm the module reports, shelved by the category it reports with
// it, plus whatever else the caller can add (the patch's edges). An algorithm
// whose category this app does not know - a module running newer firmware -
// lands under "other" rather than disappearing, which is the reason the
// category is appended to the registry record rather than spliced into it.
export function catalogue(algorithms, endpoints = []) {
  const shelves = new Map(ALGORITHM_CATEGORIES.map((c) => [c.value, []]));
  const other = shelves.get(P.AlgorithmCategory.CATEGORY_NONE);

  for (const d of algorithms ?? []) {
    if (!d) continue;
    const shelf = shelves.get(d.category) ?? other;
    shelf.push({
      value: String(d.id),
      label: d.name,
      note: `${d.nIn} in · ${d.nOut} out${d.wantsTick ? ' · clocked' : ''}`,
      hint: d.summary ?? '',
    });
  }

  const groups = ALGORITHM_CATEGORIES
    .filter((c) => shelves.get(c.value).length)
    .map((c) => ({ key: c.key, label: c.label, options: shelves.get(c.value) }));

  if (endpoints.length) {
    groups.push({
      key: 'edges',
      label: 'edges',
      options: endpoints.map((e) => ({ value: e.key, label: e.label, note: e.note, hint: e.hint })),
    });
  }
  return groups;
}

// The words a search runs against: everything the row shows, plus the shelf it
// is on - so "midi" finds the MIDI algorithms and "seq" finds the sequencers
// whether or not either word is in the name.
const haystack = (option, group) =>
  `${group.label} ${option.label} ${option.note ?? ''} ${option.hint ?? ''}`.toLowerCase();

// Every term has to match somewhere, which is what makes "note seq" narrow to
// one row instead of matching everything with a note in it. Empty groups fall
// away, so a shelf never appears with nothing on it.
export function filterGroups(groups, query) {
  const terms = String(query ?? '').toLowerCase().split(/\s+/).filter(Boolean);
  if (!terms.length) return groups;
  return groups
    .map((group) => ({
      ...group,
      options: group.options.filter((o) => terms.every((t) => haystack(o, group).includes(t))),
    }))
    .filter((group) => group.options.length);
}

export const optionsOf = (groups) => groups.flatMap((group) => group.options);

export const optionFor = (groups, value) =>
  optionsOf(groups).find((o) => o.value === value) ?? null;

// --- the widget -------------------------------------------------------------

let nextId = 0;

// `value` is the option's value, `groups` is what `catalogue()` returned, and
// `onPick` is called with the new value once - the caller re-renders, and the
// panel goes with it.
export function richSelect({ value, groups, onPick, label, empty = 'nothing to add' }) {
  const id = `picker-${nextId++}`;
  const all = optionsOf(groups);
  const chosen = optionFor(groups, value) ?? all[0] ?? null;

  const face = el('button', {
    type: 'button', class: 'picker-face', id: `${id}-face`,
    role: 'combobox', 'aria-haspopup': 'listbox', 'aria-expanded': 'false',
    'aria-controls': `${id}-list`, 'aria-label': label,
  },
    el('span', { class: 'picker-face-text' },
      el('span', { class: 'picker-name' }, chosen ? chosen.label : empty),
      chosen?.note ? el('span', { class: 'picker-note' }, chosen.note) : null),
    el('span', { class: 'picker-caret', 'aria-hidden': 'true' }, '▾'));

  const root = el('div', { class: 'picker' }, face);
  if (!all.length) {
    face.disabled = true;
    return root;
  }

  // Open, and everything that closes it. `panel` is null when it is shut,
  // which is also what says whether the window listeners are attached.
  let panel = null;
  let active = null;      // the option row the keyboard is on
  let host = face;        // whatever has the focus while it is open

  // The highlight is announced from the element the keys are going to, so it
  // moves when the focus does - a finger that taps into the search box after
  // the panel opened without it takes the announcement with it.
  const setHost = (next) => {
    if (next === host) return;
    const at = host.getAttribute('aria-activedescendant');
    host.removeAttribute('aria-activedescendant');
    host = next;
    if (at) host.setAttribute('aria-activedescendant', at);
  };

  const close = ({ focus = false } = {}) => {
    if (!panel) return;
    window.removeEventListener('pointerdown', onOutside, true);
    panel.remove();
    panel = null;
    active = null;
    host.removeAttribute('aria-activedescendant');
    host = face;
    face.setAttribute('aria-expanded', 'false');
    if (focus) face.focus();
  };

  const onOutside = (e) => { if (!root.contains(e.target)) close(); };

  const pick = (option) => {
    close();
    onPick(option.value);
  };

  // The highlight, which is not the focus: focus stays where the typing goes -
  // the search box, or the face when there is none - and
  // `aria-activedescendant` on *that* element is what tells a screen reader
  // which row the arrows are on.
  const highlight = (row) => {
    if (active) { active.classList.remove('active'); active.setAttribute('aria-selected', 'false'); }
    active = row ?? null;
    if (!active) { host.removeAttribute('aria-activedescendant'); return; }
    active.classList.add('active');
    active.setAttribute('aria-selected', 'true');
    host.setAttribute('aria-activedescendant', active.id);
    active.scrollIntoView({ block: 'nearest' });
  };

  const move = (by) => {
    const rows = [...panel.querySelectorAll('.picker-option')];
    if (!rows.length) return;
    const at = active ? rows.indexOf(active) : -1;
    const next = by > 0
      ? (at + 1 >= rows.length ? 0 : at + 1)
      : (at <= 0 ? rows.length - 1 : at - 1);
    highlight(rows[next]);
  };

  // One row per option: the name and its cost on the first line, the
  // algorithm's own summary under it. `mousedown` is prevented so a click on a
  // row does not take the focus off the search box before the click lands.
  const rowFor = (option, at) => {
    const row = el('button', {
      type: 'button', class: 'picker-option', id: `${id}-o${at}`,
      role: 'option', 'aria-selected': 'false',
      onmousedown: (e) => e.preventDefault(),
      onclick: () => pick(option),
      onpointerenter: () => highlight(row),
    },
      el('span', { class: 'picker-option-head' },
        el('span', { class: 'picker-name' }, option.label),
        option.note ? el('span', { class: 'picker-note' }, option.note) : null),
      option.hint ? el('span', { class: 'picker-hint' }, option.hint) : null);
    if (option.value === chosen?.value) row.classList.add('chosen');
    return row;
  };

  // The list, as a listbox of groups. The shelf's name is a real `group` with
  // a label rather than a line of text between the options - a listbox whose
  // children are not options is a listbox a screen reader reads wrongly - and
  // the heading it draws is hidden from that reading, because the group is
  // already announced.
  const listFor = (query) => {
    const shown = filterGroups(groups, query);
    const list = el('div', { class: 'picker-list', id: `${id}-list`, role: 'listbox', 'aria-label': label });
    let at = 0;
    for (const group of shown) {
      list.append(el('div', { class: 'picker-shelf', role: 'group', 'aria-label': group.label },
        el('div', { class: 'picker-group', 'aria-hidden': 'true' }, group.label),
        group.options.map((option) => rowFor(option, at++))));
    }
    if (!at) list.append(el('div', { class: 'picker-empty' }, 'nothing matches'));
    return list;
  };

  const search = el('input', {
    type: 'text', class: 'picker-search', placeholder: 'search', spellcheck: 'false',
    autocomplete: 'off', 'aria-label': `search ${label}`,
    // A textbox that drives the listbox below it: the keys it does not use -
    // the arrows and Enter - move and take the highlight, which is what
    // `aria-controls` and `aria-activedescendant` say out loud.
    'aria-autocomplete': 'list', 'aria-controls': `${id}-list`,
    onfocus: () => setHost(search),
    oninput: () => {
      panel.querySelector('.picker-list').replaceWith(listFor(search.value));
      active = null;
      highlight(panel.querySelector('.picker-option'));
    },
  });

  const onKeys = (e) => {
    if (e.key === 'ArrowDown') { e.preventDefault(); move(1); }
    else if (e.key === 'ArrowUp') { e.preventDefault(); move(-1); }
    else if (e.key === 'Enter') { e.preventDefault(); if (active) active.click(); }
    else if (e.key === 'Escape') { e.preventDefault(); close({ focus: true }); }
    else if (e.key === 'Tab') close();
  };

  const open = () => {
    if (panel) { close({ focus: true }); return; }
    panel = el('div', { class: 'picker-panel', onkeydown: onKeys },
      all.length >= SEARCH_AT ? search : null,
      listFor(''));
    root.append(panel);
    host = face;
    // Below the control, unless there is more room above it. The add bar is
    // the last thing on the patch tab, so a panel that always dropped would
    // always open off the bottom of the page - which on a phone is a list you
    // have to scroll to before you can read it.
    const box = face.getBoundingClientRect?.();
    const room = globalThis.innerHeight ?? 0;
    if (box && room && room - box.bottom < Math.min(panel.scrollHeight + 12, 320)
        && box.top > room - box.bottom) {
      panel.classList.add('up');
    }
    face.setAttribute('aria-expanded', 'true');
    highlight(panel.querySelector('.picker-option.chosen') ?? panel.querySelector('.picker-option'));
    // A search box that steals the focus on a phone opens the keyboard over
    // the list it is meant to narrow, so it only takes it where there is a
    // pointer that hovers - a mouse, or a trackpad.
    if (panel.contains(search) && globalThis.matchMedia?.('(hover: hover)').matches) search.focus();
    else face.focus();
    window.addEventListener('pointerdown', onOutside, true);
  };

  face.addEventListener('click', open);
  face.addEventListener('keydown', (e) => {
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { e.preventDefault(); if (!panel) open(); else move(e.key === 'ArrowDown' ? 1 : -1); }
    else if (panel) onKeys(e);
  });
  return root;
}
