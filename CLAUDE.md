# CLAUDE.md

Firmware for a Eurorack CV & MIDI processor on a Teensy 4.1, plus a WebAssembly
build of the same core and a browser app that drives it. `README.md` is the
reference for the machine itself.

## Where things live

- `src/` — the firmware core. Freestanding: no heap after boot, no exceptions,
  no libc. `src/hal/teensy/` and `main.cpp` are the only files that know what
  a Teensy is.
- `test/` — the native unit tests, one directory per suite. They build `src/`
  minus those two.
- `emulator/` — the same core compiled to WebAssembly, with the page standing
  in for the hardware. Nothing under `src/` is duplicated to make this work.
- `app/` — the browser app, a client of the firmware's protocol. ES modules,
  no build step; `emulator/build.sh` also emits it as one file.
- `scripts/` — provisioning and the checks, reached by path from the hooks,
  from CI and from `make`.

## Workflow

**Before any check, wait for the environment**: `bash scripts/await_ready.sh`.
A container is cloned fresh for every web and mobile session with no `.venv/`
and no module. It returns instantly when the container is warm. Do not
diagnose `pio: not found`, or a page that loads with no module in it, before
it has returned.

**The commit gate**: `bash scripts/checks.sh` — the native unit tests, the
algorithm purity grep, the WebAssembly build, the module smoke test, the
generated protocol and the app's suites. Silence is the pass. It takes scopes:
`firmware`, `app`, or neither for both. The Claude `PreToolUse` hook runs it
before every `git commit`, scoped to what the commit touches, so a red branch
cannot be committed by accident.

**`src/` is both scopes.** A change to a header under `src/protocol/` breaks
the app without touching a line of JavaScript, because `app/src/protocol.js` is
generated from those headers and the module is compiled from that core. The
hook routes it to both; do not talk yourself out of the second one.

**The pre-PR gate**: `bash scripts/checks.sh && make build`. The teensy41 build
is not in the commit gate — it needs the ARM toolchain, and it is the only
thing that says the firmware still fits on the board and links against the
Teensy core.

**Done means seen, not green.** Any change visible in the app gets exercised in
the running app and shown as screenshots (load the `app-screenshots` skill),
not described:

```bash
bash scripts/start_app.sh
node .claude/skills/app-screenshots/scripts/screenshot.mjs --width phone --width desktop
```

**Always send the screenshots to the chat** with `SendUserFile`, in the same
turn you take them — do not merely save them under `.dev/screenshots/` and say
where they are. Most sessions here are read on a phone; a path is not a
picture. Send them as you go, not batched at the very end.

**A pull request closes its issues by name, one keyword each.** `Closes #1,
#2` links the first and silently drops the rest; a bare `#1` links without
ever closing. An issue the branch only advances gets `Part of #1`, so it
survives the merge.

Most sessions here are fired from a phone and read back as a pull request. A
stated bug, a scoped issue, a change already described — build it and push it.
Ask first only where the shape of the machine is a real choice: the preset
format, a new algorithm's parameters, anything that changes what a patch built
last week does when it is loaded next week.

## Code

- Everything is a `Node` reading and writing bus indices. Nothing under
  `src/algorithm/` may name a pin, a transport or include `Arduino.h`; only the
  port nodes hold an `IGpio` or an `IMidiOut`. `checks.sh` greps for this,
  because what it forbids compiles perfectly well.
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
  it; run `node app/tools/generate-protocol.mjs`.
- The app is a client of the protocol in `src/protocol/sysex.h`. If it needs
  something the protocol has not got, add a message — never a side channel.
- `.dev/` is the only runtime directory, and it is gitignored: logs, pids, the
  prepared marker, the base URL, screenshots. Nothing at the repo root.

## Documentation

There are four documents and there should not be a fifth: `README.md`,
`app/README.md`, `emulator/README.md` and this file. Do not add design notes,
audits, migration guides, changelogs or `docs/` — put the reasoning in a commit
message or a comment next to the code it explains. A `SKILL.md` under
`.claude/skills/` is not a fifth document: it is an instruction loaded for the
work it covers, and it is read by no one doing anything else.

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
