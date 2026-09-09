# MMMC emulator

The firmware core, compiled unchanged to WebAssembly, with the browser standing
in for the Teensy. Open one HTML file, load a patch, patch the jacks with the
mouse, send it MIDI, and watch the buses, the jacks and the MIDI it sends.

```
make emulator          # builds emulator/dist/index.html and smoke-tests it
open emulator/dist/index.html
```

The build from `main` is live at <https://mixedmode-fx.github.io/central/>,
deployed by `.github/workflows/pages.yml` on every push to `main`. Running that
workflow by hand on another branch puts that branch's build on the site until
the next push to `main`. Being served over `https://` also satisfies Web MIDI's
secure-context requirement, so the Web MIDI bridge works from there.

Nothing under `src/` is modified or duplicated: the algorithms, the buses, the
node pool, the registry, the port nodes and `MixedModeMaster` are the same
object code the tests run, built from the same sources. What the emulator adds
is a second implementation of the hardware seam, next to the Teensy one:

| | Teensy 4.1 | Emulator |
|---|---|---|
| `IGpio` (the 8 jacks) | `src/hal/teensy/teensy_gpio.cpp` | `emulator/src/web_hal.h` `WebGpio`: eight bytes the page reads and writes |
| `IMidiOut` (the transports) | `src/hal/teensy/teensy_midi.cpp` | `WebMidiOut`: one call into JavaScript |
| main loop | `src/main.cpp` | `index.html`: passes on simulated time |
| clock timer and sync pin | `src/hal/teensy/teensy_clock.cpp` | `index.html`: `MasterClock::advance()` every `subtick_interval_us()`, `sync_edge()` from a simulated jack |
| MIDI input | `mm_midi_read()` and the input queue | `MixedModeMaster::deliver_midi()` from the page |
| entropy at boot | cycle counter, floating ADC | `Math.random()` |

## Why this works with no rewrite

The investigation that led here, kept so the reasoning survives.

**The seam already exists.** The firmware logic reaches hardware through two
interfaces, `IGpio` and `IMidiOut` (`src/hal/`). `main.cpp` and
`src/hal/teensy/` are the only files that include the Arduino core, and the
`native` environment in `platformio.ini` already builds everything else on a
PC for the unit tests. CI enforces that nothing under `src/algorithm/` names a
pin or a transport. An emulator is a third client of that seam, after the
Teensy and the test fakes.

**The core is freestanding.** Across `src/` minus the Teensy layer, the only
standard headers used are `<stdint.h>`, `<stddef.h>` and `<new>` (for
placement new into the node pool). There is no heap after boot, no exceptions,
no RTTI, no floating point, no strings. That is what makes the WebAssembly
build trivial: no libc is needed, so no Emscripten is needed either. A stock
`clang --target=wasm32 -ffreestanding -nostdlib` compiles every file, and
`emulator/shim/new` supplies the one declaration `<new>` would have. The
module is about 20 KB and imports exactly one function, the MIDI callback.

**It held when the firmware grew.** The emulator was first built against the
bus model alone; when #17 landed (master clock, MIDI input queue, modifier
family, sequencers, 4500 lines) every new source compiled to wasm32 unchanged
under `-Wall -Wextra -Wshadow -Weffc++ -Werror`, and the only emulator work
was on its side of the seam. `MasterClock` exposes `advance()` and
`external_edge()` as plain methods for the Teensy's interrupt handlers to
call, so the page is the interval timer: it calls `advance()` every
`subtick_interval_us()` of simulated time, reprogramming itself when
`take_interval_change()` says so, exactly as `teensy_clock.cpp` does.

### Options considered

| Approach | Verdict |
|---|---|
| **Full-system emulation** (QEMU, Renode) of the i.MX RT1062 | No maintained Teensy 4.1 machine model. Would need models of the USB device and host controllers, the UARTs, the interval timers and the Teensy core's startup. Months of work to verify the vendor core rather than this firmware. |
| **Native host binary + local server** (the `native` toolchain, a WebSocket bridge to a page) | Works, but needs a process running next to the browser, so it cannot be a static page, a CI artifact or a GitHub Pages link, and it adds a Python or Node service to maintain. |
| **Emscripten** | Works, but brings a libc and a 1 GB SDK for a core that uses neither. |
| **Stock clang, freestanding wasm32** | Chosen. Two packages (`clang`, `lld`), a 130-line build script, a 20 KB module, no runtime beyond `memcpy`/`memset`. |

