# The MMMC app

One page that **is** the module: it builds a patch, runs the firmware that
plays it, and lets you hear the result while you edit. With no encoder, no
switches and no display on the hardware, this and the serial console are how
the module gets configured — a shipping deliverable, not a companion app.

## Running it

A Vite app with no framework: plain ES modules building the DOM, a stylesheet
per component, and `vite build` folding all of it and the WebAssembly module
into one HTML file.

```sh
make dev                          # the source tree with hot reload, on Vite's port
make preview                      # the built single-file page (what CI ships)
make app                          # the checks below, against the module
make stop                         # stop whichever is running
```

Both are `scripts/start_app.sh`: idempotent, detached, and they build the
module first. The dev server reaches it at `emulator/dist/mmmc.wasm` through
the `@module` alias in `vite.config.js`; the built page carries it inlined,
which is what lets `emulator/dist/index.html` open from a download with
nothing serving it. The build from `main` is live at
<https://mixedmode-fx.github.io/central/>.

## The shape of it

Six tabs — **patch**, **globals**, **MIDI**, **module**, **library**, **schema** — and
**play** as a button at the top beside *connect a module*, because those two
answer the same question: which module am I listening to, the one in the page
or the one on the cable. Play does not open a tab: it opens the performance
surface, which is a second shell with no header and no tabs.

**The transport is in the bar with the tabs**, and on the surface's own case:
start, stop, continue and **panic** are the machine's controls rather than any
panel's, so they are reachable from whatever is on screen. Panic reaches all
three places a note can be — the module's own nodes, this computer's MIDI
ports, and the page's audio — because a button that missed one would be worse
than none.

**The module runs in the page, always.** The firmware compiled to WebAssembly
is both the *transport* the editor talks to (`Device` talks to a transport, not
to Web MIDI, so every edit is the SysEx message a cable would have carried and
the validator is the firmware's own) and a *machine that runs* (the page is its
main loop, interval timer and sync pin). That is also what makes the app usable
on iOS, where Web MIDI does not exist at all.

**patch** — the graph, as a canvas: a box per node, jack and MIDI port, a
socket per port, an arrow wherever two are on the same bus, and the bar above
it is where things are added. Selecting a block opens its **details** below
the picture — every port with what it is wired to, its **signals**, and every
parameter as a control — in a panel that folds to its title bar and closes
with its cross, beside the buttons that copy, duplicate and remove the block
whole. The signals are the block's ports over time, whatever the block is: a
scope row per gate and control bus it reads or writes, a piano roll of its
note buses, a jack's own level, and a MIDI port's cables beside its buses — so
a sequencer's notes, a divider's pulses and a transformation's two sides are
each read off the block itself, in the same shades and with the same hiding
chips as the module tab's scope and roll. The picture takes the **whole window** from the button in the bar
or **F** — the canvas and the details of what is selected, and nothing else —
and **Esc** gives it back. Parameters are sorted onto the same sections on every
node (behaviour, pitch, timing, dynamics, chance, MIDI) rather than left in
the firmware's order, so a hand that has found *root* on one card finds it in
the same place on the next — unless the algorithm labels its own groups
(`src/node/param.h`), which is how a node with several controls over one
mechanism keeps them together rather than having them sorted apart by what
their names sound like. A control the firmware is currently ignoring is dimmed
and says why, rather than sweeping and changing nothing. Beside each control are two buttons, each opening a
menu: **learn** arms a learn when a controller is listening and lists the CC
numbers either way, and **CV** lists the control buses, with what writes each,
and routes one onto it. Under the graph is the **mod matrix**: every binding
and every route in the patch, and every field of a binding, so one can be
built with no controller in the room. Under that is the **macro bench**, where
a macro is named and shaped: its destinations are drawn as bands across its
travel — rising where each one acts and holding past the top of its window,
with the module's live position marked on the same axis — because four rows of
numbers do not say what a macro does. A destination that is silent because
nobody has moved the macro yet, and one pinned against the end of its target's
range, are drawn apart: they look identical in a table and are two different
problems. Sequencers get purpose-built views — a
step grid for the gate and drum sequencers, a note lane over scale degrees for
the note sequencers — and the step being played is outlined in the grid you
are editing. **What is outlined is the step that is sounding**, never the one
the next clock edge will play: one rule, in `src/ui/panels/grids/playhead.js`,
for every display that lays a pattern out in squares, because a display a step
ahead of the jack is read as the module being wrong. `Harmony` gets the
**circle of fifths**: the key's chords where
the circle puts them, and either the moves the walk would make from one of
them, each arrow weighted by how much it wants it, or the loop it has written
down as a path through them. Every note on it is spelled the way the key
spells it — a seven-note scale uses each letter once, in order — because a
chord called D♯ where E♭ belongs does not read as a spelling slip, it reads as
the degree being wrong.

