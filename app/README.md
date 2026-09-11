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

## Four tabs, and a place to go and listen

The app is four tabs — **patch**, **MIDI**, **library**, **schema** — and one
button at the top of the page beside *connect a module*: **play**.

That is the shape because *play* is not a fourth thing to edit. It is the
module *running*, and the question it answers is the same one *connect a
module* answers: which module am I listening to, the one in this page or the
one on the cable? The two belong together, above the tabs, rather than one
being a tab and the other a button. Leaving *play* goes back to whichever tab
it was entered from.

The library was called *patches*, with a button in the header that went to the
tab of that name directly below it. One name for one thing, in one place.

**patch** — the graph, drawn or spelled out. **blocks** is the canvas: a box
per node, jack and MIDI port, a socket per inlet and outlet, and an arrow
wherever two of them are on the same bus. Drag from a socket to another to
connect them, drag a block to move it, click one to edit it in full below the
picture. **list** is what the tab always was: every node's card, one after
another, with a selector per port. The two are one patch and one set of edits —
see below.

Either way, a sequencer gets a purpose-built view — a step grid for the gate
and drum sequencers, a note lane over scale degrees for the note sequencers —
and while the patch runs, **the step being played is outlined in the same grid
you are editing**.

**play** — the module, running. Two LEDs and the gate buses, the clock's
transport and tempo, the eight jacks (tap, hold or free-run an input; an output
lights when the firmware drives it), an on-screen keyboard and CC sender that
go in through the module's MIDI input on a chosen port and channel, a small
synth and a drum kit per drum sequencer so the patch can be heard, and the MIDI
the module is sending.

It also has the two views that answer a question no lamp can, because the
answer only exists over time:

* **the scope** — every jack and every gate bus the patch uses, drawn against
  four seconds. A divider, a Euclidean sequencer, a clock and a logic gate are
  all things whose output *is* a pattern in time: a dot that blinks says a gate
  fired, and two dots blinking say nothing at all about whether the second is
  half the rate of the first. Two traces on one axis are read at a glance. It
  was dropped when the emulator page became this tab and should not have been.
* **the piano roll** — every note of the last eight seconds on a pitch-against-
  time grid, with a colour for each place the note was seen: played *into* the
  module, sent *out* of it, and **each note bus the patch writes**. The same
  phrase appears more than once — a sequencer writes a bus, an arpeggiator
  reads it and writes another, a MIDI out sends that one — and a pitch lane is
  split between the sources playing in it, so the copies sit beside each other
  rather than on top. Where two of them disagree is the bug. The log below is
  the same events in words, which is the right form for a CC or a Program
  Change and the wrong one for a melody.

Both draw from the module's own per-pass sampling, so what is on the screen is
what the firmware did rather than what the page caught it doing.

**And you choose what to listen to.** *listen* is a list of **players**, each
one voice pointed at one thing:

* **what the module sends** — the MIDI leaving a MIDI output node, which is
  what a synth on the far end of the cable would receive. Complete, and useless
  while a patch is being built: a bus only leaves the module once somebody has
  patched a MIDI out to it, so a sequencer feeding an arpeggiator feeding
  nothing is silent no matter how right it is.
* **a note bus** — the patch's own signal, read straight off the bus, patched
  to an output or not.

Add as many as there are things to hear: the sequencer on bus 0 through a saw,
the arpeggiator on bus 2 through a square, each at its own level, because
"which of these two is wrong" is a question about hearing them apart. Each
player says in words what writes the bus it is on, so the list is not eight
identical "note bus N".

**Drums are not players.** A drum sequencer is an instrument, not a source
somebody might point a sawtooth at, so it is not in that list: every drum
sequencer in the patch gets **its own kit and its own level** — acoustic, 808,
909, drum synth — and it is audible because it is *in the patch*, not because
somebody added a player for it. Two drum machines in one patch are two rows,
which is what makes "which of these two am I hearing" a question with an
answer.

What a lane plays is read from the patch rather than guessed. A `DrumSeqMidi`
lane sends a note number, so the drum is the one General MIDI names — 36 a
kick, 42 a closed hat — and a note number outside that map gets a tuned
percussion voice at its own pitch. A `DrumSeqGate` lane sends a gate and no
note number at all, so the drum is the one the *firmware* would send for that
lane; `app/test/app.test.mjs` reads those defaults out of
`src/algorithm/sequencer/drum_sequencer.cpp` and fails if the two ever
disagree, because a lane renamed in the firmware and not here would be a snare
on the kick's lane, silently, for ever.

