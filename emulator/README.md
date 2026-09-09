# The module, compiled to WebAssembly

The firmware core, built unchanged for the browser, with the page standing in
for the Teensy. This directory is the *module*; the app that drives it is
`app/` (see `app/README.md`), and there is no page here any more — the emulator
and the editor are one app, because the module in the page was always both the
thing being edited and the thing running.

```
make module            # builds emulator/dist/mmmc.wasm and the app page,
                       # then replays the test_master scenarios through it
make app               # the above, plus the app's own checks
```

The build from `main` is live at <https://mixedmode-fx.github.io/central/>,
deployed by `.github/workflows/pages.yml` on every push to `main`. Running that
workflow by hand on another branch puts that branch's build on the site until
the next push to `main`. Being served over `https://` also satisfies Web MIDI's
secure-context requirement, so a real module and a real controller can be
reached from there.

Nothing under `src/` is modified or duplicated: the algorithms, the buses, the
node pool, the registry, the port nodes and `MixedModeMaster` are the same
object code the tests run, built from the same sources. What this adds is a
second implementation of the hardware seam, next to the Teensy one:

| | Teensy 4.1 | In the page |
|---|---|---|
| `IGpio` (the 8 jacks) | `src/hal/teensy/teensy_gpio.cpp` | `emulator/src/web_hal.h` `WebGpio`: eight bytes the page reads and writes |
| `IMidiOut` (the transports) | `src/hal/teensy/teensy_midi.cpp` | `WebMidiOut`: one call into JavaScript |
| main loop | `src/main.cpp` | `app/src/module.js`: passes on simulated time |
| clock timer and sync pin | `src/hal/teensy/teensy_clock.cpp` | `module.js`: `MasterClock::advance()` every `subtick_interval_us()`, `sync_edge()` from a simulated jack |
| MIDI input | `mm_midi_read()` and the input queue | `MixedModeMaster::deliver_midi()` from the page |
| the control plane (`main.cpp` step 5) | the loop's tail | `emu_control_service()`, the same five calls in the same order |
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
  build.sh          clang + wasm-ld, then app/tools/bundle.mjs for the page
  shim/new          placement new, the one thing <new> is needed for
  src/web_hal.h     WebGpio and WebMidiOut: the seam, browser side
  src/emu_api.cpp   the C ABI the page calls; owns one MixedModeMaster
  src/runtime.cpp   memcpy, memset and the two C++ ABI hooks the compiler emits
  test/smoke.mjs    the test_master scenarios, replayed through the module
  dist/             build output (ignored by git): mmmc.wasm, and index.html -
                    the whole app as one file. CI uploads the page, the Pages
                    workflow publishes the directory.
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

## What the page may do

The page never sees a struct. It builds a patch through the `emu_patch_*`
setters, reads the registry through `emu_algo_*` and the sizing constants
through `emu_const_*`, so a change to `config.h`, to the registry or to the
preset format is picked up by rebuilding. The only firmware values written into
JavaScript are enum names: MIDI port bits, gate directions, domains and the
error codes — and even those are generated from the headers, in
`app/src/protocol.js`.

Two rules keep the seam a seam:

- **the page decides nothing the firmware decides.** It calls
  `emu_control_service()` once per pass, which is `main.cpp`'s five tail calls
  in `main.cpp`'s order, including the beat flash and the clock-running state
  behind the green LED: the LED vocabulary is `led/status_leds.h`'s, and a page
  that chose for itself when to flash would be showing something the module
  does not do.
- **an incoming MIDI event takes the loop's path.** Program Change to preset
  recall, then NRPN, then the binding table, then `deliver_midi()` — so a
  controller played into the page is played into the firmware's control plane,
  not into a JavaScript imitation of it.

What the app does with all this — the tabs, the patch library, the audio — is
`app/README.md`.

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
  - **#13 and #14 (done):** nothing at the seam. The sequencer grids are
    read through `emu_seq_*`, a read-only view resolved from the node's
    descriptor id, and the `seq` sugar in the page packs steps into the
    firmware's parameter layout. The patch-swap handover (`Node::silence`,
    flushed through the old patch's MIDI ports) shows up in the MIDI log as
    the note-offs that arrive when a patch is loaded over a sounding one.
  - **#7, LEDs and console:** whatever interface the LEDs get, the page gets
    two more LEDs; console output can go to a text panel.
  - **#11 and #12 (done):** `emu_sysex_in()` feeds the firmware's own
    `SysexHandler`, which makes the module in the page a device for the app to
    talk to with no hardware attached. The app's acceptance criterion "a patch
    the app accepts is never rejected by the firmware's validator" holds by
    construction, because the validator in the page *is* the firmware's — and
    `app/test/protocol.test.mjs` runs it in CI.
