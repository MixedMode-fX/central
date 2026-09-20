// The module's clock while nothing is painting the page.
//
// A browser stops giving a hidden page the time of day, and "hidden" is not
// only another tab: a window behind another window counts, and so does the
// screen going off. `requestAnimationFrame` does not fire there at all, and
// `setInterval` is clamped to one wake-up a second - one a minute after five
// minutes of it. Those two are the module's clocks (`runtime/module.js`), so
// the machine in the page stopped dead the moment you looked away: the
// sequencer froze mid-bar, the clock stopped, and a synth on the end of a
// cable was left holding whatever note was sounding.
//
// **The audio render thread is the exception.** An AudioContext keeps
// rendering its quanta at real time whether or not anybody is looking - it is
// what makes a page play music in the background at all - and a worklet on it
// can post a message to the page per quantum. A message is not a timer
// wake-up and is not throttled like one, so it is a clock: every few
// milliseconds the passes the wall clock owes are run, and the module goes on
// playing while the page is away.
//
// Three things worth knowing:
//
//   * **It needs a gesture.** A browser starts no audio without one, so this
//     arms itself on the first press anywhere in the page rather than asking
//     for anything: the module keeps running in the background from the first
//     time you touch it.
//   * **It beats only while the page is hidden.** Visible, the context is
//     suspended and the timer and the animation frame are the clock again - a
//     page being looked at has no business holding the audio device open, or
//     interrupting what else a phone is playing.
//   * **It is not a promise.** iOS suspends a backgrounded tab's audio
//     outright, and a browser without AudioWorklet gets nothing here. Where it
//     cannot beat the module stops with the page and skips forward when it
//     comes back, which is what it did everywhere before this.

import { TICK_MS } from './module.js';

const PROCESSOR = 'mmmc-heartbeat';

// The worklet, as source. The app is one file with the module inlined
// (`emulator/build.sh`), so the processor is compiled from a string here
// rather than fetched from a second URL that would not be beside it.
const SOURCE = `
class Heartbeat extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.every = options.processorOptions.every;
    this.last = -1;
  }
  // Called once per render quantum, on the audio thread, for as long as the
  // context runs. The message is the beat; the silence it leaves in its
  // output is what keeps it connected to something and therefore pulled.
  process() {
    if (currentTime - this.last >= this.every) {
      this.last = currentTime;
      this.port.postMessage(0);
    }
    return true;
  }
}
registerProcessor('${PROCESSOR}', Heartbeat);
`;

const GESTURES = ['pointerdown', 'keydown', 'touchend'];

export class Heartbeat {
  constructor(module, { every = TICK_MS, doc = globalThis.document } = {}) {
    this.module = module;
    // Seconds, which is what the audio thread counts in.
    this.every = every / 1000;
    this.doc = doc ?? null;
    this.ctx = null;
    this.node = null;
    this.armed = false;
    // Beating, as opposed to meant to be: set by the first beat that actually
    // arrives, so "the page is hidden" and "something is running the passes"
    // are two different facts and the page can say which it has.
    this.beating = false;
    // Why there is no heartbeat, where there is a reason worth reporting.
    this.reason = '';
    // The last thing a press or a visibility change asked of the audio
    // context, still in flight. Nothing in the app waits on it - a page that
    // has just been hidden has nobody to tell - but a test does.
    this.ready = Promise.resolve();
  }

  // Wait for the first press anywhere, then build the context. Also the one
  // place the page's own comings and goings are listened to.
  arm() {
    if (this.armed || !this.doc?.addEventListener) return;
    this.armed = true;
    const wake = () => {
      for (const type of GESTURES) this.doc.removeEventListener(type, wake, true);
      this.ready = this.enable();
    };
    for (const type of GESTURES) this.doc.addEventListener(type, wake, { capture: true, passive: true });
    this.doc.addEventListener('visibilitychange', () => { this.ready = this.follow(); });
  }

  // The context, the worklet and the node - built once, and left suspended
  // until the page is hidden.
  async enable() {
    if (this.ctx) return true;
    const Context = globalThis.AudioContext ?? globalThis.webkitAudioContext;
    if (!Context || !globalThis.AudioWorkletNode) {
      this.reason = 'this browser has no audio worklet';
      return false;
    }
    try {
      const ctx = new Context();
      await ctx.audioWorklet.addModule(URL.createObjectURL(new Blob([SOURCE], { type: 'text/javascript' })));
      const node = new globalThis.AudioWorkletNode(ctx, PROCESSOR, { processorOptions: { every: this.every } });
      node.port.onmessage = () => this.beat();
      // A worklet is only pulled if what it makes reaches the destination, and
      // what this one makes is silence: it is a clock, not a sound.
      node.connect(ctx.destination);
      this.ctx = ctx;
      this.node = node;
    } catch (error) {
      // No heartbeat is a page that stops in the background, which is a thing
      // the app can live with; it is not worth a message over the app.
      this.reason = error.message;
      return false;
    }
    await this.follow();
    return true;
  }

  // The page changed sides. Hidden, the context runs and its beats are what
  // the passes are run from; visible, the animation frame and the timer have
  // it again and the context is suspended.
  async follow() {
    if (!this.ctx) return;
    try {
      if (this.doc?.hidden) {
        await this.ctx.resume();
      } else {
        this.beating = false;
        await this.ctx.suspend();
      }
    } catch (error) {
      // A context the browser would not move - a phone that will not start
      // audio for a page nobody is looking at. The module stops with the page,
      // as it did before there was a heartbeat.
      this.beating = false;
      this.reason = error.message;
    }
  }

  // One beat: the passes the wall clock owes, run on the audio thread's time.
  beat() {
    this.beating = true;
    this.module.tick();
  }

  // Everything closed and forgotten. Nothing in the app calls this - the
  // heartbeat lives as long as the page does - but a test does, and a context
  // left open across one is a context the next test inherits.
  async close() {
    this.beating = false;
    this.node?.disconnect();
    this.node = null;
    const ctx = this.ctx;
    this.ctx = null;
    try { await ctx?.close(); } catch { /* already gone */ }
  }
}
