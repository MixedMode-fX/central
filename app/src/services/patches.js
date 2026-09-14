// Where a patch lives, and every way of moving one: this browser, a file, the
// module's preset slots, the examples. The patch never changes shape on the
// way - it is the image (`codec.encodePatch`) in all of them.

import * as P from '../protocol/generated.js';
import * as codec from '../protocol/codec.js';
import { toPatchJsonText, fromPatchJson } from '../core/patchjson.js';
import { EXAMPLES } from '../core/examples.js';
import { toBase64 } from './storage.js';
import { noPatch, UNTITLED } from './state.js';

const AUTOSAVE_MS = 400;

// A patch called "my patch (2)" must not become a file with a slash in it.
const fileName = (name) => (name || 'patch').replace(/[^\w .-]+/g, '-').slice(0, 60).trim() || 'patch';

export class Patches {
  constructor({ state, library, editor, arrangement, listener = null, render }) {
    this.state = state;
    this.library = library;
    this.editor = editor;
    this.arrangement = arrangement;
    this.listener = listener;
    this.render = render;
    this.autosaveTimer = null;
  }

  image() { return codec.encodePatch(this.state.patch, this.state.globals); }

  patchJson() { return toPatchJsonText(this.state.patch, this.state.globals, this.editor.device); }

  // --- replacing what is on screen -------------------------------------------

  // The patch on screen replaced by one nobody typed here: loaded, imported,
  // recalled, dumped off a module. One routine, so nothing that has to happen
  // on the way - the arrangement, the tab, the autosave, the send - is
  // forgotten by one of the six doors a patch can arrive through.
  replace({ patch, globals, current, savedImage = null, said, goToPatch = true }) {
    this.state.patch = patch;
    this.state.globals = globals;
    this.state.current = current;
    this.state.savedImage = savedImage;
    this.arrangement.reset();
    if (goToPatch) this.state.ui.tab = this.state.ui.editingTab = 'patch';
    this.autosave({ now: true });
    this.editor.sendWhole(said);
  }

  // Anything unsaved, put somewhere it can be found again: called before the
  // patch on screen is replaced by something the user did not type.
  stash() {
    const { current, patch } = this.state;
    if (!this.library.available || !patch.nodes.length) return null;
    if (current.id && !current.dirty) return null;
    try {
      return this.library.save({
        id: null,
        name: current.id ? `${current.name} (unsaved)` : current.name,
        bytes: this.image(),
        nodes: patch.nodes.length,
      });
    } catch {
      return null;
    }
  }

  // --- the working patch -----------------------------------------------------

  // Whatever is being edited, kept across a reload. Not the library: a patch
  // nobody named is not something a user asked to keep a copy of, but losing
  // an afternoon's work to a tab reload is not acceptable either.
  autosave({ now = false } = {}) {
    clearTimeout(this.autosaveTimer);
    const write = () => {
      if (!this.library.available) return;
      const bytes = this.image();
      const { current } = this.state;
      this.library.saveWorking({ id: current.id, name: current.name, bytes });
      const dirty = this.state.savedImage !== null && toBase64(bytes) !== this.state.savedImage;
      if (dirty !== current.dirty) {
        current.dirty = dirty;
        this.render();
      }
    };
    // Debounced, because a slider sweep is a hundred edits; immediate where
    // the next thing a user does might be closing the tab.
    if (now) write(); else this.autosaveTimer = setTimeout(write, AUTOSAVE_MS);
  }

  // The monitor setup, kept across a reload like the patch is. Written on the
  // change itself rather than debounced: these are a handful of deliberate
  // choices, not a slider sweep.
  saveListen() {
    if (this.listener) this.library.saveListen(this.listener.toJSON());
  }

  restoreWorking() {
    const working = this.library.readWorking();
    if (!working) return;
    try {
      const { patch, globals } = codec.decodePatch(working.bytes);
      // Whether it counts as changed is decided against the library entry it
      // came from: reopening a patch nobody touched must not claim there is
      // something to save.
      const entry = working.id ? this.library.get(working.id) : null;
      const savedImage = entry ? toBase64(entry.bytes) : null;
      this.replace({
        patch, globals, savedImage,
        current: {
          id: entry ? entry.id : null,
          name: working.name ?? UNTITLED,
          dirty: entry ? toBase64(working.bytes) !== savedImage : false,
          savedAt: entry?.updated ?? 0,
        },
        said: `restored “${working.name ?? UNTITLED}”`,
        goToPatch: false,
      });
    } catch (error) {
      // An image this build's decoder refuses is not a crash: it is a patch
      // this build cannot read, and saying so beats an empty page.
      this.state.status = `could not reopen the last patch: ${error.message}`;
      this.library.clearWorking();
    }
  }

  // --- the library -----------------------------------------------------------

  save({ asNew = false } = {}) {
    const { current, patch } = this.state;
    const bytes = this.image();
    try {
      const entry = this.library.save({
        id: asNew ? null : current.id,
        name: asNew ? `${current.name} copy` : current.name,
        bytes,
        nodes: patch.nodes.length,
      });
      this.arrangement.follow(entry.id);
      this.state.current = { id: entry.id, name: entry.name, dirty: false, savedAt: entry.updated };
      this.state.savedImage = toBase64(bytes);
      this.state.status = `saved “${entry.name}”`;
      this.autosave({ now: true });
    } catch (error) {
      this.state.error = error.message;
    }
    this.render();
  }

