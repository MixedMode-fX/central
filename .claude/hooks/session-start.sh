#!/usr/bin/env bash
# SessionStart: get the checkout ready to be checked, without making the
# session wait for it.
#
# A container is cloned fresh for every web and mobile session, which leaves
# no .venv/ and no emulator/dist — and until those exist, `pio test` is
# "command not found" and the page loads with no module. An agent that meets
# those failures cold will debug them, and the transcript that comes back to
# your phone is twenty minutes of a solved problem.
#
# Preparation is a minute on a cold container and milliseconds on a warm one,
# so it runs detached here and `scripts/await_ready.sh` is what blocks on it.
# The hook itself must return in seconds: it is in front of the first prompt.
# A hook that runs the provisioning inline is killed by the hook timeout
# part-way through, which leaves a half-installed toolchain and no message
# saying so.
set -uo pipefail

repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || exit 0
cd "$repo_root" || exit 0

run_dir="$repo_root/.dev"
mkdir -p "$run_dir"

# A developer's machine already has whatever PlatformIO install they prefer;
# the detached provisioning is for containers.
if [[ "${CLAUDE_CODE_REMOTE:-}" == "true" && ! -f "$run_dir/prepared" ]]; then
  # setsid detaches it from the session's process group, so the hook can
  # return now and the work survives.
  detach=()
  command -v setsid >/dev/null && detach=(setsid)
  "${detach[@]}" nohup bash scripts/await_ready.sh >"$run_dir/prepare.log" 2>&1 </dev/null &
  disown $! 2>/dev/null || true

  context="Preparing the dev environment in the background: PlatformIO into .venv/ with the Teensy and native platforms, and the firmware compiled to WebAssembly (emulator/build.sh). Log: .dev/prepare.log"
else
  context="The dev environment is already prepared."
fi

# PlatformIO is installed project-locally, so the tools are on PATH for the
# rest of the session rather than reached by path in every command.
if [[ -n "${CLAUDE_ENV_FILE:-}" ]]; then
  echo "export PATH=\"${PWD}/.venv/bin:\${PATH}\"" >> "$CLAUDE_ENV_FILE"
fi

# The hook's real payload: a sentence in the model's context, at the top of
# the session, saying what to do about all this.
jq -n --arg context "$context" '{
  hookSpecificOutput: {
    hookEventName: "SessionStart",
    additionalContext: ($context + "\n\nBefore running any check or starting the app, run `bash scripts/await_ready.sh` — it returns immediately when preparation is done and blocks until it is when it is not. Do not diagnose `pio: not found`, a wasm module that will not load, or a page with no module in it, before it has returned.")
  }
}'