The kits are **synthesised rather than sampled**, and that is a choice rather
than a shortcut: the build that matters is one HTML file that opens from a
download with nothing serving it, and a sample set is megabytes of audio a
`file://` page cannot fetch. The machines were synthesisers anyway — the 808's
hat is six square oscillators through a high-pass, not a recording — so every
drum here is at most four ingredients (a pitched body, a band of noise, a
cluster of squares, a click), and a kit is those four with different numbers.
`drums.js` has the recipes.

Nothing plays a drum twice. A hit is played by the sequencer that wrote it,
never by the player that happened to carry it, and a drum sequencer patched to
a MIDI output — which sends the same hit down the cable as well — is heard once,
on the bus, where the sequencer that made it is.

**The gate listener is the third thing, and it says which gate.** A gate
carries no note and no velocity, so a clock division, a Euclidean pattern or a
logic gate sends no MIDI and cannot be heard as music — but it can be heard as
percussion, a blip per rising edge. What it listens to is a list, of both
kinds: a **gate bus**, the signal inside the module whether or not anything is
patched to it, and a **jack**, that same signal on the outside where a cable
would be. The two are worth telling apart — a bus nothing is patched to is
exactly the one nothing else can make audible, and a jack is what says whether
the signal made it out — and the default is what it always did, a blip on every
output jack. It has its own level for the same reason the drums do: it is
percussion under the notes rather than part of them.

The whole setup is kept in `localStorage`, because it is a page of choices
about a patch and losing it on every reload is the same annoyance as losing the
patch.

The pass interval, the speed multiplier, single-stepping, the bus tables and
the registry listing did *not* come back: those are instruments for debugging
the *simulation*, and this tab answers questions about the *patch*. Anything
that needs them has the native tests, `emulator/test/smoke.mjs` and a debugger.

**MIDI** — an external controller, the controller bindings, the routing, the
clock and Program Change recall.

**library** — where a patch lives: this browser, a file, or the module's own
preset slots.

**schema** — what the module can do, in a form something that is not this
editor can read: a JSON Schema of the patch format, generated from the attached
module, inside a prompt to copy. See below.

## Two ways of reading one patch

The list of cards says what a patch *contains*. It cannot say what a patch
*is*, because the shape of a patch is which thing feeds which — and under the
bus model that shape is spread across a dozen selectors reading "gate bus 2".
Following a signal meant reading every card and matching numbers. The canvas
draws the matching.

**An arrow is drawn, not stored.** There is no cable in this machine: an outlet
writes a bus and an inlet reads one, so an arrow is the *observation* that two
ports are on the same bus. The canvas is therefore a rendering of the patch
itself and never a second model beside it — there is nothing in it that can
drift out of step with what the module is running, a bus changed from a
selector in the list moves the arrow, and a patch that arrives from a file or
off a module is drawn without having been drawn before.

That honesty costs three things a cable would have hidden, and each is shown
rather than papered over:

* one outlet on a bus two inlets read is **two arrows**, made by one bus
  selection. A drag onto a socket that is already on a bus adds a listener to
  it rather than moving it, which is what makes "one sequencer, three things
  reading it" the easy shape to build;
* two outlets on one bus are **two sources merged** — legal, occasionally
  deliberate, and what a patch that "plays two sequences at once" usually turns
  out to be. Those arrows are dashed and the bus is marked `×2` below the
  canvas;
* removing one arrow of either shape **cannot be done without moving a port off
  its bus**, which changes the other arrows. Disconnecting takes the *inlet*
  off, and says in words what else that inlet has stopped hearing.

A drag is refused before it is made rather than after: a domain mismatch is
something the module rejects, so while a connection is being dragged only the
sockets that could take it are lit. And a jack or a MIDI port is only in a
patch while it is on a bus — the module validates the pair — so those have no
disconnection, only another bus or removal. `app/test/app.test.mjs` drives all
of this against the firmware's own validator: what a drag produces, the module
accepts.

**Where a block sits is not part of a patch.** The image the module stores, the
`.syx` file and the JSON dialect describe a graph and say nothing about a
canvas, and adding a coordinate to any of them would be a change to the
firmware's format for the benefit of one view in one app. So the canvas lays a
patch out from its own shape — signal left to right, each block one column
right of the furthest-right thing that writes to it, with cycles laid flat
rather than hung on — and a block dragged somewhere by hand is remembered
beside the patch in `localStorage`, as a preference about looking at it. A
patch nobody has arranged simply gets the automatic layout, which is what makes
one pasted from a chat readable the moment it opens.

**And the list did not go away.** A phone opens on it: a 200px block is not
where a 32-step lane or a slider per parameter belongs, and a canvas that took
every one-finger drag would be a page you could not scroll past. The canvas is
the default where there is room to draw one, either can be chosen at any width,
and the choice is remembered.

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

