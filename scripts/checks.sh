#!/usr/bin/env bash
# The commit gate — one home for the fast quality checks, shared by the Claude
# PreToolUse hook, by CI, and by anyone typing `make checks`.
#
#   bash scripts/checks.sh             everything
#   bash scripts/checks.sh firmware    the native unit tests, and the purity grep
#   bash scripts/checks.sh app         the module, the protocol, the app's tests
#
# Prints a failure report and exits 1; exits 0 silently on success.
# Silence is the pass.
#
# One home is the entire point. A hook with its own copy of the command list
# drifts from CI within a fortnight, and the drift is only ever discovered by
# a pull request that was green locally.
#
# What belongs here: everything fast enough to sit between a change and a
# commit. Both scopes are under a minute — the native tests because they are
# host builds with no Teensy in sight, the app because the wasm link is
# seconds and the tests are node. What does not belong: the teensy41 build,
# which needs the ARM toolchain and proves something only a flash proves. CI
# does that on every push.
set -uo pipefail

repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || repo_root=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo_root"

scopes=("$@")
if [[ ${#scopes[@]} -eq 0 ]]; then
  # A bare run means everything, never nothing: a run that silently checked
  # nothing is indistinguishable at the call site from one that passed, and a
  # bare run is what CLAUDE.md tells a reader to type.
  scopes=(firmware app)
fi

for scope in "${scopes[@]}"; do
  case "$scope" in
    firmware|app) ;;
    *) echo "unknown scope: $scope (expected 'firmware' or 'app')" >&2; exit 2 ;;
  esac
done

has() { [[ " ${scopes[*]} " == *" $1 "* ]]; }

failures=()
report=""

run() {
  local label="$1"; shift
  local out
  if ! out=$("$@" 2>&1); then
    failures+=("$label")
    # Tail, not the whole thing: the report is read back by an agent with a
    # context window, and the last 40 lines are where the error is.
    report+=$'\n'"--- $label ---"$'\n'"$(tail -n 40 <<<"$out")"$'\n'
    return 1
  fi
}

# Nothing under src/algorithm/ may include Arduino.h, name a GPIO pin or a MIDI
# transport: only the hardware port nodes do that. It is a grep rather than a
# test because the thing it forbids compiles perfectly well.
algorithms_name_no_hardware() {
  ! grep -rnE 'Arduino\.h|GPIO_PIN_|mmMIDI_|hardware\.h|hal/teensy|IGpio|IMidiOut' src/algorithm/
}

# --- firmware ----------------------------------------------------------------

if has firmware; then
  if [[ ! -x .venv/bin/pio ]]; then
    # The cause, not the cascade: without this line the reader gets `pio: not
    # found` and starts installing PlatformIO by hand in a container that was
    # already installing it.
    echo "Checks failed: no .venv/bin/pio — run 'bash scripts/await_ready.sh' first."
    exit 1
  fi

  run "algorithms must not name hardware" algorithms_name_no_hardware
  run "pio test -e native" .venv/bin/pio test -e native
fi

# --- app ---------------------------------------------------------------------

if has app; then
  missing=()
  for tool in clang++ wasm-ld node; do
    command -v "$tool" >/dev/null || missing+=("$tool")
  done
  if [[ ${#missing[@]} -gt 0 ]]; then
    echo "Checks failed: missing ${missing[*]} — the module needs clang, lld and node (apt install clang lld nodejs)."
    exit 1
  fi

  # The module first, and nothing after it if it failed: every check below
  # loads emulator/dist/mmmc.wasm, so a broken build reports itself once here
  # instead of four times as a stale or absent module.
  if run "emulator/build.sh" emulator/build.sh; then
    run "module smoke test" node emulator/test/smoke.mjs
    # app/src/protocol.js is generated from the firmware headers. This is what
    # catches it having been hand-edited, or the headers having moved under it.
    run "protocol matches the firmware" node app/tools/generate-protocol.mjs --check
    run "app protocol tests" node app/test/protocol.test.mjs
    run "app tests" node app/test/app.test.mjs
  fi
fi

# --- report ------------------------------------------------------------------

if [[ ${#failures[@]} -gt 0 ]]; then
  echo "Checks failed: ${failures[*]}"
  echo "$report"
  exit 1
fi
