# MMMC patch editor

A browser editor for the module, over Web MIDI (#12).

`src/hardware.h` declares no encoder, no switches and no display, so there is
**no human input on the module at all**. This is not a nicer alternative to a
cramped panel menu — with the serial console as the only other option, it is
how the module gets configured. It is a shipping deliverable, not a companion
app.

## Running it

There is no build step and no dependencies: it is ES modules and one HTML file.
Web MIDI needs a **secure context**, so serve it rather than opening it from
disk:

```
python3 -m http.server -d editor 8080     # then open http://localhost:8080
```

GitHub Pages satisfies the secure-context requirement too, which is the
intended home for it.

## Browser support

Verify this against current browser support before relying on it; the landscape
moves. As of writing:

| Browser | Web MIDI | SysEx |
|---|---|---|
| Chrome, Edge, Opera | yes | with permission |
| Firefox | recent versions, behind a permission prompt | with permission |
| Safari | **no** | — |

A browser without Web MIDI is not a degraded experience — it is a user who
cannot set their module up. So the page says plainly what is wrong, and
**`.syx` export is a first-class path, not an afterthought**: build a patch
offline with nothing plugged in, export it, and send it with any standard SysEx
librarian. Import reads back either an exported `.syx` or a raw patch image.

## How it stays honest

**No special-case firmware.** The editor is a client of the protocol in
`src/protocol/sysex.h`. If it needs something the protocol does not have, the
protocol gains a message — never a private side channel — so the console, this
editor and an eventual Launchpad stay interchangeable.

**Everything is read from the device.** The algorithms, their inlets and
outlets and domains, every parameter's name, range, default, display kind and
enum options, and the module's real capacities all come from the registry and
capability messages. An algorithm added to the firmware appears here with a
working panel and no change to any file in this directory.

**The message layout is generated, not copied.** `src/protocol.js` is derived
from the firmware headers by `tools/generate-protocol.mjs`. `make editor`
regenerates it and fails if the checked-in copy differs, so a protocol change
breaks both builds at once instead of silently corrupting a patch on real
hardware. That is the reason the editor lives in this repository.

**Client-side validation uses the same rules** the firmware enforces
(`src/validate.js` against `registry::validate` and
`MixedModeMaster::validate`), so an error surfaces while editing rather than on
send. An editor that lets you build a patch the module will reject is worse
than no editor.

**And it is tested against the real firmware.** `test/protocol.test.mjs` runs
the module — compiled to WebAssembly by the emulator build — and talks to it
through this editor's own `Device` and codec over the actual SysEx protocol. So
"a patch the editor accepts is never rejected by the firmware's validator" is a
check, not a hope:

```
make emulator      # builds the wasm the test drives
make editor        # regenerates the protocol module and runs the checks
```

## What the views do

**Buses are the connections.** Every inlet and outlet is a bus selector
offering only the buses of its own domain, because that is all the module will
accept. There is no cable to drag and no cable to lose: a patch reads as a list
of what each node reads and writes.

**Dragging a connection sends one message**, not a full dump — under the bus
model a connection change is one byte, with no re-sort and no graph rebuild.
Adding or removing a node changes the graph's *shape*, so that is a whole
patch.

The generic parameter view is wrong for a sequencer — nobody enters a drum
pattern as a list of numbers — so those have their own:

- **Step grid** for `StepSequencer` and both drum sequencers: lanes down, steps
  across, click to toggle, with each lane's own length visible, since lanes can
  differ and that is the polyrhythm the node exists for.
- **Note lane** for the mono and poly sequencers, over **scale degrees**, which
  is what the sequencer stores. The pitch each degree resolves to against the
  current root and scale is shown beside it, so changing the root visibly moves
  the pitches without touching the stored pattern.

## Still to build

- Live bus activity streamed on the control cable — with no panel feedback
  beyond two LEDs, a view of which buses are firing is the module's missing
  display. It has to be rate-limited and disable-able, because it competes with
  musical traffic.
- Per-step velocity, length, tie/rest and probability in the note lane, which
  currently shows degrees and pitches only.
- A dial-and-preview for the Euclidean parameters, which want to be turned
  rather than typed.
- Naming preset slots, which needs a place in the patch format for a name.
