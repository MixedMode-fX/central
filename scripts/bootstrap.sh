#!/usr/bin/env bash
#
# Provision the PlatformIO toolchain into .venv/ at the repository root.
#
# Safe to re-run: an existing venv is reused and packages are only upgraded.
# Pass --warm to also download the Teensy platform and the native test
# platform up front, which is what makes the first build fast in a fresh
# container.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

if [ ! -x .venv/bin/python ]; then
    python3 -m venv .venv
fi

.venv/bin/python -m pip install --quiet --upgrade pip
.venv/bin/python -m pip install --quiet --upgrade platformio

if [ "${1:-}" = "--warm" ]; then
    # Roughly 600 MB of toolchain. Not fatal if the network is unavailable:
    # the first build will fetch it instead.
    .venv/bin/pio pkg install -e teensy41 -e native >/dev/null 2>&1 ||
        echo "bootstrap: could not pre-fetch platforms; the first build will fetch them" >&2
fi

.venv/bin/pio --version
