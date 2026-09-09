# The MMMC app

One page that **is** the module: it builds a patch, runs the firmware that will
play it, and lets you hear the result while you edit it.

`src/hardware.h` declares no encoder, no switches and no display, so there is
**no human input on the module at all**. With the serial console as the only
other option, this is how the module gets configured — a shipping deliverable,
not a companion app.

## Running it

There is no build step and no dependencies: ES modules and one HTML file. Serve
it rather than opening it from disk, because a browser will not load a module
from a `file://` URL and Web MIDI needs a secure context anyway:

```sh
make module                       # compiles the firmware to WebAssembly
python3 -m http.server 8080       # from the repository root
                                  # then open http://localhost:8080/app/
```

The build from `main` is live at <https://mixedmode-fx.github.io/central/>.
`make module` also writes `emulator/dist/index.html`, which is the whole app as
a single file with the module embedded — that one opens from a download, with
nothing serving it, and it is what CI attaches to every branch.

## Why the editor and the emulator are one app

They were two pages. The editor built patches and sent them to a module over
Web MIDI or to a module embedded in the page; the emulator ran that same
embedded module and let you *hear* it, but took its patch as a blob of JSON
pasted into a box. The loop that matters — change a parameter, hear what it did
— went through the clipboard.

They are one app now, because the module in the page was always doing both
jobs:

- **it is the transport.** `Device` talks to a *transport*, not to Web MIDI, so
  the firmware compiled to WebAssembly is one. Every edit reaches it as the
  SysEx message a cable would have carried, and the validator that accepts or
  refuses a patch is the firmware's own.
- **it is a machine that runs.** The page is its main loop, its interval timer
  and its sync pin, exactly as `main.cpp` and `teensy_clock.cpp` are on a
  Teensy. So its jacks, its LEDs, its MIDI output and its sequencer positions
  are all live in the same page as the controls that shape them.

Nothing had to be added to the firmware for this: `emulator/README.md` has the
seam it hangs on.

## The four tabs

**patch** — the graph. Buses are the connections: every inlet and outlet is a
selector offering only the buses of its own domain, under the name the firmware
gives it, and each one says what else is on its bus, because that is what a
patch cable would have shown. A sequencer gets a purpose-built view — a step
grid for the gate and drum sequencers, a note lane over scale degrees for the
note sequencers — and while the patch runs, **the step being played is outlined
in the same grid you are editing**.

**play** — the module, running. Two LEDs and the gate buses, the clock's
transport and tempo, the eight jacks (tap, hold or free-run an input; an output
lights when the firmware drives it), an on-screen keyboard and CC sender that
go in through the module's MIDI input on a chosen port and channel, a small
synth so the patch can be heard, and the MIDI the module is sending.

This is the emulator's surface, cut down. The old page also had the pass
interval, a speed multiplier, single-stepping, a scope, the bus tables and the
registry listing: instruments for debugging the *simulation*. This tab answers
questions about the *patch*. Anything that needs the others has the native
tests, `emulator/test/smoke.mjs` and a debugger.

**MIDI** — an external controller, the controller bindings, the routing, the
clock and Program Change recall.

**patches** — where a patch lives: this browser, a file, or the module's own
preset slots.

## Patches are kept in the browser

A module holds four preset slots in EEPROM; the built-in module holds its own
in RAM and a reload empties them. Neither is somewhere to keep work. So the app
keeps a library in `localStorage`: named patches, listed newest first, loaded
in one press, renamed in place, duplicated, deleted or exported.

**What is stored is the patch image** — the same bytes `encodePatch()`
produces, the same bytes a `.syx` file carries and the module writes to a slot.
Not an object of the app's own shape: that would be a third format to keep in
step with the firmware, and the one that would silently rot. A stored patch can
go to hardware or to a file with no conversion, and a patch from an older
format version is caught by the same decoder that catches it in a file.

Whatever is being edited is also written back on every change, so a reload — or
a phone deciding to discard the tab — picks up where you left off, named or
not. And when something is about to replace what is on screen that you did not
type, such as the patch running on a module you just plugged in, the unsaved
one is put in the library first rather than dropped.

