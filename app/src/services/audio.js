// The audio rack and the USB host socket: the module's, not the patch's
// (src/audio/rack.h). The rack is stored on the module by itself and a patch
// load leaves it running, so nothing here goes through the editor, the
// undoable patch or the library - every write is the module's at once, and
// what the page shows is what the module answers back.

export class Audio {
  constructor({ session, render, fail }) {
    this.session = session;
    this.render = render;
    this.fail = fail;
  }

  get device() { return this.session.device; }
  slot(index) { return this.session.rack?.slots[index] ?? null; }

  // A slot's plugin and routing. A new plugin starts at its defaults, which
  // is the module's call, so the slot is read back rather than guessed.
  async setSlot(index, changes) {
    const current = this.slot(index);
    if (!current || !this.device) return;
    const next = { ...current, ...changes };
    try {
      await this.device.setAudioSlot(index, next);
    } catch (e) {
      this.fail(`slot ${index + 1}: ${e.message}`);
    }
    await this.readSlot(index);
    this.render();
  }

  // One parameter, as a slider commits it. The page already shows it, so
  // this does not rebuild anything.
  async setParam(index, param, value) {
    const slot = this.slot(index);
    if (!slot || !this.device) return;
    slot.params[param] = value;
    try {
      await this.device.setAudioParam(index, param, value);
    } catch (e) {
      this.fail(`slot ${index + 1}: ${e.message}`);
    }
  }

  async clear(index) {
    try { await this.device?.clearAudioSlot(index); } catch (e) { this.fail(e.message); }
  }

  // The interface to use from the next time one is plugged in; null for any.
  async choose(usb) {
    try {
      await this.device?.setAudioDevice(usb ?? { vid: 0, pid: 0, name: '' });
    } catch (e) {
      this.fail(e.message);
    }
    await this.session.pollAudio({ force: true });
    this.render();
  }

  async readSlot(index) {
    const slot = await this.device?.getAudioSlot(index).catch(() => null);
    if (slot && this.session.rack) this.session.rack.slots[index] = slot;
  }
}