  load(id) {
    const entry = this.library.get(id);
    if (!entry) { this.editor.fail('that patch is no longer in this browser'); return; }
    try {
      const { patch, globals } = codec.decodePatch(entry.bytes);
      this.replace({
        patch, globals, savedImage: toBase64(entry.bytes),
        current: { id: entry.id, name: entry.name, dirty: false, savedAt: entry.updated },
        said: `loaded “${entry.name}”`,
      });
    } catch (error) {
      this.editor.fail(`could not read “${entry.name}”: ${error.message}`);
    }
  }

  // Deliberately without a render: this is called from a text field's own
  // change event, which fires when something else is clicked, and rebuilding
  // the list under that click would swallow it.
  rename(id, name) {
    const entry = this.library.rename(id, name.trim() || UNTITLED);
    if (entry && entry.id === this.state.current.id) this.state.current.name = entry.name;
  }

  duplicate(id) {
    this.library.duplicate(id);
    this.render();
  }

  remove(id) {
    this.library.remove(id);
    this.arrangement.drop(id);
    if (this.state.current.id === id) { this.state.current.id = null; this.state.savedImage = null; }
    this.editor.say('deleted');
  }

  exportSaved(id) {
    const entry = this.library.get(id);
    if (entry) this.downloadSyx(entry.bytes, `${fileName(entry.name)}.syx`);
  }

  // An example is loaded exactly as a file is: through the JSON dialect,
  // unsaved. It is a starting point, not a preset the app owns.
  loadExample(name) {
    const example = EXAMPLES[name];
    if (!example) return;
    this.stash();
    this.loadJson(JSON.stringify(example.patch), `the “${name}” example`, name);
  }

  newPatch() {
    this.stash();
    this.replace({
      patch: codec.emptyPatch(), globals: codec.emptyGlobals(), current: noPatch(),
      said: 'a new patch, empty',
    });
  }

  // The module recalled a preset, or applied a patch it was sent earlier.
  adopt({ patch, globals, said }) {
    this.replace({ patch, globals, current: { ...this.state.current, dirty: true }, said, goToPatch: false });
  }

  // --- the module's slots ----------------------------------------------------

  loadSlot(slot) {
    this.editor.edit(async () => {
      await this.editor.device.loadSlot(slot);
      const { patch, globals } = await this.editor.device.dump();
      this.replace({ patch, globals, current: { id: null, name: `slot ${slot}`, dirty: true, savedAt: 0 },
                     said: `recalled slot ${slot}`, goToPatch: false });
    }, 'load');
  }

  // --- files -----------------------------------------------------------------

  download(name, text, type) {
    const blob = new Blob([text], { type });
    const link = document.createElement('a');
    link.href = URL.createObjectURL(blob);
    link.download = name;
    document.body.append(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(link.href), 1000);
  }

  // A .syx file is the fallback for a browser with no Web MIDI and the way a
  // patch is backed up: the same chunk messages the module receives, so any
  // standard SysEx librarian can send it.
  downloadSyx(image, name) {
    const chunks = codec.patchChunks(image, this.editor.device?.deviceId ?? P.SYSEX_DEFAULT_DEVICE);
    const bytes = [];
    for (const chunk of chunks) bytes.push(...chunk);
    this.download(name, Uint8Array.from(bytes), 'application/octet-stream');
    this.editor.say(`exported ${name}`);
  }

  exportSyx() { this.downloadSyx(this.image(), `${fileName(this.state.current.name)}.syx`); }

  exportJson() {
    this.download(`${fileName(this.state.current.name)}.json`, this.patchJson(), 'application/json');
    this.editor.say('exported .json');
  }

  async importFile(file) {
    const name = file.name.replace(/\.[^.]+$/, '');
    try {
      if (/\.json$/i.test(file.name)) {
        this.loadJson(await file.text(), file.name, name);
        return;
      }
      const bytes = new Uint8Array(await file.arrayBuffer());
      // Either a raw image or the chunked SysEx a librarian would have saved.
      const image = bytes[0] === 0xf0 ? codec.reassembleFile(bytes) : bytes;
      const { patch, globals } = codec.decodePatch(image);
      this.replace({ patch, globals, current: { id: null, name, dirty: true, savedAt: 0 },
                     said: `loaded ${file.name}` });
    } catch (error) {
      this.editor.fail(`could not read that file: ${error.message}`);
    }
  }

  // Returns whether it loaded, so the page that asked can move on.
  loadJson(text, what = 'that JSON', name = null) {
    try {
      const { patch, globals } = fromPatchJson(JSON.parse(text), this.editor.device);
      const current = name ? { id: null, name, dirty: true, savedAt: 0 } : { ...this.state.current };
      this.replace({ patch, globals, current, said: `loaded ${what}` });
      return true;
    } catch (error) {
      this.editor.fail(`could not read ${what}: ${error.message}`);
      return false;
    }
  }
}
