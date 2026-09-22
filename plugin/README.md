# The module, as a plugin

The firmware core in a DAW, with the host standing in for the Teensy. This
directory is the *plugin*; the page it shows is `app/`, and the module it
runs is `src/`, neither of them changed for it.

```
make plugin            # the engine and its test, with a host compiler (the commit gate's plugin scope)
make vst3              # the VST3 and a standalone app, on this machine (fetches JUCE)
```

CI builds the VST3 for Windows on every push and pull request and uploads it
as an artifact (`.github/workflows/ci.yml`, `vst3`): a `MMMC.vst3` folder for
a DAW's VST3 directory. It needs the WebView2 runtime, which is part of
Windows.

## The seam

Nothing under `src/` is modified or duplicated. This is the third
implementation of the hardware seam, beside the Teensy's and the page's:

| | Teensy 4.1 | In the page | In the plugin |
|---|---|---|---|
| `IGpio` (the jacks) | `src/hal/teensy/teensy_gpio.cpp` | `WebGpio` | `PluginGpio`: **no jacks** |
| `IMidiOut` | `teensy_midi.cpp` | `WebMidiOut` | `PluginMidiOut`: every cable is the host's MIDI output |
| `IEeprom` | `teensy_eeprom.cpp` | `WebEeprom` | `PluginEeprom`: RAM, saved with the project |
| main loop | `src/main.cpp` | `app/src/runtime/module.js` | `Engine::process`: passes on the sample clock |
| clock timer | `teensy_clock.cpp` | `module.js` | `Engine::drive_clock` |
| clock source | the timer, the sync jack, MIDI clock | the same, simulated | **the host's playhead, as MIDI clock** |
| MIDI input | `mm_midi_read()` | `deliver_midi()` | the block's MIDI, at its sample offsets |
| the editor | the console, the app over a cable | the app, in the page | the app, in the plugin's window |

`src/engine.h` is all of that with no plugin framework in it: `main.cpp`'s
loop over the plugin's HAL, built and tested by `test/engine_test.cpp` with
whatever C++ compiler is on the desk. `src/processor.cpp` and
`src/editor.cpp` are the two files that know what a DAW is.

## Output routing is just MIDI

The module routes by an eight-bit mask of cables - four USB cables, DIN
ports, a host port - and eight gate jacks. A plugin has one MIDI output, so:

- **A `MidiOutPort` on any musical cable reaches the host.** The mask is
  the module's; where the events go afterwards is the DAW's routing. The
  control cable (`MIDI_CONTROL_PORT`) is the one exception: a SysEx reply
  addressed to it is the protocol answering the editor, and goes to the
  plugin's window instead. Clock out (`clock_out_mask`) leaves by the same
  wire.
- **There are no jacks.** A gate port in the patch is on no wire: an
  output writes into the void, an input reads low. The patch format keeps
  them because it is the module's format.
- **The host's MIDI input arrives on `HOST_IN`** (`mmMIDI_USB_0`). The
  page's keyboard and surface play on whichever cable the page chooses, as
  they do against the module in the page, so the DAW track and the window
  are two inputs of the patch. Realtime bytes on the host's MIDI input are
  dropped: the playhead is the transport here.

## Time

A pass is `PASS_US` of the sample clock; a block runs the passes that fall
inside it. A message from the host is heard by the pass that covers its
sample, and a message the module sends is stamped with the sample of the pass
that made it, so the plugin is sample-accurate to within a pass.

**The host is a MIDI clock.** `HostClock` turns the playhead into the
realtime a slaved module hears, on `HOST_IN`: a start when play begins, a
stop when it ends, and a clock byte at every `1/MASTER_PPQN` of a quarter of
the host's own grid after that. The firmware's rule then applies unchanged:
a patch whose clock source is MIDI follows the transport, a patch on the
internal clock runs free while start and stop still reach it. The plugin's
default globals set the source to MIDI, because a plugin that ignored the
play button would be the odd one out on the track. Start is the downbeat and
every clock byte is the end of a tick, which is why the pulse *at* the
downbeat is never sent; a loop or a relocate re-phases the pulses without a
start, so the count keeps going forward.

## State

The host saves the module's memory whole (`Engine::write_state`): the EEPROM
image, which is slot 0 and the presets exactly as the module keeps them, and
the running patch encoded on its own, because a slot is a kilobyte and the
running patch is allowed to be larger. Nothing is migrated: a state from
another `PATCH_FORMAT_VERSION` is refused whole and the module boots as one
flashed over a foreign EEPROM does, on its defaults with the red LED lit.
Restoring a state tells an open editor over the protocol
(`SYSEX_EVENT_PATCH_APPLIED`), so it re-reads the module by the path it
already has.

## The editor

The editor is the app in a web view - WebKit on Linux, WebView2 on Windows -
showing the one-file page `emulator/build.sh` produces, embedded at build
time. The page finds out it is inside the plugin the way it finds out about
hardware, by what answers (`app/src/runtime/host.js`), and the bridge
carries three things, all of them bytes a cable could carry: a SysEx message
each way, and a channel message from the page's keyboard on the cable it
chose. The plugin's window therefore sees exactly what its MIDI input would,
and the app is the same client of `src/protocol/sysex.h` it is everywhere.

## Threads

The host runs the passes on its audio thread; the editor and the state calls
arrive on the message thread. The firmware is one loop with the control
plane between passes, so the engine is one object under one lock, taken
whole and briefly by a block, a SysEx message and a state save alike.

## Layout

```
plugin/
  CMakeLists.txt      the core, the engine and its test; with MMMC_VST3, JUCE and the plugin
  src/plugin_hal.h    PluginGpio, PluginMidiOut, PluginEeprom, PluginLeds: the seam, host side
  src/engine.h/.cpp   the module in the plugin, and the host's playhead as a MIDI clock
  src/processor.*     juce::AudioProcessor: blocks in, MIDI out, the state
  src/editor.*        the app in a web view, and the bridge to it
  test/engine_test.cpp  the engine driven as a host drives it
  build/              build output (gitignored), JUCE fetched into it
```
