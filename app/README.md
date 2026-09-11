# The MMMC app

One page that **is** the module: it builds a patch, runs the firmware that
plays it, and lets you hear the result while you edit. With no encoder, no
switches and no display on the hardware, this and the serial console are how
the module gets configured — a shipping deliverable, not a companion app.

## Running it

No build step and no dependencies: ES modules and one HTML file. Serve it
rather than opening it from disk — a browser will not load a module from
`file://` and Web MIDI needs a secure context.

```sh
make module                       # compiles the firmware to WebAssembly
python3 -m http.server 8080       # from the repository root, then /app/
make app                          # the above, plus the checks below
```

`make module` also writes `emulator/dist/index.html`, the whole app as one file
with the module embedded, which opens from a download. The build from `main` is
live at <https://mixedmode-fx.github.io/central/>.

## The shape of it

Four tabs — **patch**, **MIDI**, **library**, **schema** — and **play** as a
button at the top beside *connect a module*, because those two answer the same
question: which module am I listening to, the one in the page or the one on the
cable.

**The module runs in the page, always.** The firmware compiled to WebAssembly
is both the *transport* the editor talks to (`Device` talks to a transport, not
to Web MIDI, so every edit is the SysEx message a cable would have carried and
the validator is the firmware's own) and a *machine that runs* (the page is its
main loop, interval timer and sync pin). That is also what makes the app usable
on iOS, where Web MIDI does not exist at all.

**patch** — the graph. *blocks* is a canvas: a box per node, jack and MIDI
port, a socket per port, an arrow wherever two are on the same bus. *list* is a
card per node with a selector per port. Sequencers get purpose-built views — a
step grid for the gate and drum sequencers, a note lane over scale degrees for
the note sequencers — and the step being played is outlined in the grid you are
editing.

**play** — the module running: the LEDs and gate buses, the clock, the jacks,
an on-screen keyboard and CC sender, and two views that answer questions no
lamp can. The **scope** draws every jack and gate bus against the last few
seconds, which is the only way to read a divider or a Euclidean pattern. The
**piano roll** draws notes with a colour per place they were seen — played in,
sent out, and each note bus the patch writes — so the same phrase is visible at
every point in the chain and a disagreement between two of them is the bug.

*listen* is a list of **players**, each one voice pointed at what the module
sends or at a **note bus** read straight off the bus, with its own waveform and
level. A bus only leaves the module once a MIDI out is patched to it, so
listening to buses is what makes a patch audible while it is being built.
Drum sequencers are not players: each gets **its own kit and level** and is
audible because it is in the patch. Kits are synthesised, not sampled, so the
whole app stays one downloadable file. The gate listener blips per rising edge
on a chosen gate bus or jack, which is the only way a clock division or a logic
gate is audible at all.

**MIDI** — the external controller, the controller bindings, routing, the clock
and Program Change recall. Every field of every binding is editable, so a
binding can be built with no controller in the room.

**library** — where a patch lives: this browser, a file, or the module's preset
slots.

**schema** — the patch format as a JSON Schema, generated from the attached
module, inside a prompt to hand to something that is not this editor.

## Rules the app is built on

**An arrow is drawn, not stored.** There is no cable in this machine — an
outlet writes a bus and an inlet reads one — so an arrow is the *observation*
that two ports share a bus. The canvas is a rendering of the patch and never a
second model beside it, so nothing in it can drift. That costs three things a
cable would have hidden, each shown rather than papered over: one outlet on a
bus two inlets read is two arrows; two outlets on one bus are dashed arrows and
a `×2` mark; and disconnecting takes the *inlet* off its bus, saying in words
what else stopped hearing it.

**Where a block sits is not part of a patch.** The stored image, the `.syx`
file and the JSON dialect describe a graph. The canvas lays a patch out from
its own shape — signal left to right — and a hand-placed block is remembered in
`localStorage` as a preference about looking at it.

**A node arrives connected.** Required inlets go to a bus something already
writes (the node added last, so a chain builds as you type), the first outlet
to a bus nothing writes. A patch the app's own validator refuses is never sent;
the first edit that makes it valid sends the whole patch (`App.diverged`).

**A drag sends one message**, not a full dump — under the bus model a
connection change is one byte. Adding or removing a node changes the graph's
shape, so that is a whole patch.

**Which way a port faces is one of its settings**, not a kind of block to add.
A gate jack's direction is a firmware field, so the toggle writes it; a MIDI
port's is not, so the toggle moves what the port carries to the first free port
on the other side. Both rules are in `graph.js`.

**What can be added is a list you can read** (`picker.js`): rows shelved by the
**category the module reports**, each with the firmware's own summary, and a
search. An algorithm from firmware newer than the app lands under *other*
rather than disappearing.

**A modulated parameter is a socket; the rest are not.** A node has anywhere
from two to `N_PARAM` parameters, so drawing them all would bury the signal
path. A parameter something modulates gets an inlet with a square dot;
dragging a control signal onto a block opens a dropdown of the ones still free.

**Patches live in the browser**, and what is stored is the patch **image** —
the same bytes a `.syx` file carries and a slot holds, not a third format to
keep in step with the firmware. The working patch is written back on every
change; anything unsaved is put in the library before something replaces it.
Example patches ship with it, each exercising one part of the machine.