It also starts with somewhere to start: twenty-three **example patches**, the ones
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

## A schema, so something else can write the patch

The editor can build any patch this module runs, because everything it knows
about the machine it read *from* the machine: the algorithms, their inlets and
outlets and domains, every parameter's range and meaning, the bus counts. Ask
anything else for a patch — a language model, a script, a person with a text
editor — and none of that is available to it. The `.json` above shows the
*shape* of a patch and not one of the rules it has to obey, so what comes back
names an algorithm this firmware has not got, or writes a bus that does not
exist, or a parameter twice its range.

The **schema** tab is the answer, and it is the same answer the rest of the app
gives: ask the module. `schema.js` turns what the device reported into a **JSON
Schema** of the JSON dialect above — every algorithm by name, each with its
connections in the firmware's own order and each parameter with its range, its
enum options and what its default is; the sequencer sugar; the module's real
jack, bus, node and slot counts. Nothing about any algorithm is written there,
so the schema describes *the module in front of you*, including one running
firmware this app has never heard of.

The page hands it over inside a prompt — the rules that are about the machine
rather than about JSON, a worked example patch, optionally the patch on screen
as the thing to change — with the answer coming back to a box on the same page
that loads it into the editor.

**The schema is written with no whitespace in it, and says each thing once.**
Neither is a detail: at thirty-odd algorithms the readable version came to
205 kB, which is more than a small model will read at all, and a prompt that
does not fit in a context window is not a prompt.

Indentation was more than half of it — two spaces a level and a line per brace,
laid out for a reader who does not exist, since what a person reads about an
algorithm is the panel on the patch tab. Repetition was most of the rest, and
in two kinds. The **conventions** are true of every algorithm — that a null is
"not connected", that a trailing run of parameters may be left out, that 0
means a parameter's own default — so they are stated once, on the node schema,
rather than in a sentence beside every socket and every byte. The
**vocabulary** repeats because the machine repeats: thirty-six algorithms share
a time in milliseconds, a MIDI channel, a scale, a step direction, so a
parameter shape used twice moves into `$defs` and is referred to. What never
moves is the firmware's *name* for a parameter on an algorithm, because that is
the part that differs between two uses of one range and the part a reader is
looking for.

205 kB → 95 kB → **60 kB**, with every constraint still in it. Three tests hold
the line: the text carries no whitespace, no parameter body is spelled out
twice, and the schema stays under two kilobytes per algorithm — per algorithm,
so a firmware that grows does not fail it and one that starts repeating itself
does. Which is the point of describing the format the
library tab already reads rather than inventing one for the occasion: an answer
that validates is an answer the editor can load, and loading it puts it through
`fromPatchJson` and then the firmware's own validator, which is what decides
whether a patch is real.

`app/test/app.test.mjs` checks the schema against the module both ways round:
every example patch has to pass it, and a patch the firmware refuses — an
algorithm it has not got, a bus past the last one, a parameter above its range,
more nodes than fit — has to fail it. A schema that is wrong about the firmware
is a bug nothing on the page would show.

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
number field below its slider (a slider cannot hit a value, and on a touch
screen a 1px drag is a whole step of a 255-wide range), a sequencer's steps
wrap onto as many rows as they need rather than running off the side, and
anything that genuinely cannot wrap — the bindings table — scrolls inside its
own box rather than pushing the page sideways.

Two things that takes, which are not obvious:

* **A grid or flex child is `min-width: auto`**, meaning "at least as wide as
  what is inside me". A 32-step lane on one line is 925px of inside, so without
  a `min-width: 0` on every box between the lane and the page, that width
  propagates out through the node card and past the screen — where the page's
  `overflow-x: hidden` clips it. The lane does not scroll: it and half the card
  around it are simply unreachable.

  On a phone a lane does not run off the side at all: it **wraps**, eight steps
  to a row so the break always falls where a bar line would, sixteen on a
  tablet, and the whole pattern on one line only where one line fits. A step
  you have to scroll to is a step you tap by mistake. Where the pattern does go
  on one line, every lane shares *one* scroller with its name stuck to the left
  edge, so the lanes stay readable against each other — and the app puts each
  scroller back where it was after a render, because the page is rebuilt on
  every edit.
* **A native range input takes any touch that lands on it**: the value jumps to
  the finger, the page then scrolls under it, and the gesture ends by writing a
  value nobody chose. So a touch has to claim a slider before it may move it —
  by dragging along it, or by pressing and holding — and a gesture the browser
  takes for scrolling puts the value back (`slider`, in `src/views.js`).
  Scrolling past a slider is not an edit.

## How it stays honest

