---
name: app-screenshots
description: Run the MMMC app locally and photograph it with Playwright. Use whenever a change to the app has to be shown rather than described, or when asked to start, run or screenshot the app — captures any tab at phone and desktop widths.
---

# Screenshot the app

A layout claim nobody can see is not evidence. Every visual change is
photographed at the widths it crosses — before as well as after when the point
of the change is how it looks.

This matters more here than in most repos, because the person reading the
result is on a phone. They cannot check out the branch, they cannot attach a
module, and the app *is* the module's only interface. The screenshot is the
review.

## 1. Start the app

```bash
bash scripts/start_app.sh
```

Idempotent — reuses whatever is already answering. It builds the WebAssembly
module first, because the page without one is a shell with `connect` and
nothing behind it. Prints the base URL and writes it to `.dev/base_url`, where
the screenshot script looks for it. Logs land in `.dev/app.log`; stop with
`bash scripts/stop_app.sh`.

Two 404s for `mmmc.wasm` in that log are not a fault: `CANDIDATE_PATHS` in
`app/src/module.js` tries the Pages layout and a copy beside the page before
the one this server has. The one that answers 200 is the third.

## 2. Photograph it

```bash
node .claude/skills/app-screenshots/scripts/screenshot.mjs --width phone --width desktop
```

| Option | Meaning |
| --- | --- |
| `--width phone\|tablet\|desktop\|<px>` | Repeatable. phone=390, tablet=768, desktop=1440. |
| `--path <path>` | Page or `#fragment` to open. Default `/`. |
| `--click <selector>` | Repeatable, applied in order — open a tab, expand a panel. |
| `--theme light\|dark` | Force a colour scheme rather than taking the runner's. |
| `--full-page` | Capture the whole scroll height, not just the viewport. |
| `--wait <ms>` | Settle time before the shot, for the clock or a trace to fill. |
| `--name <slug>` | Filename prefix. |
| `--base <url>` | Override `.dev/base_url`. |

PNGs land in `.dev/screenshots/` and every written filename is printed.

## 3. Send them to the chat

**This is the step that is actually the deliverable.** Read the images back
and attach them with `SendUserFile`, in the same turn you take them — do not
save them under `.dev/screenshots/` and write a sentence saying where they
are. A path is not a picture, and the person reading this is on a phone.

Send them as you go, not batched at the end.

## Common shots

The app opens on **patch**; every other tab is a click, so a shot of anything
else starts with one.

```bash
S=.claude/skills/app-screenshots/scripts/screenshot.mjs

# The patch tab at both widths
node $S --width phone --width desktop

# Another tab — the label is the button's text
node $S --click 'text=MIDI' --name midi --full-page
node $S --click 'text=library' --name library

# The module running: play, then let the clock turn over before the shot
node $S --click 'text=play' --wait 2000 --name play --full-page

# A node's details panel, which is where most parameter work shows up
node $S --click 'text=Metronome' --name details --full-page
```

A trace view — the scope, the piano roll — is empty until the module has run.
`--wait` is not optional there: a scope photographed at t=0 is a picture of an
empty grid, and it looks exactly like a scope that is broken.

## Troubleshooting

- **Nothing answering** → `bash scripts/start_app.sh` first; check `.dev/app.log`.
- **A page with no module** — `connect` offered and no `connected · built-in`
  under the title — means `emulator/dist/mmmc.wasm` is missing or stale. Run
  `emulator/build.sh`, or `bash scripts/await_ready.sh --refresh`.
- **A blank or half-drawn page** → the script prints console and page errors it
  saw; read those before re-shooting.
- **No browser** → it falls back through `PLAYWRIGHT_CHROMIUM_EXECUTABLE`,
  `/opt/pw-browsers/chromium` and the system chromium before giving up.
- **A collapsed panel photographs as a title bar** → that is the shot the
  reviewer will disbelieve. `--click` it open first.
- **A band of empty background above a sticky header** is the script, not the
  page: it is what a torn frame looks like when the shot lands mid-scroll. It
  should not happen — the runner forces reduced motion and waits for scrolling
  to stop — but if you ever see one, re-shoot before reporting a layout bug.
