// The patches tab: what a patch is kept in, and every way of moving one.
//
// A module holds four preset slots and forgets everything else; the built-in
// module forgets those too, because its EEPROM is RAM. So the app is where
// patches live. Three places, and the tab is laid out as the three of them:
//
//   * **this browser** - the library. Named patches, saved as the patch image
//     itself, kept in localStorage. This is the working set: the thing you
//     come back to tomorrow.
//   * **a file** - `.syx`, the bytes a module stores and any SysEx librarian
//     can send, and `.json`, the same patch in words.
//   * **the module** - its preset slots, recalled by Program Change.
//
// Everything here moves a patch between two of those, and the patch never
// changes shape on the way: it is the image in all three.

import { el } from './views.js';
import { ago } from './library.js';
import { EXAMPLES } from './examples.js';

export function patchesTab(app) {
  return el('div', {},
    currentPanel(app),
    libraryPanel(app),
    examplesPanel(app),
    filesPanel(app),
    slotsPanel(app));
}

function currentPanel(app) {
  const current = app.current;
  // On input, and with no re-render: the field is usually left by clicking
  // "save", and a page rebuilt on the way out of the field would replace the
  // button before the click reached it.
  const name = el('input', {
    type: 'text', class: 'grow', value: current.name, 'aria-label': 'patch name',
    oninput: (e) => { app.current.name = e.target.value.trim() || 'untitled'; },
  });
  return el('section', { class: 'panel' },
    el('h2', {}, 'this patch'),
    el('div', { class: 'row' }, name,
      el('button', { class: 'primary', onclick: () => app.savePatch() },
        current.id ? 'save' : 'save to this browser'),
      current.id ? el('button', { onclick: () => app.savePatch({ asNew: true }) }, 'save as a copy') : null),
    el('p', { class: 'hint' },
      current.id
        ? `Saved here ${current.savedAt ? ago(current.savedAt) : 'earlier'}${current.dirty ? ' — with changes since' : ''}.`
        : 'Not saved yet. Whatever is being edited is kept across a reload anyway; saving gives it a name '
          + 'and a place in the list below.'),
    el('div', { class: 'row' },
      el('button', { class: 'ghost', onclick: () => app.newPatch() }, 'start a new patch')));
}

function libraryPanel(app) {
  if (!app.library.available) {
    return el('section', { class: 'panel' },
      el('h2', {}, 'in this browser'),
      el('p', { class: 'hint' }, app.library.reason));
  }
  const entries = app.library.list();
  const rows = entries.map((entry) => {
    const name = el('input', {
      type: 'text', class: 'grow', value: entry.name, 'aria-label': `name of ${entry.name}`,
      onchange: (e) => app.renameSaved(entry.id, e.target.value),
    });
    return el('div', { class: `saved ${entry.id === app.current.id ? 'current' : ''}` },
      el('div', { class: 'row' }, name,
        el('button', { onclick: () => app.loadSaved(entry.id) }, 'load')),
      el('div', { class: 'row' },
        el('span', { class: 'hint grow' },
          `${entry.nodes} node${entry.nodes === 1 ? '' : 's'} · ${ago(entry.updated)}`
          + `${entry.id === app.current.id ? ' · open' : ''}`),
        el('button', { class: 'ghost', onclick: () => app.duplicateSaved(entry.id) }, 'duplicate'),
        el('button', { class: 'ghost', onclick: () => app.exportSaved(entry.id) }, '.syx'),
        el('button', { class: 'ghost danger', onclick: () => app.deleteSaved(entry.id) }, 'delete')));
  });

  return el('section', { class: 'panel' },
    el('h2', {}, `in this browser (${entries.length})`),
    entries.length ? el('div', { class: 'saved-list' }, rows) : el('p', { class: 'hint' },
      'Nothing saved yet. Build a patch, give it a name above and save it — it stays in this '
      + 'browser, on this device, and never leaves it.'),
    entries.length ? el('p', { class: 'hint' },
      'These live in this browser only. Export the ones you care about: clearing site data takes '
      + 'them with it, and another device cannot see them.') : null);
}