## What it can and cannot verify

It verifies the firmware's **logic**, with the firmware's own code:

- patch validation: `MixedModeMaster::load()` is the validator, so a patch the
  emulator rejects is a patch the module rejects, with the same error;
- the bus model: double buffering, fan-in rules, one pass of latency per
  stage, feedback as a one-pass delay, overflow counting;
- every algorithm, with its parameters, at gate rate and (via the tick
  button) at clock rate;
- routing: which jacks are claimed as outputs, which MIDI targets receive an
  event, channel overrides and source filters.

It does not model:

- **time**: a pass takes as long as the page says (default 100 µs) and passes
  are evenly spaced. Jitter, ISR latency and the real loop period are Teensy
  measurements;
- **the transports**: DIN framing, the USB MIDI cables, the USB host stack
  and MIDI parsing live in `src/hal/teensy/teensy_midi.cpp` and are not
  exercised. The page delivers already-parsed events, as `deliver_midi()`
  receives them;
- **input polarity**: `TeensyGpio::read()` inverts the pin for the active-low
  input stage. The page supplies "gate present" directly, which is the level
  algorithms see after that normalisation;
- **the Teensy core**, its libraries, EEPROM, the LEDs and the console (none
  of which the firmware uses yet beyond `Serial.println` at boot);
- **the ARM compiler**: the module is built by clang for wasm32, the firmware
  by GCC for Cortex-M7. Sizes shown in the registry table are the wasm build's.

## Layout

```
emulator/
  build.sh          clang + wasm-ld; produces dist/mmmc.wasm and dist/index.html
  index.html        the page; fetches mmmc.wasm or uses the copy build.sh embeds
  shim/new          placement new, the one thing <new> is needed for
  src/web_hal.h     WebGpio and WebMidiOut: the seam, browser side
  src/emu_api.cpp   the C ABI the page calls; owns one MixedModeMaster
  src/runtime.cpp   memcpy, memset and the two C++ ABI hooks the compiler emits
  test/smoke.mjs    the test_master scenarios, replayed through the module
  dist/             build output (ignored by git; CI uploads index.html,
                    the Pages workflow publishes the directory)
```

`build.sh` compiles the same file set as `build_src_filter` in the `native`
environment, with the firmware's warning flags, and refuses to link if any
symbol other than the MIDI callback is imported: a libc call creeping into the
core fails the build rather than the page.

The page never sees a struct. It builds a patch through `emu_patch_*` setters,
reads the registry through `emu_algo_*` and the sizing constants through
`emu_const_*`, so a change to `config.h`, to the registry or to the preset
format is picked up by rebuilding. The only firmware values written into
JavaScript are enum names: MIDI port bits, gate directions, domains and the
error codes.

## Using the page

- **Patch.** A JSON patch, resolved by algorithm *name* through the registry.
  Each preset is a simple patch that exercises one part of the machine, with
  a line saying what to do and what to expect: the default patch from
  `main.cpp`, a metronome off two dividers, MIDI thru with the sustain pedal,
  a pure router, a channel split with a merge, chord and transpose, mono note
  priority, an arpeggiator on a divider, three Euclidean sequencers in
  lock-step, a step sequencer thinned by Probability, a random sequencer with
  a shred jack, a divider chain, the logic gates, a feedback loop and a patch
  the validator rejects. Loading is `MixedModeMaster::load()` followed by
  `setup()`; a rejected patch leaves the running one in place, as on the module.

  ```json
  {
    "gate_ports": [{ "port": 1, "dir": "out", "bus": 0 }],
    "midi_in":    [{ "sources": ["DIN 1"], "channel": 0, "bus": 0 }],
    "nodes":      [{ "algo": "ClockDiv", "out": [0], "params": [0, 6] },
                   { "algo": "Arpeggiator", "in": [0, 0], "out": [1], "params": [2, 2, 60, 0] }],
    "midi_out":   [{ "targets": ["USB 1", "DIN 2"], "channel": 0, "bus": 1 }]
  }
  ```

  Jacks are numbered 1 to 8 as on the panel. MIDI ports go by the names a
  user knows: `DIN 1`, `DIN 2`, `USB 1` to `USB 4` (the device cables, counted
  from 1 as a DAW lists them; the firmware enum counts them from 0) and
  `USB Host`; the firmware's enum names (`SERIAL_1`, `USB_0`, ...) are
  accepted too, and the mapping is listed under "Algorithms" on the page.
  Bus indices are in the domain the algorithm declares for that inlet, as in
  `NodeConfig`. `null` leaves an optional inlet unconnected. `"ALL"` is every
  port bit; the firmware's `ALL_MIDI_PORTS` depends on which transports a
  build compiles in. `ClockDiv`'s amount is in PPQN ticks: `/24` is a quarter
  note, `/6` a sixteenth.
