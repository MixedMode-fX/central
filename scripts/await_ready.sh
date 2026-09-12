#!/usr/bin/env bash
# Block until the dev environment is prepared; return immediately when it is.
#
# Contract:
#   prepared            -> returns in milliseconds
#   preparation running -> joins it (flock) rather than racing it
#   never prepared      -> runs the preparation inline
#   --refresh           -> force a re-run
#
# Run this before the first check on a fresh container. It is the answer to
# "pio: not found" and to a page that loads with no module: the container is
# cloned without .venv/ and without emulator/dist, and the SessionStart hook
# only *started* filling them in.
set -uo pipefail

repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || repo_root=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo_root"

run_dir="$repo_root/.dev"
mkdir -p "$run_dir"
marker="$run_dir/prepared"

[[ "${1:-}" == "--refresh" ]] && rm -f "$marker"

[[ -f "$marker" ]] && exit 0

exec 9>"$run_dir/prepare.lock"
flock 9
# Someone else may have finished while we were waiting on the lock.
[[ -f "$marker" ]] && exit 0

# 9>&- closes the lock fd for children, so nothing long-lived inherits it and
# holds the lock for the life of the container.
bash scripts/start_app.sh --prepare-only 9>&-
