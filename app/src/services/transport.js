// Start, stop, continue — and the panic button.
//
// These are the machine's controls, not a panel's: they belong to whichever
// module is live, they mean the same thing on every tab, and there is nothing
// about them to edit. So they live here rather than inside the clock panel
// they used to be buried in, and the shell and the performance surface both
// draw the same four (ui/components/Transport.js).
//
// **Over the protocol, not over a cable.** The transport is otherwise reached
// by MIDI realtime, which arrives on a *musical* port - and the app holds the
// control cable, which carries nothing musical by design. A SysEx transport
// message (`SYSEX_TRANSPORT`) is therefore the only way the app can press
// start on a module it is editing, and it is the same call for the module in
// the page and the one on the end of a cable: the page's module answers SysEx
// through the same handler the hardware runs.
//
// **A panic has three places to reach**, and a button that only did the first
// would be the worst kind of half-measure:
//
//   * **the module**, which is holding the notes its patch played and is the
//     only thing that can release them on its own cables (`SYSEX_PANIC`).
//   * **this computer's MIDI ports**, where what the page has already sent is
//     past the module for good: only a note-off from here ends it
//     (runtime/midiout.js).
//   * **the page's own audio**, whose voices are not MIDI notes at all and no
//     message ends (runtime/audio/listener.js).

import * as P from '../protocol/generated.js';

export class Transport {
  constructor({ session, listener = () => null, outputs = () => null, fail = () => {} }) {
    this.session = session;
    this.listenerOf = listener;
    this.outputsOf = outputs;
    this.failed = fail;
  }

  start() { this.press(P.CcTransportTarget.CC_TRANSPORT_START); }
  stop() { this.press(P.CcTransportTarget.CC_TRANSPORT_STOP); }
  resume() { this.press(P.CcTransportTarget.CC_TRANSPORT_CONTINUE); }
  tap() { this.press(P.CcTransportTarget.CC_TRANSPORT_TAP); }

  panic() {
    // The module first, so its own note-offs - each carrying the
    // transformation that made its note - are on the wire before the blunt
    // sweep the ports below send.
    this.ask((device) => device.panic());
    this.outputsOf()?.panic();
    this.listenerOf()?.panic();
  }

  press(what) { this.ask((device) => device.pressTransport(what)); }

  // A press must not wait for the round trip - a transport that took a
  // hundred milliseconds to feel would be worse than no button - and a module
  // that has gone away must not swallow it in silence, so the failure is said
  // where every other failed command is said.
  ask(what) {
    const { device } = this.session;
    if (!device) return;
    what(device).catch((error) => this.failed(error.message));
  }
}