The library is this browser, on this device, and nothing leaves it. `.syx` and
`.json` export are how a patch travels.

It also starts with somewhere to start: eighteen **example patches**, the ones
the emulator page used to open with, each exercising one part of the machine
and saying what to do and what to expect. Loading one is loading a file — it
arrives unsaved, so nothing you have kept is touched. `test/app.test.mjs` loads
every one of them into the real firmware and fails if it is refused, so an
example cannot rot into a patch that no longer validates.

## Two file formats, for two different readers

`.syx` is the patch **image**: the bytes the module stores and a librarian
sends. It is the right thing for hardware and unreadable by a person. It is
also the fallback for a browser with no Web MIDI — build a patch offline with
nothing plugged in, export it, and send it with any standard SysEx librarian.

`.json` is the same patch in words: named algorithms, jacks numbered from 1,
named MIDI ports, bus indices per domain. It is readable, diffable and
pasteable, it carries the globals and the controller bindings too, and it
imports back, so it is a door in both directions rather than a one-way export.

## An external controller

Web MIDI has two jobs here, and they are separate.

**Reaching a module** — the control cable — is what *connect a module* uses: an
identity request goes out on every output, and the module that answers is the
one the app talks to.

**Playing the module in the page** is what the MIDI tab's *external controller*
does. The keys and knobs on a controller plugged into the computer are routed
into the built-in module's MIDI input, on a port and channel you choose, and
everything the module plays can go back out to a real port — a DAW, a synth, or
the interface it came in on.

Routing a controller *into* the module rather than around it is what makes
**learn work with no module in the room**: an incoming CC takes the path
`main.cpp` gives it — preset recall, then NRPN, then the binding table, then the
graph — because the page offers it to the firmware's own control plane in that
order. Arm learn beside a parameter, turn a knob on the desk, and the binding is
made by the firmware.

## Browser support

Verify this against current browser support before relying on it; the landscape
moves. As of writing:

| Browser | Web MIDI | SysEx |
|---|---|---|
| Chrome, Edge, Opera | yes | with permission |
| Firefox | recent versions, behind a permission prompt | with permission |
| Safari | **no** | — |
| Any of them, mobile included | not needed for the built-in module | — |

That table is about reaching *hardware* and playing a controller. The built-in
module needs no Web MIDI at all, so the app is fully usable on any modern
browser including iOS Safari — you just cannot talk to a real module or a real
controller from one.

A browser without Web MIDI is not a degraded experience — it is a user who
cannot set their module up. So the page says plainly what is wrong, the
on-screen keyboard plays the patch anyway, a binding can be typed in by hand
with no controller present, and `.syx` export is a first-class path.

**A phone is the first target, not the fallback.** With the module in the page
there is no cable to plug in, so a phone is a complete app — and the only one
iOS can have. Every control is finger-sized, every numeric parameter has a
number field beside its slider (a slider cannot hit a value, and on a touch
screen a 1px drag is a whole step of a 255-wide range), and anything that
cannot shrink — a 32-step lane, the bindings table — scrolls inside its own box
rather than pushing the page sideways.

## How it stays honest

**No special-case firmware.** The app is a client of the protocol in
`src/protocol/sysex.h`. If it needs something the protocol does not have, the
protocol gains a message — never a private side channel — so the console, this
app and an eventual Launchpad stay interchangeable.

**Everything is read from the device.** The algorithms, **their inlets' and
outlets' names** and domains, a one-line summary of each algorithm, every
parameter's name, range, default, display kind and enum options, and the
module's real capacities all come from the registry and capability messages. An
algorithm added to the firmware appears here with a working, *described* panel
and no change to any file in this directory.

The only words not read from the device are the ones the protocol defines by
enum rather than by description — MIDI port names, clock sources, takeover
modes. Those live in `src/names.js`, keyed by the generated enum's own
identifiers and checked against them on import, so renaming one in the firmware
breaks the app loudly instead of mislabelling a cable.