**play** — the **performance surface**: sixteen pads, eight pots, the whole
viewport, and it fits a 360x640 screen with no page scroll. The geometry is
fixed and the assignment is soft — you cannot move a control, you can change
everything about what it does, which is what the module's eventual front panel
will be. A pot sends a CC; a pad sends a note, a CC, or a **Program Change**
that launches a stored patch on the next bar. A pad is momentary or latching,
and **a latch is drawn as an outline and says "last sent"**, because it is
showing what it sent and not what the module holds — the one control here that
genuinely reads back is a pot on a macro, which follows the module's own
report of where that macro is. Press to play, and hold for as long as the note
lasts: **edit** is a toggle in the bar, not a gesture, and while it is on a
press asks what a control does instead of playing it. That sheet is also where
a control is bound — picking a target arms the module's own learn, and moving
the control finishes it. The whole surface is on **one lead**, the way a
controller has one: which of the module's inputs it plays into is chosen in the
bar while edit is on, and each control keeps its own channel. What each control
sends, what it is called and what colour it is are **yours, not the patch's** — they
live in this browser beside the canvas arrangement, keyed globally rather than
per patch, because a pad that changed meaning with every patch load is one
nobody could learn. A **keyboard** is summoned over it: one scrolling row of
real keys, six octaves, a drag across it playing a glissando, and **a cable and
channel of its own** — playing a part into one input of the patch while the
pads drive another is the ordinary case.

**module** — the module running: the LEDs and gate buses, the jacks — the sync
jack among them, since no cable reaches it in a browser — an on-screen keyboard
and CC sender, and two views that answer questions no
lamp can. The **scope** draws every jack, gate bus and CV bus the patch uses
against the last few seconds, which is the only way to read a divider, a
Euclidean pattern or an LFO. The **piano roll** draws notes with a shade per
place they were seen — played in, sent out, and each note bus the patch writes
— so the same phrase is visible at every point in the chain and a disagreement
between two of them is the bug. Both have a legend of chips, and pressing a
chip hides its trace.

**Colour is the domain.** A gate is green, a note is orange and a control
signal is purple — in an arrow, a socket, a bus chip, a lamp and a trace. Where
one view has to tell several signals of one domain apart, they are shades of
that domain's colour (`shade`, in `src/scope.js`), never a borrowed one; the
blue accent marks what is selected, bound or playing, and nothing else.

*listen* is a list of **players**, each one voice pointed at what the module
sends or at a **note bus** read straight off the bus, with its own waveform and
level. A bus only leaves the module once a MIDI out is patched to it, so
listening to buses is what makes a patch audible while it is being built.
Drum machines are not players: a drum sequencer, and any node the patch has set
to **channel 10**, gets **its own kit and level** and is audible because it is
in the patch. Kits are synthesised, not sampled, so the whole app stays one
downloadable file. The gate listener blips per rising edge on a chosen gate bus
or jack, which is the only way a clock division or a logic gate is audible at
all.

**globals** — `GlobalSettings` (`src/patch/patch_codec.h`) drawn, and all of it:
the key, the clock, Program Change recall and NRPN. A setting is here exactly
when it travels with the patch and belongs to no node, which is what keeps it a
page rather than a drawer. The **key** — one scale, one root and one register
for the whole patch — is drawn on a keyboard: the notes of the key are lit, its
root is ringed, and pressing a key moves the root. Nothing in the patch names a
scale or a root of its own; a node says only which register it plays in, and its
default is the key's. The **clock**'s three settings — what drives it, how fast,
and the CV rate — are one row at every width, with the cables it follows and
clocks under them; where the clock is following something else, the panel says
so, because a tempo field that is not what the module is running at is a number
to be believed and then disbelieved.