**Two file formats, for two readers.** `.syx` is the image, which is what
hardware and a librarian want and what nobody can read — and the fallback for a
browser with no Web MIDI. `.json` is the same patch in words, and it imports
back.

**The schema is generated from the device**, so it describes the module in
front of you, including firmware the app has never heard of. It is written with
no whitespace and says each thing once — shared conventions on the node schema,
repeated parameter shapes in `$defs` — because a prompt that does not fit in a
context window is not a prompt. Tests hold that line: no whitespace, no
parameter body spelled out twice, and a cap per algorithm rather than a cap on
the whole file.

**A controller is routed *into* the module**, not around it, which is what
makes learn work with no module in the room: an incoming CC takes the path
`main.cpp` gives it — preset recall, NRPN, the binding table, then the graph.

**What is live is sampled by the module, not polled by the page.** The jacks,
the gate buses, the LEDs and the note buses are read once per *pass*, and the
page folds together everything since it last painted. A trigger is high for one
or two passes and an animation frame is sixteen milliseconds, so reading at
paint time shows a pattern nobody is playing. A bus is only read when something
is listening to it.

**A phone is the first target.** Every control is finger-sized, every numeric
parameter has a number field beside its slider, sequencer steps wrap onto as
many rows as they need, and anything that cannot wrap scrolls inside its own
box. Two non-obvious costs: a grid or flex child is `min-width: auto`, so every
box between a 32-step lane and the page needs `min-width: 0` or the lane's
width propagates out past the screen; and a native range input takes any touch
that lands on it, so a touch has to claim a slider (drag along it, or press and
hold) before it may move it, and a gesture the browser takes for scrolling puts
the value back (`slider`, in `src/views.js`).

## How it stays honest

- **No special-case firmware.** The app is a client of
  `src/protocol/sysex.h`. If it needs something the protocol has not got, the
  protocol gains a message — never a private side channel.
- **Everything is read from the device**: algorithms, port names, parameter
  names, ranges, defaults, display kinds, enum options, capacities. An
  algorithm added to the firmware appears here with a working panel and no
  change to any file in this directory. The only words not read from the device
  are the ones the protocol defines by enum — MIDI port names, clock sources,
  takeover modes — which live in `src/names.js`, keyed by the generated enum's
  identifiers and checked against them on import.
- **The message layout is generated, not copied.** `src/protocol.js` comes from
  the firmware headers via `tools/generate-protocol.mjs`; `make app` fails if
  the checked-in copy has drifted. That is why the app lives in this repository.
- **Client-side validation uses the firmware's rules** (`src/validate.js`
  against `registry::validate` and `MixedModeMaster::validate`), so an error
  surfaces while editing rather than on send.
- **It is tested against the real firmware.** `test/protocol.test.mjs` drives
  the WebAssembly module through this app's own `Device` and codec over the
  actual SysEx protocol; `test/app.test.mjs` covers the library, the runtime
  seam, the drag planning, the schema both ways round, every example patch and
  the single-file build.

## Layout

```
app/
  index.html          the page: one file, all the styling
  src/
    app.js            the shell: tabs, state, the library's commands
    module.js         the firmware in the page: transport and running machine
    device.js         the protocol client, over any transport
    protocol.js       GENERATED from the firmware headers
    codec.js          the patch image and the SysEx framing
    validate.js       the firmware's own rules, client side
    graph.js          the patch's shape: connections, drags, port direction
    layout.js         where a block sits, and where its sockets are
    canvas.js         the patch drawn: blocks, arrows, dragging, the inspector
    picker.js         the add list, shelved by category
    views.js          node and jack cards: parameters and sequencer grids
    midi.js           routing, bindings, the clock, the external controller
    perform.js        the play surface, and everything that updates live
    scope.js          the scope and the piano roll
    library.js        the library tab
    schema.js         the patch format as a JSON Schema, read from the module
    storage.js        localStorage: the library, the working patch, the monitor
    examples.js       the example patches
    controller.js     a MIDI controller plugged into this computer
    audio.js          the players, the drum voices and the gate listener
    drums.js          the drum kits, and which drum a lane or a note means
    webmidi.js        Web MIDI: support, discovery, the hardware transport
    names.js          the words for what the protocol carries as numbers
    patchjson.js      the patch as readable JSON, both ways
  test/
    protocol.test.mjs the app against the real firmware, over SysEx
    app.test.mjs      the library, the runtime seam, the schema, the build
  tools/
    generate-protocol.mjs   src/protocol.js, from the firmware headers
    bundle.mjs              the single-file build
```

## Browser support

Web MIDI is Chrome, Edge and Opera with permission, Firefox behind a prompt,
and not Safari — verify against current support before relying on it. That is
only about reaching *hardware* and playing a controller: the built-in module
needs none of it, so the app is fully usable on any modern browser including
iOS Safari. A browser without Web MIDI is not a degraded experience, it is a
user who cannot set their module up, so the page says plainly what is wrong and
`.syx` export is a first-class path.

## Not built

- Live bus activity **from a real module**, streamed on the control cable. It
  would have to be rate-limited and disable-able, since it competes with
  musical traffic.
- Per-step velocity, length, tie/rest and probability in the note lane.
- A dial-and-preview for the Euclidean parameters.
- Naming preset slots, which needs a place in the patch format for a name.
- The library on more than one device — a file or a cable already works.