**The message layout is generated, not copied.** `src/protocol.js` is derived
from the firmware headers by `tools/generate-protocol.mjs`. `make app`
regenerates it and fails if the checked-in copy differs, so a protocol change
breaks both builds at once instead of silently corrupting a patch on real
hardware. That is the reason the app lives in this repository.

**Client-side validation uses the same rules** the firmware enforces
(`src/validate.js` against `registry::validate` and
`MixedModeMaster::validate`), so an error surfaces while editing rather than on
send. An app that lets you build a patch the module will reject is worse than
no app.

**And it is tested against the real firmware.** `test/protocol.test.mjs` runs
the module — compiled to WebAssembly by `emulator/build.sh` — and talks to it
through this app's own `Device` and codec over the actual SysEx protocol, so "a
patch the app accepts is never rejected by the firmware's validator" is a
check, not a hope. `test/app.test.mjs` covers what the merge added: the patch
library, the runtime seam (a bound CC really does move a parameter through the
firmware's control plane), and the single-file build.

```sh
make app       # builds the wasm, checks the protocol module, runs both suites
```

## Layout

```
app/
  index.html          the page: one file, all the styling
  src/
    app.js            the shell - tabs, state, the patch library's commands
    module.js         the firmware in the page: transport and running machine
    device.js         the protocol client, over any transport
    protocol.js       GENERATED from the firmware headers
    codec.js          the patch image and the SysEx framing
    validate.js       the firmware's own rules, client side
    graph.js          how a node arrives connected
    views.js          the patch tab: nodes, parameters, sequencer grids
    midi.js           routing, bindings, the clock, the external controller
    perform.js        the play tab, and everything that updates live
    patches.js        the patches tab
    library.js        localStorage: the patch library and the working patch
    examples.js       the example patches, in that JSON
    controller.js     a MIDI controller plugged into this computer
    audio.js          the synth standing in for what is downstream
    webmidi.js        Web MIDI: support, discovery, the hardware transport
    names.js          the words for what the protocol carries as numbers
    patchjson.js      the patch as readable JSON, both ways
  test/
    protocol.test.mjs the app against the real firmware, over SysEx
    app.test.mjs      the library, the runtime seam, the build
  tools/
    generate-protocol.mjs   src/protocol.js, from the firmware headers
    bundle.mjs              the single-file build
```

**A node arrives connected.** Its required inlets go to a bus something already
writes — the node added last, so a chain builds in the order you build it — and
its first outlet to a bus nothing writes yet. A node added unconnected is one
the module refuses, which left the app a graph ahead of the device; from there
every incremental edit named a node the module had never taken and came back
`SYSEX_ERR_BAD_ARGUMENT`. The app tracks that divergence now (`App.diverged`):
a patch its own validator refuses is never sent, and the first edit that makes
it valid sends the whole patch instead of an addressed one.

**Dragging a connection sends one message**, not a full dump — under the bus
model a connection change is one byte, with no re-sort and no graph rebuild.
Adding or removing a node changes the graph's *shape*, so that is a whole
patch.

**Bindings are edited, not only learned.** The MIDI tab lists every controller
binding in words and makes every field of every slot editable — CC, channel,
source ports, target, sub-range, takeover, relative encoding, 14-bit pairing,
pass-through — so a binding can be built with no controller in the room, which
is the same case `.syx` export exists for.

## Still to build

- Live bus activity **from a real module**, streamed on the control cable. The
  built-in module already shows it, because the page can read its state
  directly; hardware would have to send it, which means rate-limiting it and
  making it disable-able since it competes with musical traffic.
- Per-step velocity, length, tie/rest and probability in the note lane, which
  currently shows degrees and pitches only.
- A dial-and-preview for the Euclidean parameters, which want to be turned
  rather than typed.
- Naming preset slots, which needs a place in the patch format for a name. The
  browser library names patches; the module's own slots still cannot.
- The library on more than one device, which means a file, a cable or a server
  — and the first two already work.