**No special-case firmware.** The app is a client of the protocol in
`src/protocol/sysex.h`. If it needs something the protocol does not have, the
protocol gains a message — never a private side channel — so the console, this
app and an eventual Launchpad stay interchangeable.

**Everything is read from the device.** The algorithms, **their inlets' and
outlets' names** and domains, every parameter's name, range, default, display
kind and enum options, and the module's real capacities all come from the
registry and capability messages. An algorithm added to the firmware appears
here with a working panel and no change to any file in this directory.

**The interface carries labels, not prose.** Every control is named, every
state is said in as few words as it takes, and nothing on screen explains
itself at length: the explanations are in this file, where they can be read
once instead of being scrolled past on every edit. The algorithm summaries the
registry sends are still read off the device; nothing prints them.

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

**What is live is sampled by the module, not polled by the page.** The jacks,
the gate buses and the LEDs are read once per *pass* - once per simulated
millisecond - and the page takes everything that has been high since it last
painted, folded together with what is high now.

This is not an optimisation, it is the difference between a light that means
something and one that does not. A trigger on this machine is high for one or
two passes; an animation frame is sixteen and the timer that used to drive
these was a hundred. Reading the levels *at* the paint therefore caught a
trigger about one time in fifty, so the gate dots blinked at random and the
pattern they showed was not the pattern being played. Sampled per pass an edge
can be one frame late; it cannot be missed. The scope's four-second ring buffer
and the piano roll's notes are filled from the same place, which is why they
can be trusted about exactly the signals they exist to show.

A note bus is read the same way and for the same reason. The buses are
double-buffered and `pass()` swaps once, so reading the queue straight after
`emu_pass()` yields exactly what that pass wrote — every event once, none
twice. A bus is only read when something is listening to it (a player, or the
piano roll following the patch), so the buses nothing is watching cost nothing.

The same shape of bug had the MIDI monitor stop after two hundred messages: the
log is a ring, so once it is full its *length* never changes again, and the
view redrew on a change of length. It counts events now (`midiSeq`), which is
why "it fills up, freezes, and refills when I press clear" cannot happen again
- and it stops following the tail while you are scrolled back through it.

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
    graph.js          the patch's shape: how a node arrives connected, what
                      blocks and arrows a patch has, and what a drag means
    layout.js         where a block sits, and where its sockets are
    canvas.js         the patch drawn: blocks, arrows, dragging, the inspector
    picker.js         the add list: what can be added, shelved by what it is,
                      and the dropdown that shows it
    views.js          the node cards and the jack cards: parameters, sequencer
                      grids, and which way a jack faces
    midi.js           routing, bindings, the clock, the external controller
    perform.js        the play surface, and everything that updates live
    scope.js          the two time views: the scope and the piano roll
    library.js        the library tab
    schema.js         the patch format as a JSON Schema, read from the module,
                      and the page that hands it over in a prompt
    storage.js        localStorage: the library, the working patch, the monitor
    examples.js       the example patches, in that JSON
    controller.js     a MIDI controller plugged into this computer
    audio.js          the ears: the players, the drum voices and the gate
                      listener, standing in for what is downstream
    drums.js          the drum kits, and which drum a lane or a note means
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
That is as true of a connection dragged between two sockets on the canvas as of
one chosen from a selector in the list: both are the same edit, planned by the
same code in `graph.js`, and neither is a wire the app then has to remember.
Adding or removing a node changes the graph's *shape*, so that is a whole
patch.

**What can be added is a list you can read.** A native `<select>` can say one
line about an option, so thirty algorithms were thirty labels doing the work of
a description and none of them could say what the algorithm *does* — while the
registry has carried a one-line summary for every one of them since the port
names landed. The add list is built instead (`picker.js`): rows shelved by the
**category the module reports** for each algorithm, each carrying its name,
what it costs in connections and the firmware's summary, with a search over all
of it. The shelves are the firmware's (`AlgorithmCategory`), not a table here,
so an algorithm added to the firmware still arrives filed; one from firmware
newer than the app lands under *other* rather than disappearing.

**Which way a port faces is one of its settings.** A jack and a MIDI port used
to be offered twice each in that list — in and out as separate things to add —
so the direction was chosen before the port existed and changing it meant
deleting a block and adding its opposite on the same bus. Both are now added
facing the way most patches want, with the direction a toggle in the port's own
card. A gate jack's direction is a field the firmware has, so the toggle writes
it and the bus travels with the turn. A MIDI port's is not — four inputs and
four outputs are eight different ports — so the toggle moves what the port
carries to the first free port on the other side and leaves the one it came
from unused. Both rules live in `graph.js` (`planJackDirection`,
`planPortFlip`) with everything else that decides the shape of a patch, so both
are tested against the firmware's validator rather than against a view.

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