**MIDI** — the external controller, the external MIDI out and routing: the room
the module is in, none of which a patch travels with. What a controller *moves*
is in the patch, so it is in the mod matrix; what it counts time by, and what a
Program Change does to it, belong to the module rather than to a cable, so they
are on the globals tab.

**library** — where a patch lives: this browser, a file, or the module's preset
slots.

**schema** — the patch format as a JSON Schema, generated from the attached
module, inside a prompt to hand to something that is not this editor.

## Rules the app is built on

**An arrow is drawn, not stored.** There is no cable in this machine — every
port names the *set* of buses it is on — so an arrow is the observation that a
writer and a reader share one. The canvas is a rendering of the patch and never
a second model beside it, so nothing in it can drift. What a cable would have
hidden is shown rather than papered over: one outlet on a bus two inlets read
is two arrows, and two writers of one bus are dashed arrows with a `×2` mark.
Cutting an arrow takes the *reader* off that one bus and leaves the rest of its
set alone, so removing a connection never removes another.

**Routing is the canvas's, and only the canvas's.** There is no bus selector on
a card: a dropdown can only ever name one bus, so summing two sources into an
inlet meant moving their sources onto one bus and taking whatever else that
merged with it. The cards say what a port is wired to; the drags say what it
is wired to next.

**Where a block sits is not part of a patch.** The stored patch, the `.syx`
file and the JSON dialect describe a graph. The canvas lays a patch out from
its own shape — signal left to right — and a hand-placed block is remembered in
`localStorage` as a preference about looking at it.

**A block arrives unconnected.** A node, a jack and a MIDI port all arrive on
no bus at all and the wires are the ones you drag. A bus chosen for you is an
arrow nobody drew. A patch the app's own validator refuses is never sent — a
node whose required inlet is still empty is one of those — and the first edit
that makes it valid sends the whole patch (`App.diverged`).

**A copy carries settings, not a place in the patch.** Copying a block takes
its algorithm, its parameters and the buses it *reads*, so a paste or a
duplicate arrives writing nothing: reading a bus twice is fan-out and changes
nothing that was already playing, where writing one twice is a merge — a change
to the sound of the patch, made by a gesture that only asked for another of
something. The routes and bindings pointing at the original stay with it, since
a parameter two knobs reach because a block was duplicated is a patch nobody
can read. Only a node is copied: a jack and a MIDI port are fixed resources of
the module, taken into use from the add bar. Copy, paste, duplicate and delete
are ⌘/Ctrl-C, V, D and Delete on the canvas, and buttons in the details bar and
the canvas bar besides, because a phone has no keyboard.

**A drag only ever adds.** The source keeps its bus, claiming a free one if
this is the first thing it has been asked to drive, and the target adds that
bus to the set it already reads. So a source reaches as many destinations as
you drag it to, a destination sums as many sources as you drag into it, and
neither costs the other a connection it already had.

**A drag sends one message**, not a full dump — a connection change is one
port's set of buses. Adding or removing a node changes the graph's shape, so
that is a whole patch.

**Which way a port faces is one of its settings**, not a kind of block to add.
A gate jack's direction is a firmware field, so the toggle writes it; a MIDI
port's is not, so turning one round moves what the port carries to the first
free port on the other side. Both rules are in `graph.js`.

**A MIDI port is a source and a destination**, and the card names which is
which: an input's source is its cables, an output's is its note buses, and the
legs are labelled rather than left to be read off their order. Fanning an
*output* out takes a free port for the same note buses on another cable, which
is the one thing a second port buys that the first cannot: its own channel.
There is no such action on an input — an input reaching a second note bus is
that bus in its own set, which is a drag.

**What can be added is a list you can read** (`picker.js`): rows shelved by the
**category the module reports**, each with the firmware's own summary, and a
search. An algorithm from firmware newer than the app lands under *other*
rather than disappearing.

**A binding and a route are part of the patch.** Both travel in the patch
image, both end at the same validated write in the firmware, and both are gone
when the patch is replaced — so they are read under the graph they act on, not
beside the cables and the clock, which outlive any patch. A CC is bound from
the same two places a route is made: the button beside the control, and the
matrix under the graph.

