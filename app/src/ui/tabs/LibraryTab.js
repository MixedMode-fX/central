// The library tab: what a patch is kept in, and every way of moving one.
// Three places - this browser, a file, the module's preset slots - and the
// examples to start from. The patch never changes shape on the way: it is
// the image in all of them.

import { el, classes } from '../dom.js';
import { Panel, Hint, Row } from '../components/Panel.js';
import { IconButton } from '../components/IconButton.js';
import { Select } from '../components/Select.js';
import { Disclosure, remembered } from '../components/Disclosure.js';
import { ago } from '../../services/storage.js';
import { EXAMPLES } from '../../core/examples.js';
import { UNTITLED } from '../../services/state.js';
import './Library.css';

export function LibraryTab(app) {
  return el('div', {}, CurrentPanel(app), SavedPanel(app), ExamplesPanel(app), FilesPanel(app), SlotsPanel(app));
}

function CurrentPanel(app) {
  const { current } = app.state;
  return Panel('this patch',
    Row(
      // On input, and with no re-render: the field is usually left by
      // clicking "save", and a page rebuilt on the way out of the field would
      // replace the button before the click reached it.
      el('input', { type: 'text', class: 'grow', value: current.name, 'aria-label': 'patch name',
                    oninput: (e) => { current.name = e.target.value.trim() || UNTITLED; } }),
      el('button', { class: 'primary', onclick: () => app.patches.save() }, 'save'),
      current.id ? el('button', { onclick: () => app.patches.save({ asNew: true }) }, 'copy') : null),
    Hint(current.id
      ? `saved ${current.savedAt ? ago(current.savedAt) : 'earlier'}${current.dirty ? ' · changed' : ''}`
      : 'not saved'),
    Row(el('button', { class: 'ghost', onclick: () => app.patches.newPatch() }, 'new patch')));
}

function SavedPanel(app) {
  if (!app.library.available) return Panel('in this browser', Hint(app.library.reason));
  const entries = app.library.list();
  const currentId = app.state.current.id;
  return Panel(`in this browser (${entries.length})`,
    entries.length
      ? el('div', { class: 'saved-list' }, entries.map((entry) => el('div', {
          class: classes('saved', entry.id === currentId && 'current'),
        },
          Row(el('input', { type: 'text', class: 'grow', value: entry.name, 'aria-label': `name of ${entry.name}`,
                            onchange: (e) => app.patches.rename(entry.id, e.target.value) }),
              el('button', { onclick: () => app.patches.load(entry.id) }, 'load')),
          Row(el('span', { class: 'hint grow' },
                 `${entry.nodes} node${entry.nodes === 1 ? '' : 's'} · ${ago(entry.updated)}`
                 + `${entry.id === currentId ? ' · open' : ''}`),
              IconButton({ icon: 'copy', label: `duplicate ${entry.name}`, class: 'ghost',
                           onclick: () => app.patches.duplicate(entry.id) }),
              IconButton({ icon: 'download', label: `export ${entry.name} as .syx`, class: 'ghost',
                           onclick: () => app.patches.exportSaved(entry.id) }),
              IconButton({ icon: 'trash', label: `delete ${entry.name}`, class: 'ghost danger',
                           onclick: () => app.patches.remove(entry.id) })))))
      : Hint('nothing saved'));
}

// Somewhere to start. An empty library in front of a machine with this many
// algorithms is a wall, not a blank page.
function ExamplesPanel(app) {
  const { ui } = app.state;
  const names = Object.keys(EXAMPLES);
  ui.example ??= names[0];
  return Panel('examples',
    Row(Select({ class: 'grow', options: names.map((name) => ({ value: name, label: name })), value: ui.example,
                 onChange: (name) => { ui.example = name; app.render(); } }),
        el('button', { onclick: () => app.patches.loadExample(ui.example) }, 'load')));
}

function FilesPanel(app) {
  const json = app.patches.patchJson();
  const box = el('textarea', { class: 'json', spellcheck: 'false', rows: '12', 'aria-label': 'this patch as JSON' }, json);
  return Panel('files',
    Row(
      el('button', { onclick: () => app.patches.exportSyx() }, 'export .syx'),
      el('button', { onclick: () => app.patches.exportJson() }, 'export .json'),
      el('label', { class: 'file' }, 'import',
        el('input', { type: 'file', accept: '.syx,.bin,.json',
                      onchange: (e) => { if (e.target.files[0]) app.patches.importFile(e.target.files[0]); } }))),
    Disclosure({ summary: 'JSON', ...remembered(app.state.ui, 'json') },
      box,
      Row(
        el('button', { onclick: async () => {
          try {
            await navigator.clipboard.writeText(json);
            app.say('JSON copied to the clipboard');
          } catch {
            box.select();
            app.say('selected');
          }
        } }, 'copy'),
        el('button', { onclick: () => app.patches.loadJson(box.value, 'the JSON above') }, 'load'))));
}

function SlotsPanel(app) {
  const slots = app.device?.capabilities?.slots ?? 0;
  if (!slots) return Panel('on the module', Hint('no module'));
  return Panel(`on the module (${slots} slots)`,
    app.session.usingModule ? Hint('RAM: a reload empties them') : null,
    el('div', { class: 'slots' }, Array.from({ length: slots }, (_, s) => el('div', { class: 'slot' },
      el('span', {}, `slot ${s}`),
      el('button', { onclick: () => app.editor.saveSlot(s) }, 'save'),
      el('button', { onclick: () => app.patches.loadSlot(s) }, 'load'),
      el('button', { class: 'ghost', onclick: () => app.editor.eraseSlot(s) }, 'erase')))));
}
