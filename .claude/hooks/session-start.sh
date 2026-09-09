#!/usr/bin/env bash
#
# Provision the PlatformIO toolchain for Claude Code on the web, so a session
# can build firmware and run tests without setting anything up first.
#
# Local sessions are left alone: a developer's machine already has whatever
# PlatformIO install they prefer.

set -euo pipefail

if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
    exit 0
fi

cd "${CLAUDE_PROJECT_DIR:-$(dirname "${BASH_SOURCE[0]}")/../..}"

# --warm also pulls the Teensy platform and toolchain (~600 MB). It is slow
# once and then cached with the container, which beats paying for it inside
# the first build of every session.
scripts/bootstrap.sh --warm

if [ -n "${CLAUDE_ENV_FILE:-}" ]; then
    echo "export PATH=\"${PWD}/.venv/bin:\${PATH}\"" >> "$CLAUDE_ENV_FILE"
fi