**A node wears the key it plays in.** The key is one setting for the whole
patch and lives on a page of its own — the right place to change it and the
wrong place to have to look it up. So a node whose descriptor says it reads the
key carries a badge naming it, and a cable on that node's root inlet renames it
after the chord the cable is playing: what it is called, what degree it is, and
where it sits on the circle of fifths. Which algorithms those are is the
module's own answer (`reads_key`), never a list in the app.

**A picture of an algorithm is the algorithm's own numbers.** The arrows on
the circle of fifths are `Harmony::weigh`, read off the running node through
`module.js` — not the same rule written a second time in JavaScript, which
would be a rule that could drift. A picture that disagrees with the music is
worse than no picture. It follows that such a view is drawn from the *running*
node and says so when there is not one yet, where every other panel is drawn
from the patch.

**A modulated parameter is a socket; the rest are not.** A node has anywhere
from two to `N_PARAM` parameters, so drawing them all would bury the signal
path. A parameter something modulates gets an inlet with a square dot;
dragging a control signal onto a block opens a dropdown of the ones still free,
and the CV button beside a control makes the same route from the other end.
Both build the route through `graph.js`, so neither can disagree about what a
new route does.

**Patches live in the browser**, and what is stored is the patch **in words** —
the JSON dialect, algorithms by name. Not the image: the image is a versioned
binary layout, and this project renumbers ids and moves fields whenever the
shape is wrong (CLAUDE.md, *Compatibility*), so a library of images is a
library that empties itself on the next such change. The words survive it. The
working patch is written back on every change; anything unsaved is put in the
library before something replaces it. Example patches ship with it, shelved by
what each is for: most exercise one part of the machine, and the performance
shelf holds finished pieces whose macros arrive already on the surface's pots.

**Two file formats, for two readers.** `.json` is the patch in words: what the
library keeps, what a saved patch exports as, and what a person can read, diff
and write by hand. `.syx` is the image, which is what hardware and a librarian
want and what nobody can read — and the fallback for a browser with no Web
MIDI. Both import back.

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

**The module's cables are routed out of it, one at a time.** A MIDI out node
names a mask of the module's own cables and the browser has a port per socket,
so the correspondence is chosen per cable (`runtime/midiout.js`) and a patch
reaches as many synths as it plays to. The routing is kept in this browser —
it is your desk, not your patch — and what a cable is holding is released to
the port that is holding it before it is pointed anywhere else, because a
note-off is the only thing that ends a note.

**What is live is sampled by the module, not polled by the page.** The jacks,
the gate buses, the LEDs and the note buses are read once per *pass*, and the
page folds together everything since it last painted. A trigger is high for one
or two passes and an animation frame is sixteen milliseconds, so reading at
paint time shows a pattern nobody is playing. A bus is only read when something
is listening to it.

**The module keeps wall-clock time, and what it plays is heard at that time.**
Simulated time is `performance.now()` from a fixed origin (`runtime/module.js`):
a timer and the animation frame run whatever passes the clock owes, so a late
frame is caught up rather than lost and no fraction of a frame is dropped. A
note is scheduled on the audio clock and on a Web MIDI port by its own
simulated time plus `OUTPUT_LATENCY_MS`, never by when the pass that made it
happened to run. **A hidden page keeps running.** A browser gives one no
animation frames and clamps its timers, so what runs the passes there is a
worklet on the audio render thread, which is the one clock it does not throttle
(`runtime/heartbeat.js`): tab away from a running patch and it is still playing
when you come back, rather than frozen where you left it. It arms itself on the
first press anywhere in the page, because audio starts from a gesture and
nothing else is asked of you; where a browser will not have it — no
AudioWorklet, or a phone that suspends a backgrounded tab's audio outright —
the module stops with the page and skips forward on return.
Rebuilding the page is the one stall the page can see coming,
so the renderer runs the module ahead by what the last rebuilds cost before it
starts one (`services/render.js`): an edit changes the patch and nothing about
the beat.

