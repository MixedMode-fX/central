#!/usr/bin/env bash
# One entry point to a running app, on a laptop or a fresh Claude container.
#
#   bash scripts/start_app.sh                 prepare + serve app/ as it is
#   bash scripts/start_app.sh --prepare-only  build the toolchain and the module, then stop
#   bash scripts/start_app.sh --build         serve the single-file build instead
#
# The app has no build step — it is ES modules, served as they are — so the
# dev server is a static server over the repository root and the page is at
# /app/. It has to be *served*: a browser will not load a module from file://,
# and Web MIDI needs a secure context. --build serves emulator/dist instead,
# which is the one-file page CI uploads and Pages publishes.
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
BUILD=0
for arg in "$@"; do
  case "$arg" in
    --prepare-only) PREPARE_ONLY=1 ;;
    --build) BUILD=1 ;;
    *) echo "unknown flag: $arg" >&2; exit 2 ;;
  esac
done

PORT="${APP_PORT:-8080}"

log() { echo "[start_app] $*"; }

# --- prepare ----------------------------------------------------------------

# Everything a checkout needs before any check can run. The two halves of this
# repository are independent — PlatformIO is pip and a platform download, the
# module is clang — so they run at once and the summary says which one the
# wait was actually for.
prepare() {
  local t0=$SECONDS

  local pio_t=0 module_t=0
  local pio_pid= module_pid=

  if [[ ! -x .venv/bin/pio ]]; then
    log "installing PlatformIO (.venv/) and the toolchains"
    ( t=$SECONDS
      scripts/bootstrap.sh --warm >>"$run_dir/setup.log" 2>&1
      echo $((SECONDS - t)) >"$run_dir/.pio_time" ) &
    pio_pid=$!
  fi

  # The module is what the page loads and what every app check drives, so a
  # prepared checkout has one. It is seconds; it is not worth a marker.
  if command -v clang++ >/dev/null && command -v wasm-ld >/dev/null; then
    log "building the WebAssembly module"
    ( t=$SECONDS
      emulator/build.sh >>"$run_dir/setup.log" 2>&1
      echo $((SECONDS - t)) >"$run_dir/.module_time" ) &
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

if [[ $BUILD -eq 1 ]]; then
  log "emulator/build.sh"
  emulator/build.sh >>"$run_dir/setup.log" 2>&1 || { tail -30 "$run_dir/setup.log"; exit 1; }
  # The one-file page is its own root: index.html plus mmmc.wasm beside it.
  spawn app python3 -m http.server "$PORT" --bind 127.0.0.1 --directory emulator/dist
  path=""
else
  # From the repository root, because app/src/module.js reaches the module at
  # ../../emulator/dist/mmmc.wasm — serving app/ alone leaves the page with no
  # module and no useful error.
  spawn app python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$repo_root"
  path="/app"
fi

base="http://127.0.0.1:$PORT$path"

# --noproxy: a container's HTTPS_PROXY must not be consulted for localhost.
for _ in $(seq 1 30); do
  curl -fsS --noproxy '*' --max-time 2 "$base/" >/dev/null 2>&1 && break
  sleep 1
done

echo "$base" >"$run_dir/base_url"
log "app: $base"
echo "$base"
