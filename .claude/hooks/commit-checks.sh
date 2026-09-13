#!/usr/bin/env bash
# PreToolUse/Bash gate: refuse `git commit` until the fast checks pass.
#
# Reads the hook payload on stdin and only acts when the command really is a
# commit. The checks themselves live in scripts/checks.sh, shared with CI, so
# the two cannot drift.
#
# Why a hook rather than a line in CLAUDE.md: an instruction is followed most
# of the time, and "most of the time" is how a red branch reaches a pull
# request you are reading on a train. A denied tool call is followed every
# time, and hands back the failure report as the reason — which is the same
# text the model needs in order to fix it.
#
# Escape hatch: CLAUDE_SKIP_COMMIT_CHECKS=1.
set -uo pipefail

payload=$(cat)
command=$(printf '%s' "$payload" | jq -r '.tool_input.command // ""')

# Matches `git commit`, `git -C dir commit`, and a commit chained after
# cd/&&/; — which is how it is nearly always written.
if ! grep -Eq '(^|[;&|(]|&&)[[:space:]]*git([[:space:]]+-[^[:space:]]+([[:space:]]+[^[:space:]-][^[:space:]]*)?)*[[:space:]]+commit([[:space:]]|$)' <<<"$command"; then
  exit 0
fi

grep -Eq -- '--dry-run' <<<"$command" && exit 0
[[ "${CLAUDE_SKIP_COMMIT_CHECKS:-}" == "1" ]] && exit 0

repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || exit 0
cd "$repo_root" || exit 0

# Staged plus unstaged, so `git commit -a` is covered too.
changed=$(git diff --cached --name-only --diff-filter=ACMR; git diff --name-only --diff-filter=ACMR)

# Which half of the repository this commit touches — a routing decision, not a
# skip. src/ is in both lists deliberately and that is the whole point of the
# routing: the firmware core is also what the WebAssembly module is compiled
# from and what app/src/protocol.js is generated against, so a change to a
# header under src/protocol/ breaks the app without touching a line of
# JavaScript.
scopes=()
grep -Eq '^(src/|test/|lib/|include/|platformio\.ini$|scripts/(version|project_warnings)\.py$)' <<<"$changed" && scopes+=(firmware)
grep -Eq '^(src/|app/|emulator/)' <<<"$changed" && scopes+=(app)

# A change to the gate itself is checked by the gate itself.
if grep -Eq '^scripts/(checks|start_app)\.sh$' <<<"$changed"; then
  scopes=(firmware app)
fi

# Nothing this gate can check — a README, a workflow, a hook — passes straight
# through rather than paying half a minute for a documentation commit.
[[ ${#scopes[@]} -eq 0 ]] && exit 0

report=$(scripts/checks.sh "${scopes[@]}") && exit 0

jq -n --arg reason "Commit blocked: checks did not pass. Fix these, then commit again.

$report" '{
  hookSpecificOutput: {
    hookEventName: "PreToolUse",
    permissionDecision: "deny",
    permissionDecisionReason: $reason
  }
}'