**A phone is the first target.** Every control is finger-sized, a button that
acts on something is an icon with its word in the tooltip and the accessible
name (`src/icons.js`), every numeric parameter has a number field beside its
slider, sequencer steps wrap onto as many rows as they need, and anything that
cannot wrap scrolls inside its own box. Two non-obvious costs: a grid or flex child is `min-width: auto`, so every
box between a 32-step lane and the page needs `min-width: 0` or the lane's
width propagates out past the screen; and a native range input takes any touch
that lands on it, so a touch has to claim a slider (drag along it, or press and
hold) before it may move it, and a gesture the browser takes for scrolling puts
the value back (`Slider`, in `src/ui/components/`).

## How it stays honest

- **No special-case firmware.** The app is a client of
  `src/protocol/sysex.h`. If it needs something the protocol has not got, the
  protocol gains a message — never a private side channel.
- **Everything is read from the device**: algorithms, port names, parameter
  names, ranges, defaults, display kinds, enum options, capacities. An
  algorithm added to the firmware appears here with a working panel and no
  change to any file in this directory. The only words not read from the device
  are the ones the protocol defines by enum — MIDI port names, clock sources,
  takeover modes — which live in `src/protocol/names.js`, keyed by the
  generated enum's identifiers and checked against them on import.
- **The message layout is generated, not copied.** `src/protocol/generated.js`
  comes from the firmware headers via `tools/generate-protocol.mjs`; `make app`
  fails if the checked-in copy has drifted. That is why the app lives in this
  repository.
- **Client-side validation uses the firmware's rules** (`src/core/validate.js`
  against `registry::validate` and `MixedModeMaster::validate`), so an error
  surfaces while editing rather than on send.
- **It is tested against the real firmware.** `test/protocol.test.mjs` drives
  the WebAssembly module through this app's own `Device` and codec over the
  actual SysEx protocol; `test/app.test.mjs` covers the library, the runtime
  seam, the drag planning, the schema both ways round, every example patch and
  the panels, built against a fake document.

## Layout

Four layers, each allowed to reach only the ones below it: the protocol, the
core, the runtime and the services are plain JavaScript with no DOM in them,
and the UI is functions that take the app and return elements.

```
app/
  index.html          the page: a root element and the entry script
  vite.config.js      the build, the dev server and the tests
  src/
    main.js           the entry point: the styles, the module, the app
    styles/           tokens, the base sheet, the layout primitives
    protocol/         the firmware's protocol: generated.js (GENERATED from
                      the headers), codec.js, device.js, names.js
    core/             the patch, with no DOM: patch.js (what more than one
                      panel asks of a patch), graph.js (blocks, arrows,
                      drags), layout.js, validate.js, patchjson.js,
                      schema.js, catalogue.js, music.js, examples.js
    runtime/          the machine and its peripherals: module.js (the
                      firmware in the page), wasm.js, webmidi.js,
                      controller.js (a controller playing it), midiout.js
                      (what it plays, out of this computer), audio/ (the
                      listener and the drum kits)
    services/         the app's state and every command on it: state.js,
                      render.js (the coalesced re-render and the per-frame
                      painters), session.js (the device), editor.js (every
                      edit), play.js (where a note goes: the module in the
                      page, or one on a cable), surface.js (the pads and
                      pots), patches.js (the library and the files),
                      arrangement.js (block positions), storage.js, app.js
                      (the composition root)
    ui/
      dom.js          el and svg
      components/     reusable widgets that know nothing about a patch, each
                      with its stylesheet beside it
      controls/       controls that read the patch: what a port is wired to,
                      a parameter, the learn and CV menus, a modulation route
      panels/         the cards, the mod matrix and the macro bench;
                      algorithms.js is the one table of algorithm-specific
                      views and inert rules
      canvas/         the patch as blocks and arrows
      surface/        the performance surface: the pads, the pots, and the
                      sheet that says what a control does
      scope/          the scope and the piano rolls
      tabs/           one file per tab
      App.js          the shell
  test/
    harness/          the real module, and the fakes for what Node lacks
    protocol.test.mjs the app against the real firmware, over SysEx
    app.test.mjs      the library, the runtime seam, the schema, the panels
  tools/
    generate-protocol.mjs   src/protocol/generated.js, from the headers
```

## Browser support

Web MIDI is Chrome, Edge and Opera with permission, Firefox behind a prompt,
and not Safari — verify against current support before relying on it. That is
only about reaching *hardware*, playing a controller and playing a synth: the
built-in module
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
