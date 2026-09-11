# The module, compiled to WebAssembly

The firmware core built unchanged for the browser, with the page standing in
for the Teensy. This directory is the *module*; the app that drives it is
`app/`.

```
make module            # builds emulator/dist/mmmc.wasm and the app page,
                       # then replays the test_master scenarios through it
make app               # the above, plus the app's own checks
```

The build from `main` is live at <https://mixedmode-fx.github.io/central/>,
deployed by `.github/workflows/pages.yml`. Running that workflow by hand on
another branch puts that branch's build on the site until the next push to
`main`. Being served over `https://` also satisfies Web MIDI's secure-context
requirement.

## The seam

Nothing under `src/` is modified or duplicated. This adds a second
implementation of the hardware seam, next to the Teensy one:

| | Teensy 4.1 | In the page |
|---|---|---|
| `IGpio` (the jacks) | `src/hal/teensy/teensy_gpio.cpp` | `emulator/src/web_hal.h` `WebGpio`: bytes the page reads and writes |
| `IMidiOut` | `src/hal/teensy/teensy_midi.cpp` | `WebMidiOut`: one call into JavaScript |
| main loop | `src/main.cpp` | `app/src/module.js`: passes on simulated time |
| clock timer and sync pin | `src/hal/teensy/teensy_clock.cpp` | `module.js`: `advance()` every `subtick_interval_us()`, `sync_edge()` from a simulated jack |
| MIDI input | `mm_midi_read()` and the input queue | `deliver_midi()` from the page |
| the control plane | `main.cpp`'s loop tail | `emu_control_service()`, the same calls in the same order |
| entropy at boot | cycle counter, floating ADC | `Math.random()` |

**Why it needs no rewrite.** The firmware reaches hardware only through `IGpio`
and `IMidiOut` (`src/hal/`); `main.cpp` and `src/hal/teensy/` are the only
files that include the Arduino core, and CI enforces that nothing under
`src/algorithm/` names a pin or a transport. The core is also freestanding —
the only standard headers are `<stdint.h>`, `<stddef.h>` and `<new>`, with no
heap after boot, no exceptions, no RTTI, no floating point, no strings — so
stock `clang --target=wasm32 -ffreestanding -nostdlib` compiles every file,
`emulator/shim/new` supplies the one declaration `<new>` would have, and no
Emscripten and no libc are needed. The module imports exactly one function, the
MIDI callback.

`build.sh` compiles the same file set as `build_src_filter` in the `native`
environment, with the firmware's warning flags, and refuses to link if any
other symbol is imported: a libc call creeping into the core fails the build
rather than the page.

## Two rules keep the seam a seam

- **The page decides nothing the firmware decides.** It calls
  `emu_control_service()` once per pass, which is `main.cpp`'s tail calls in
  `main.cpp`'s order, the beat flash and the LED vocabulary included.
- **An incoming MIDI event takes the loop's path**: Program Change to preset
  recall, then NRPN, then the binding table, then `deliver_midi()`.

The page never sees a struct. It builds a patch through `emu_patch_*`, reads
the registry through `emu_algo_*` and the sizing constants through
`emu_const_*`, so a change to `config.h`, the registry or the preset format is
picked up by rebuilding. The only firmware values written into JavaScript are
enum names, and those are generated into `app/src/protocol.js`.

## What it can and cannot verify

It verifies the firmware's **logic**, with the firmware's own code: patch
validation (`MixedModeMaster::load()` *is* the validator, so the emulator
rejects what the module rejects, with the same error), the bus model, every
algorithm at gate rate and at clock rate, and routing.

It does not model:

- **time** — a pass takes as long as the page says and passes are evenly
  spaced. Jitter, ISR latency and the real loop period are Teensy measurements;
- **the transports** — DIN framing, USB MIDI cables, the USB host stack and
  MIDI parsing live in `teensy_midi.cpp`. The page delivers already-parsed
  events;
- **input polarity** — the page supplies "gate present" directly, which is what
  algorithms see after the HAL's active-low normalisation;
- **the Teensy core**, its libraries, EEPROM and the console;
- **the ARM compiler** — this is clang for wasm32, the firmware is GCC for
  Cortex-M7.

## Layout

```
emulator/
  build.sh          clang + wasm-ld, then app/tools/bundle.mjs for the page
  shim/new          placement new, the one thing <new> is needed for
  src/web_hal.h     WebGpio and WebMidiOut: the seam, browser side
  src/emu_api.cpp   the C ABI the page calls; owns one MixedModeMaster
  src/runtime.cpp   memcpy, memset and the two C++ ABI hooks clang emits
  test/smoke.mjs    the test_master scenarios, replayed through the module
  dist/             build output (gitignored): mmmc.wasm, and index.html —
                    the whole app as one file
```

## Keeping it true to the firmware

- A new source file needs nothing. A new hardware seam needs a web
  implementation in `web_hal.h`, as it needs a Teensy one in `src/hal/teensy/`.
- `test/smoke.mjs` replays the `test_master` scenarios in CI. When the native
  tests gain a scenario worth seeing in the browser, add it there too.
