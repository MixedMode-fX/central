# CLAUDE.md

Firmware for a Eurorack CV & MIDI processor on a Teensy 4.1, plus a WebAssembly
build of the same core and a browser app that drives it. `README.md` is the
reference for the machine itself.

## Commands

```
make build     # firmware for the Teensy 4.1
make test      # native unit tests, no hardware
make app       # WebAssembly build + protocol check + the app's test suites
make size      # flash and RAM usage
```

Run `make test` before proposing a firmware change and `make app` before
proposing an app or protocol change.

## Code

- Everything is a `Node` reading and writing bus indices. Nothing under
  `src/algorithm/` may name a pin, a transport or include `Arduino.h`; only the
  port nodes hold an `IGpio` or an `IMidiOut`.
- No heap allocation after boot. Nodes are placement-new'd into a fixed pool;
  `NODE_SLOT_SIZE` is checked per class with `static_assert`.
- Sizing constants live in `src/config.h`, pins in `src/hardware.h`. Use the
  names, never the literals.
- Algorithm ids in `src/node/registry.h` are part of the preset format: append,
  never renumber.
- A node that emits a note-on owns its note-off, released with the
  transformation originally applied. Use `SoundingNotes`.
- Builds are `-Wall -Wextra -Weffc++ -Wshadow -Werror`. Warnings are errors.
- `app/src/protocol.js` is generated from the firmware headers. Do not hand-edit
  it; run `make app`.
- The app is a client of the protocol in `src/protocol/sysex.h`. If it needs
  something the protocol has not got, add a message — never a side channel.

## Documentation

There are four documents and there should not be a fifth: `README.md`,
`app/README.md`, `emulator/README.md` and this file. Do not add design notes,
audits, migration guides, changelogs or `docs/` — put the reasoning in a commit
message or a comment next to the code it explains.

When editing them:

- **Say what the code does now.** No history, no "used to", no "what changed",
  no deprecation notes, no backwards-compatibility discussion. Git has that.
- **Nothing that rots.** No test counts, line counts, file sizes, algorithm
  counts, timings or commit hashes. Name a constant (`N_NODE`) instead of its
  value; point at a file instead of restating its contents.
- **Only what the code cannot say itself.** Parameter lists, ranges and enum
  options are reported by the firmware over the protocol — do not copy them
  into a README. Document the decisions and invariants a reader could not infer.
- **Short.** One sentence beats a paragraph. Cut a section rather than let it
  drift out of date.
