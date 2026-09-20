// The composition root: every service, built once and wired together, and the
// few commands that cross more than one of them.
//
// What a view gets is this object. It reads `state`, asks `device` what the
// firmware has, and calls a service to change anything - never the device
// itself, and never `state` except through a service.

import { EmbeddedModule } from '../runtime/module.js';
import { loadWasm } from '../runtime/wasm.js';
import { Listener } from '../runtime/audio/listener.js';
import { Controller } from '../runtime/controller.js';
import { MidiOutputs } from '../runtime/midiout.js';
import { describeSupport, access, discover, WebMidiTransport } from '../runtime/webmidi.js';
import { Library } from './storage.js';
import { createState, noPatch } from './state.js';
import { Live, Renderer } from './render.js';
import { Arrangement } from './arrangement.js';
import { Session } from './session.js';
import { Editor } from './editor.js';
import { Play } from './play.js';
import { Transport } from './transport.js';
import { Surface } from './surface.js';
import { Patches } from './patches.js';

export function createApp({ root, view, wasmUrl }) {
  const state = createState();
  const library = new Library();
  const live = new Live();
  const app = {
    state, library, live,
    module: null,           // the firmware, running in this page
    listener: null,         // the audio standing in for what is downstream
    controller: null,       // a MIDI controller plugged into this computer
    outputs: null,          // where what the module plays leaves this computer
    get device() { return session.device; },
  };
  const render = () => renderer.render();
  const renderer = new Renderer({
    root, live, view: () => view(app),
    // The rebuild is the one stall the page can see coming: the module runs
    // ahead of it, so the passes it would have held up are already done.
    before: (ms) => app.module?.runAhead(ms),
    onPainted: () => { patches.autosave(); app.refreshLive(); },
  });
  const arrangement = new Arrangement({ state, library });
  const session = new Session({ state, live, render });
  const editor = new Editor({ state, session, arrangement, render });
  // Where a note goes when you play one: the module in the page, or the one
  // on the cable. No view knows which (services/play.js).
  const play = new Play({
    state, session, render,
    module: () => app.module,
    listener: () => app.listener,
    refresh: () => app.refreshLive(),
  });
  // Start, stop, continue and panic: the machine's own controls, which is why
  // they are in the shell rather than on a tab (services/transport.js).
  const transport = new Transport({
    session,
    listener: () => app.listener,
    outputs: () => app.outputs,
    fail: (message) => app.fail(message),
  });
  const patches = new Patches({ state, library, editor, arrangement, render });
  // The pads and pots: the performer's own controller, kept in this browser
  // rather than in the patch (services/surface.js).
  const surface = new Surface({ state, library, play, editor, session, render });
  Object.assign(app, { render, arrangement, session, editor, patches, play, transport, surface });

  app.say = (message) => editor.say(message);
  app.fail = (message) => editor.fail(message);
  app.dismissError = () => { state.error = null; render(); };

  // The lights, the playheads, the scope and the meters, painted once per
  // animation frame off the module's own frame callback: the module is the
  // thing that knows when a frame's worth of passes has just run.
  app.refreshLive = () => {
    if (!app.module || !session.usingModule) return;
    live.tick({ module: app.module, activity: app.module.takeActivity() });
  };

  session.addEventListener('replaced', (e) => patches.adopt(e.detail));
  // The module's learn is the truth about whether one is running: when it
  // lands, the control that armed it stops saying it is waiting.
  session.addEventListener('learned', () => {
    editor.learned();
    state.ui.surface.armed = null;
  });

  // --- the module, and the module on the cable ------------------------------

  // The module starts with the page. Nothing here asks the user to press
  // anything first: an app whose whole premise is "the thing you are editing
  // is running" cannot open with it stopped.
  app.boot = async () => {
    render();
    try {
      app.module = await EmbeddedModule.instantiate(await loadWasm(wasmUrl));
      app.listener = new Listener(app.module);
      app.listener.restore(library.readListen());
      editor.listener = app.listener;
      patches.listener = app.listener;
      app.controller = new Controller(app.module);
      app.outputs = new MidiOutputs(app.module, library);
      await app.useModule({ silent: true });
      patches.restoreWorking();
    } catch (error) {
      state.status = `the built-in module did not start: ${error.message}`;
      state.error = 'no algorithms: connect a module, or serve mmmc.wasm beside the page';
    }
    render();
  };

  let stopLive = null;

  // Back to (or on to) the module in the page.
  app.useModule = async ({ silent = false } = {}) => {
    if (!app.module) return;
    if (!silent) patches.stash();
    app.module.start();
    play.detach();
    await session.adopt(app.module, { usingModule: true });
    state.current = noPatch();
    state.savedImage = null;
    arrangement.reset();
    stopLive?.();
    stopLive = app.module.onFrame(app.refreshLive);
    render();
  };

  // A module on the end of a cable. Its running patch replaces what is on
  // screen, so anything unsaved is put in the library first.
  app.connect = async () => {
    const support = describeSupport();
    if (!support.ok) { app.say(support.reason); return; }
    app.say('looking for a module…');
    try {
      const midi = await access();
      const found = await discover(midi);
      if (!found.length) { app.say('no module answered'); return; }
      const kept = patches.stash();
      const port = found[0];
      // The module in the page is about to stop playing, and a note it sent
      // out of this computer is only ended by a note-off: nothing will send
      // one once it has stopped.
      app.outputs?.panic();
      app.module?.stop();
      stopLive?.();
      stopLive = null;
      await session.adopt(new WebMidiTransport(port.input, port.output), { deviceId: port.deviceId });
      // Musical MIDI goes out a different cable from the protocol's, which is
      // why cable 3 is reserved: a dump in flight and a pad being hit must not
      // collide. The access is the one already granted - a second request
      // would be a second permission prompt for something already allowed.
      play.attach({ access: midi, control: port.output, name: port.name });
      state.current = { id: null, name: `on ${port.name}`, dirty: true, savedAt: 0 };
      state.savedImage = null;
      arrangement.reset();
      if (kept) state.status += ` — kept “${kept.name}”`;
    } catch (error) {
      state.status = `could not connect: ${error.message}`;
    }
    render();
  };

  // The MIDI devices on this computer, asked for once: a controller to play
  // the module with and the outputs to play a synth out of are two halves of
  // the same permission, and asking twice is two prompts for something
  // already allowed.
  app.connectMidi = async () => {
    try {
      const midi = await app.controller.connect();
      await app.outputs.connect();
      // A device unplugged mid-session should not leave a dead selection on
      // screen, and a route pointed at it is not forgotten - it comes back
      // when the device does. Web MIDI tells us, so the page follows. One
      // owner: `onstatechange` is a single slot, so a second listener set
      // from a peripheral would silently take the first one's place.
      midi.onstatechange = () => render();
      state.status = 'MIDI devices found';
    } catch (error) {
      state.status = `could not reach the MIDI devices: ${error.message}`;
    }
    render();
  };

  // --- the tabs ---------------------------------------------------------------

  app.showTab = (tab) => {
    state.ui.tab = state.ui.editingTab = tab;
    render();
  };

  // play is the module running, not a fourth thing to edit, so it is a
  // button at the top rather than a tab - and leaving it goes back to
  // whichever tab it was entered from.
  app.togglePlay = () => {
    const ui = state.ui;
    if (ui.tab === 'play') ui.tab = ui.editingTab;
    else { ui.editingTab = ui.tab; ui.tab = 'play'; }
    render();
  };

  return app;
}
