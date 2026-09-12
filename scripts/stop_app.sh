#!/usr/bin/env bash
# Stop whatever scripts/start_app.sh started. Leaves .dev/prepared alone —
# stopping the server does not un-install the dependencies.
set -uo pipefail

repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || repo_root=$(cd "$(dirname "$0")/.." && pwd)
run_dir="$repo_root/.dev"

for pidfile in "$run_dir"/*.pid; do
  [[ -e "$pidfile" ]] || continue
  name=$(basename "$pidfile" .pid)
  pid=$(cat "$pidfile")
  if kill -0 "$pid" 2>/dev/null; then
    # Negative pid: kill the process group, because setsid made one and the
    # npx wrapper is not the process actually holding the port.
    kill -- "-$pid" 2>/dev/null || kill "$pid" 2>/dev/null
    echo "[stop_app] stopped $name ($pid)"
  fi
  rm -f "$pidfile"
done

rm -f "$run_dir/base_url"