- **Clock & run.** The page is the main loop: it calls `pass(now_us)` on
  simulated time at the chosen pass interval, at up to the chosen multiple of
  real time, or one pass at a time. It is also the interval timer for the
  master clock, so BPM, source and transport are the real `MasterClock`:
  internal, CV (a simulated sync jack, pulsed by hand or at a rate, with the
  pulses-per-quarter setting) or MIDI (the page can send MIDI clock at the
  BPM into a chosen port). A beat LED and the subtick count show it running.
- **Jacks.** Inputs can be toggled, pulsed for 20 ms, or driven by a square
  wave at a chosen frequency. Outputs light when the firmware drives them
  high. A jack's card follows the mode the port node claimed.
- **Buses.** The front buffer after each pass: which gate buses are high, how
  many events each note bus carried and the last one, the overflow counters,
  the CV values.
- **Scope.** The last two seconds of every jack and every gate bus, sampled
  every millisecond of simulated time with short pulses held so they show.
- **Play.** An on-screen keyboard with octave shift, built for touch as much
  as for the mouse, a CC sender and an all-notes-off, all going through
  `deliver_midi()` into the chosen port and channel. The count of ports that
  accepted the message is shown, so a routing or channel filter is visible.
  The page lays out in one column on a phone, so it can be played from one.
- **MIDI out.** Every `IMidiOut::send()` with its simulated timestamp, the
  target ports by name, the type, channel and data.
- **Listen.** A small Web Audio synth stands in for whatever would be
  downstream of the module: one oscillator per note on the chosen MIDI
  target(s), with a short envelope, CC 64 sustain, pitch bend, and CC 120/123
  silence. Events are scheduled on the audio clock at the simulated time they
  happened, so an arpeggio sounds at the rate the patch produced it. Output
  jacks can click on every rising edge, pitched by jack number, which makes a
  clock division or a logic gate audible. None of this is firmware: it plays
  what `IMidiOut::send()` and the jacks emit. Audio has to be enabled with the
  button because browsers only start sound from a user gesture.
- **Web MIDI.** Optional. In Chrome or Edge, a real controller can feed
  `deliver_midi()` and the firmware's output can drive a real port, so a DAW
  can be pointed at the emulated module.

## Keeping it true to the firmware

- The build compiles `src/` with the native environment's file selection.
  A new source file needs nothing; a new hardware seam needs a web
  implementation in `web_hal.h`, as it needs a Teensy one in `src/hal/teensy/`.
- `test/smoke.mjs` replays the `test_master` scenarios through the module in
  CI. When the native tests gain a scenario worth seeing in the browser, add
  it there too.
- Where the roadmap touches the seam:
  - **#4 and #5 (done):** the page is the interval timer and the sync jack,
    and delivers MIDI with a timestamp as the queue drain in `main.cpp` does.
  - **#7, LEDs and console:** whatever interface the LEDs get, the page gets
    two more LEDs; console output can go to a text panel.
  - **#11, the patch protocol:** feeding SysEx bytes to the same handler
    makes the emulator a device for #12's editor to talk to, with no hardware
    attached. The editor's acceptance criterion "a patch the editor accepts
    is never rejected by the firmware's validator" then holds by
    construction, because the validator in the page *is* the firmware's, and
    it can run in CI.
