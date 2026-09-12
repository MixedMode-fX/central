// The patch library: patches kept in this browser.
//
// The module holds PATCH_SLOTS presets in EEPROM and the built-in module holds
// its own in RAM, which a reload throws away. Neither is somewhere to keep
// work. A patch is a few hundred bytes, so the browser can hold as many as
// anyone will make, and keeping them here means the app is useful with no
// module, no cable and no file manager - which is the case the whole app is
// built for.
//
// **What is stored is the patch image**: the same bytes `encodePatch()`
// produces, the same bytes a `.syx` file carries and the module writes to a
// slot. Not a JavaScript object of the app's own shape - that would be a
// third format to keep in step with the firmware, and the one that would
// silently rot. So a stored patch can be exported, sent to hardware or loaded
// into a slot without conversion, and a patch format version change is caught
// by the same decoder that catches it in a file.

const LIBRARY_KEY = 'mmmc.library.v1';
const WORKING_KEY = 'mmmc.working.v1';
const LISTEN_KEY = 'mmmc.listen.v1';
const CANVAS_KEY = 'mmmc.canvas.v1';

// How many patches keep hand-placed blocks. A layout is a few hundred bytes
// and only exists for a patch somebody arranged by hand, but the store must
// not grow without a bound either, so the least recently arranged fall off.
const LAYOUTS_KEPT = 24;

export const toBase64 = (bytes) => {
  let binary = '';
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary);
};

export const fromBase64 = (text) => {
  const binary = atob(text);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return bytes;
};

const newId = () => `p${Date.now().toString(36)}${Math.floor(Math.random() * 1e6).toString(36)}`;

export class Library {
  constructor(storage = globalThis.localStorage) {
    this.storage = storage;
    // Private windows, disabled site data and a full quota all show up here.
    // A library that cannot be written is not a reason for the app to fail -
    // it is a reason to say so and keep the export path working.
    this.available = false;
    this.reason = 'this browser is not storing patches';
    try {
      const probe = '__mmmc_probe__';
      storage.setItem(probe, '1');
      storage.removeItem(probe);
      this.available = true;
      this.reason = '';
    } catch (error) {
      this.reason = `patches cannot be saved here (${error.name}). Export a file instead.`;
    }
  }

  read() {
    if (!this.available) return [];
    try {
      const raw = this.storage.getItem(LIBRARY_KEY);
      const parsed = raw ? JSON.parse(raw) : [];
      return Array.isArray(parsed) ? parsed : [];
    } catch {
      return [];
    }
  }

  write(entries) {
    if (!this.available) throw new Error(this.reason);
    try {
      this.storage.setItem(LIBRARY_KEY, JSON.stringify(entries));
    } catch (error) {
      // A quota error is the one failure a user can act on, and the action is
      // to delete something - so it says that rather than the DOM's wording.
      throw new Error(error.name === 'QuotaExceededError'
        ? 'no room left: delete a patch, or export it to a file'
        : `could not save: ${error.message}`);
    }
  }

  // Newest first, without the images: a list of fifty patches should not carry
  // fifty patch images through the render path.
  list() {
    return this.read()
      .map(({ image, ...rest }, at) => ({ ...rest, at }))
      // Newest first, and for two saved in the same millisecond the later one
      // first: "duplicate" then puts the copy above the original, where the
      // eye is already looking.
      .sort((a, b) => b.updated - a.updated || b.at - a.at)
      .map(({ at, ...rest }) => rest);
  }

  get(id) {
    const found = this.read().find((entry) => entry.id === id);
    return found ? { ...found, bytes: fromBase64(found.image) } : null;
  }

  // Upsert. A save with no id makes a new patch; a save with one overwrites
  // it, which is what the "save" button on an already-named patch does.
  save({ id, name, bytes, nodes }) {
    const entries = this.read();
    const now = Date.now();
    const image = toBase64(bytes);
    const at = id ? entries.findIndex((entry) => entry.id === id) : -1;
    const entry = at >= 0
      ? { ...entries[at], name, image, nodes, updated: now }
      : { id: id ?? newId(), name, image, nodes, created: now, updated: now };
    if (at >= 0) entries[at] = entry; else entries.push(entry);
    this.write(entries);
    return entry;
  }

  rename(id, name) {
    const entries = this.read();
    const at = entries.findIndex((entry) => entry.id === id);
    if (at < 0) return null;
    entries[at] = { ...entries[at], name, updated: Date.now() };
    this.write(entries);
    return entries[at];
  }