// Somewhere to start. An empty library in front of a machine with thirty
// algorithms is a wall, not a blank page.
function examplesPanel(app) {
  const names = Object.keys(EXAMPLES);
  const pick = el('select', { class: 'grow',
                              onchange: (e) => { app.example = e.target.value; app.render(); } });
  for (const name of names) {
    const option = el('option', { value: name }, name);
    if (name === app.example) option.selected = true;
    pick.append(option);
  }
  app.example ??= names[0];
  return el('section', { class: 'panel' },
    el('h2', {}, 'start from an example'),
    el('div', { class: 'row' }, pick,
      el('button', { onclick: () => app.loadExample(pick.value) }, 'load')),
    el('p', { class: 'summary' }, EXAMPLES[app.example]?.about ?? ''),
    el('p', { class: 'hint' },
      'Each one exercises one part of the machine. It lands here unsaved, so nothing you have '
      + 'saved is touched — give it a name above to keep your version of it.'));
}

function filesPanel(app) {
  const json = app.patchJson();
  const box = el('textarea', { class: 'json', spellcheck: 'false', rows: '12',
                               'aria-label': 'this patch as JSON' }, json);
  const jsonDetails = el('details', {},
    el('summary', {}, 'this patch as JSON'),
    el('p', { class: 'hint' },
      'The patch in words: algorithms by name, jacks numbered from 1, MIDI ports as a user knows '
      + 'them. It is readable, diffable and pasteable — and it carries the globals and the '
      + 'controller bindings too, so nothing is lost by going through it.'),
    box,
    el('div', { class: 'row' },
      el('button', { onclick: async () => {
        try {
          await navigator.clipboard.writeText(json);
          app.status = 'JSON copied to the clipboard';
        } catch {
          box.select();
          app.status = 'selected — copy it with your keyboard';
        }
        app.render();
      } }, 'copy'),
      el('button', { onclick: () => app.loadJson(box.value, 'the JSON above') }, 'load what is in the box')));
  jsonDetails.open = app.isOpen('json');
  jsonDetails.addEventListener('toggle', () => app.setOpen('json', jsonDetails.open));

  return el('section', { class: 'panel' },
    el('h2', {}, 'files'),
    el('p', { class: 'hint' },
      'A .syx file is the patch image — the bytes the module stores. Any SysEx librarian can send '
      + 'one, which is how a patch built with nothing plugged in reaches a module.'),
    el('div', { class: 'row' },
      el('button', { onclick: () => app.exportSyx() }, 'export .syx'),
      el('button', { onclick: () => app.exportJson() }, 'export .json'),
      el('label', { class: 'file' }, 'import a file',
        el('input', {
          type: 'file', accept: '.syx,.bin,.json',
          onchange: (e) => { if (e.target.files[0]) app.importFile(e.target.files[0]); },
        }))),
    jsonDetails);
}

function slotsPanel(app) {
  const slots = app.device?.capabilities?.slots ?? 0;
  if (!slots) {
    return el('section', { class: 'panel' },
      el('h2', {}, 'on the module'),
      el('p', { class: 'hint' }, 'Connect a module to use its preset slots.'));
  }
  const buttons = [];
  for (let s = 0; s < slots; s++) {
    buttons.push(el('div', { class: 'slot' },
      el('span', {}, `slot ${s}`),
      el('button', { onclick: () => app.edit(async () => {
        await app.device.saveSlot(s);
        app.status = `saved this patch to slot ${s}`;
        app.render();
      }, 'save') }, 'save'),
      el('button', { onclick: () => app.edit(async () => {
        await app.device.loadSlot(s);
        const dumped = await app.device.dump();
        app.patch = dumped.patch;
        app.globals = dumped.globals;
        app.diverged = false;
        app.current = { id: null, name: `slot ${s}`, dirty: true, savedAt: 0 };
        app.status = `recalled slot ${s}`;
        app.render();
      }, 'load') }, 'load'),
      el('button', { class: 'ghost', onclick: () => app.edit(() => app.device.eraseSlot(s), 'erase') }, 'erase')));
  }
  return el('section', { class: 'panel' },
    el('h2', {}, `on the module (${slots} slots of ${app.device.capabilities.slotBytes} bytes)`),
    el('p', { class: 'hint' },
      app.usingModule
        ? 'The built-in module’s slots are RAM, so a reload empties them — they are here to try '
          + 'Program Change recall, not to keep a patch. The library above is what keeps a patch.'
        : 'These are the module’s own presets, in its EEPROM. Program Change recalls them once '
          + 'recall is turned on under MIDI.'),
    el('div', { class: 'slots' }, buttons));
}
