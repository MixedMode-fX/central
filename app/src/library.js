// The library tab: what a patch is kept in, and every way of moving one.
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

import { el, iconButton } from './views.js';
import { ago } from './storage.js';
import { EXAMPLES } from './examples.js';

export function libraryTab(app) {
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
      el('button', { class: 'primary', onclick: () => app.savePatch() }, 'save'),
      current.id ? el('button', { onclick: () => app.savePatch({ asNew: true }) }, 'copy') : null),
    el('p', { class: 'hint' },
      current.id
        ? `saved ${current.savedAt ? ago(current.savedAt) : 'earlier'}${current.dirty ? ' · changed' : ''}`
        : 'not saved'),
    el('div', { class: 'row' },
      el('button', { class: 'ghost', onclick: () => app.newPatch() }, 'new patch')));
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
        iconButton({ icon: 'copy', label: `duplicate ${entry.name}`, class: 'ghost',
                     onclick: () => app.duplicateSaved(entry.id) }),
        iconButton({ icon: 'download', label: `export ${entry.name} as .syx`, class: 'ghost',
                     onclick: () => app.exportSaved(entry.id) }),
        iconButton({ icon: 'trash', label: `delete ${entry.name}`, class: 'ghost danger',
                     onclick: () => app.deleteSaved(entry.id) })));
  });

  return el('section', { class: 'panel' },
    el('h2', {}, `in this browser (${entries.length})`),
    entries.length ? el('div', { class: 'saved-list' }, rows)
                   : el('p', { class: 'hint' }, 'nothing saved'));
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
    el('h2', {}, 'examples'),
    el('div', { class: 'row' }, pick,
      el('button', { onclick: () => app.loadExample(pick.value) }, 'load')));
}

function filesPanel(app) {
  const json = app.patchJson();
  const box = el('textarea', { class: 'json', spellcheck: 'false', rows: '12',
                               'aria-label': 'this patch as JSON' }, json);
  const jsonDetails = el('details', {},
    el('summary', {}, 'JSON'),
    box,
    el('div', { class: 'row' },
      el('button', { onclick: async () => {
        try {
          await navigator.clipboard.writeText(json);
          app.status = 'JSON copied to the clipboard';
        } catch {
          box.select();
          app.status = 'selected';
        }
        app.render();
      } }, 'copy'),
      el('button', { onclick: () => app.loadJson(box.value, 'the JSON above') }, 'load')));
  jsonDetails.open = app.isOpen('json');
  jsonDetails.addEventListener('toggle', () => app.setOpen('json', jsonDetails.open));

  return el('section', { class: 'panel' },
    el('h2', {}, 'files'),
    el('div', { class: 'row' },
      el('button', { onclick: () => app.exportSyx() }, 'export .syx'),
      el('button', { onclick: () => app.exportJson() }, 'export .json'),
      el('label', { class: 'file' }, 'import',
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
      el('p', { class: 'hint' }, 'no module'));
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
    el('h2', {}, `on the module (${slots} slots)`),
    app.usingModule ? el('p', { class: 'hint' }, 'RAM: a reload empties them') : null,
    el('div', { class: 'slots' }, buttons));
}