  remove(id) {
    this.write(this.read().filter((entry) => entry.id !== id));
  }

  duplicate(id) {
    const entry = this.read().find((e) => e.id === id);
    if (!entry) return null;
    const copy = { ...entry, id: newId(), name: `${entry.name} copy`, created: Date.now(), updated: Date.now() };
    this.write([...this.read(), copy]);
    return copy;
  }

  // The patch being edited, saved on every change so a reload - or a phone
  // deciding to discard the tab - does not lose it. This is separate from the
  // library: work in progress is not something a user asked to keep, and it
  // must not appear in the list as if it were.
  saveWorking(state) {
    if (!this.available) return;
    try {
      this.storage.setItem(WORKING_KEY, JSON.stringify({
        id: state.id ?? null, name: state.name, image: toBase64(state.bytes), updated: Date.now(),
      }));
    } catch { /* a full quota must never break editing */ }
  }

  readWorking() {
    if (!this.available) return null;
    try {
      const raw = this.storage.getItem(WORKING_KEY);
      if (!raw) return null;
      const parsed = JSON.parse(raw);
      return { ...parsed, bytes: fromBase64(parsed.image) };
    } catch {
      return null;
    }
  }

  clearWorking() {
    if (this.available) this.storage.removeItem(WORKING_KEY);
  }

  // How the monitor is set up: which note buses have a player on them, how
  // loud each one is, how loud the gate clicks are. Not part of a patch - it
  // is about listening to a patch, and it should outlive the one on screen -
  // but losing it on every reload is the same annoyance as losing the patch.
  saveListen(state) {
    if (!this.available) return;
    try {
      this.storage.setItem(LISTEN_KEY, JSON.stringify(state));
    } catch { /* a full quota must never break the audio */ }
  }

  readListen() {
    if (!this.available) return null;
    try {
      const raw = this.storage.getItem(LISTEN_KEY);
      return raw ? JSON.parse(raw) : null;
    } catch {
      return null;
    }
  }

  // Where a block was dragged to. Not part of a patch - a `.syx` file and the
  // module's own slots have nowhere to put a coordinate and should not gain
  // one - so this is kept beside the library rather than in it, keyed per
  // patch, and a patch with nothing here simply gets the automatic layout.
  readCanvas() {
    if (!this.available) return { layouts: {} };
    try {
      const raw = this.storage.getItem(CANVAS_KEY);
      const parsed = raw ? JSON.parse(raw) : null;
      return { layouts: parsed && typeof parsed.layouts === 'object' ? parsed.layouts : {} };
    } catch {
      return { layouts: {} };
    }
  }

  writeCanvas(state) {
    if (!this.available) return;
    try {
      this.storage.setItem(CANVAS_KEY, JSON.stringify(state));
    } catch { /* a full quota must never break editing */ }
  }

  layoutFor(key) {
    return this.readCanvas().layouts[key] ?? null;
  }

  // Written newest last, so trimming from the front drops the patch nobody has
  // arranged for longest.
  saveLayout(key, positions) {
    const state = this.readCanvas();
    delete state.layouts[key];
    const entries = Object.entries(state.layouts).slice(-(LAYOUTS_KEPT - 1));
    entries.push([key, positions]);
    this.writeCanvas({ ...state, layouts: Object.fromEntries(entries) });
  }

  dropLayout(key) {
    const state = this.readCanvas();
    if (!(key in state.layouts)) return;
    delete state.layouts[key];
    this.writeCanvas(state);
  }

  // A patch saved for the first time was until then the working patch, so the
  // arrangement follows it to the identity it has just been given.
  moveLayout(from, to) {
    const state = this.readCanvas();
    if (from === to || !state.layouts[from]) return;
    state.layouts[to] = state.layouts[from];
    delete state.layouts[from];
    this.writeCanvas(state);
  }
}

// "3 minutes ago" beats a timestamp in a list whose whole purpose is to answer
// "which one was I just working on?".
export function ago(then, now = Date.now()) {
  const seconds = Math.max(0, Math.round((now - then) / 1000));
  if (seconds < 60) return 'just now';
  const minutes = Math.round(seconds / 60);
  if (minutes < 60) return `${minutes} min ago`;
  const hours = Math.round(minutes / 60);
  if (hours < 24) return `${hours} h ago`;
  const days = Math.round(hours / 24);
  if (days < 30) return `${days} d ago`;
  return new Date(then).toLocaleDateString();
}
