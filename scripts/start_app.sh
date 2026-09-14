#!/usr/bin/env bash
# One entry point to a running app, on a laptop or a fresh Claude container.
#
#   bash scripts/start_app.sh                 prepare, then serve the built page
#   bash scripts/start_app.sh --dev           prepare, then the Vite dev server
#   bash scripts/start_app.sh --prepare-only  build the toolchain and the module, then stop
#
# The built page is emulator/dist/index.html - one file with the module
# inlined, the same page CI uploads and Pages publishes - served by a static
# server. It has to be *served*: Web MIDI needs a secure context, and a
# screenshot needs a URL. --dev serves the source tree instead, with hot
# reload, from Vite's own server on its own port.
#
# Idempotent: a server already answering is reused, not restarted. Runtime
# state lives in .dev/ (gitignored): logs, pids, the prepared marker, the
# base URL. Stop everything with scripts/stop_app.sh.
set -uo pipefail

repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || repo_root=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo_root"

run_dir="$repo_root/.dev"
mkdir -p "$run_dir"

PREPARE_ONLY=0
DEV=0
for arg in "$@"; do
  case "$arg" in
    --prepare-only) PREPARE_ONLY=1 ;;
    --dev) DEV=1 ;;
    *) echo "unknown flag: $arg" >&2; exit 2 ;;
  esac
done

PORT="${APP_PORT:-8080}"
DEV_PORT="${APP_DEV_PORT:-5173}"

log() { echo "[start_app] $*"; }

# --- prepare ----------------------------------------------------------------

# Everything a checkout needs before any check can run. The two halves of this
# repository are independent — PlatformIO is pip and a platform download, the
# module and the page are clang and npm — so they run at once and the summary
# says which one the wait was actually for.
prepare() {
  local t0=$SECONDS

  local pio_t=0 module_t=0
  local pio_pid= module_pid=

  if [[ ! -x .venv/bin/pio ]]; then
    log "installing PlatformIO (.venv/) and the toolchains"
    # `exit $status` rather than letting the timing write be the last command:
    # a subshell's status is its last command's, so a failed bootstrap would
    # wait 0, pass the check below, touch the prepared marker and leave the
    # container claiming to be ready with no pio in it.
    ( t=$SECONDS
      scripts/bootstrap.sh --warm >>"$run_dir/setup.log" 2>&1
      status=$?
      echo $((SECONDS - t)) >"$run_dir/.pio_time"
      exit $status ) &
    pio_pid=$!
  fi

  # The module is what the page loads and what every app check drives, so a
  # prepared checkout has one - and the page built beside it, which is where
  # the app's dependencies get installed.
  if command -v clang++ >/dev/null && command -v wasm-ld >/dev/null; then
    log "building the WebAssembly module and the page"
    ( t=$SECONDS
      emulator/build.sh >>"$run_dir/setup.log" 2>&1
      status=$?
      echo $((SECONDS - t)) >"$run_dir/.module_time"
      exit $status ) &
    module_pid=$!
  else
    log "no clang/lld: skipping the module (the app checks will say so)"
  fi

  local failed=()
  [[ -n "$pio_pid" ]] && { wait "$pio_pid" || failed+=("PlatformIO"); pio_t=$(cat "$run_dir/.pio_time" 2>/dev/null || echo 0); }
  [[ -n "$module_pid" ]] && { wait "$module_pid" || failed+=("module"); module_t=$(cat "$run_dir/.module_time" 2>/dev/null || echo 0); }
  rm -f "$run_dir/.pio_time" "$run_dir/.module_time"

  if [[ ${#failed[@]} -gt 0 ]]; then
    log "prepare failed: ${failed[*]}"
    tail -30 "$run_dir/setup.log"
    exit 1
  fi

  # Slowest first, so "startup is slow" is answerable from the log it already
  # wrote rather than from a second run with a stopwatch.
  { [[ $pio_t -ge $module_t ]] && printf '  %ss  PlatformIO\n  %ss  module\n' "$pio_t" "$module_t" \
    || printf '  %ss  module\n  %ss  PlatformIO\n' "$module_t" "$pio_t"; } >&2

  touch "$run_dir/prepared"
  log "prepared in $((SECONDS - t0))s"
}

prepare

if [[ $PREPARE_ONLY -eq 1 ]]; then
  exit 0
fi

# --- start ------------------------------------------------------------------

spawn() {
  local name="$1"; shift
  local pidfile="$run_dir/$name.pid"
  if [[ -f "$pidfile" ]] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
    log "$name already running"
    return 0
  fi
  # setsid detaches the server from this shell's process group, so it survives
  # the command that started it returning.
  local detach=()
  command -v setsid >/dev/null && detach=(setsid)
  "${detach[@]}" nohup "$@" >"$run_dir/$name.log" 2>&1 </dev/null &
  echo $! >"$pidfile"
  log "$name started (log: .dev/$name.log)"
}

if [[ $DEV -eq 1 ]]; then
  # The source tree, with hot reload. The module is reached at
  # emulator/dist/mmmc.wasm through the alias in app/vite.config.js.
  spawn dev npm --prefix app run --silent dev -- --port "$DEV_PORT" --strictPort --host 127.0.0.1
  base="http://127.0.0.1:$DEV_PORT"
else
  # The one-file page is its own root, already built by prepare: index.html
  # plus mmmc.wasm beside it. A page that was already being served is served
  # again as it is now - the build wrote over the file the server reads.
  [[ -f "$run_dir/prepared" ]] && emulator/build.sh >>"$run_dir/setup.log" 2>&1
  spawn app python3 -m http.server "$PORT" --bind 127.0.0.1 --directory emulator/dist
  base="http://127.0.0.1:$PORT"
fi

# --noproxy: a container's HTTPS_PROXY must not be consulted for localhost.
for _ in $(seq 1 30); do
  curl -fsS --noproxy '*' --max-time 2 "$base/" >/dev/null 2>&1 && break
  sleep 1
done

echo "$base" >"$run_dir/base_url"
log "app: $base"
echo "$base"
