#!/usr/bin/env bash
#
# Build the firmware core to WebAssembly and wrap it in a single HTML page.
#
# Compiles every source the native test environment compiles (src/ minus
# main.cpp and hal/teensy/) with the project's warning flags, plus the web
# HAL and the C bridge in emulator/src/, and links them with wasm-ld into a
# freestanding module. No Emscripten, no libc: the firmware core uses
# neither, so a stock clang with a wasm32 target is enough.
#
# Output:
#   emulator/dist/mmmc.wasm     the module
#   emulator/dist/index.html    the page with the module embedded, so it
#                               opens from a file:// URL or any static host
#
# Requirements: clang (>= 15) and lld. On Debian/Ubuntu: apt install clang lld.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EMU="$ROOT/emulator"
DIST="$EMU/dist"
OBJ="$DIST/obj"

CXX="${CXX:-clang++}"
WASM_LD="${WASM_LD:-}"

VERSION="$(cat "$ROOT/VERSION" 2>/dev/null || echo 0.0.0)"
GIT_REV="$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo unknown)"

CXXFLAGS=(
    --target=wasm32
    -std=gnu++17
    -O2
    -ffreestanding -nostdlib -nostdinc++
    -fno-exceptions -fno-rtti
    -fvisibility=hidden
    -I"$ROOT/src" -I"$EMU/src" -isystem "$EMU/shim"
    -DMMMC_VERSION="\"$VERSION\"" -DMMMC_GIT_REV="\"$GIT_REV\"" -DMMMC_BUILD="\"$VERSION+$GIT_REV\""
)
# The same flags the firmware is held to (platformio.ini, project_warnings.py).
WARNFLAGS=(-Wall -Wextra -Wshadow -Weffc++ -Werror)

LDFLAGS=(
    -Wl,--no-entry
    -Wl,--export-dynamic
    -Wl,--export=__wasm_call_ctors
    -Wl,--initial-memory=1048576
    -Wl,-z,stack-size=65536
)

rm -rf "$OBJ"
mkdir -p "$OBJ"

# Same selection as build_src_filter in [env:native].
mapfile -t SOURCES < <(find "$ROOT/src" -name '*.cpp' ! -path '*/hal/teensy/*' ! -name main.cpp | sort)
SOURCES+=("$EMU/src/emu_api.cpp")

OBJECTS=()
for src in "${SOURCES[@]}"; do
    obj="$OBJ/$(echo "${src#$ROOT/}" | tr '/' '_').o"
    "$CXX" "${CXXFLAGS[@]}" "${WARNFLAGS[@]}" -c "$src" -o "$obj"
    OBJECTS+=("$obj")
done
# The runtime shims define memcpy & co.; -Weffc++ has nothing to say there
# but the file is held to the rest.
"$CXX" "${CXXFLAGS[@]}" -Wall -Wextra -Werror -c "$EMU/src/runtime.cpp" -o "$OBJ/runtime.o"
OBJECTS+=("$OBJ/runtime.o")

if [ -n "$WASM_LD" ]; then LDFLAGS+=(-fuse-ld="$WASM_LD"); fi
"$CXX" "${CXXFLAGS[@]}" "${LDFLAGS[@]}" "${OBJECTS[@]}" -o "$DIST/mmmc.wasm"

# Any import other than the page's MIDI callback means a libc symbol crept
# into the core; fail here rather than at page load.
if command -v llvm-nm >/dev/null 2>&1; then
    undefined="$(llvm-nm -u "$DIST/mmmc.wasm" | grep -v mmmc_midi_send || true)"
    if [ -n "$undefined" ]; then
        echo "emulator: unexpected imports:" >&2
        echo "$undefined" >&2
        exit 1
    fi
fi

# Single-file page: the module goes in as base64 where index.html expects it.
python3 - "$EMU/index.html" "$DIST/mmmc.wasm" "$DIST/index.html" <<'PY'
import base64, sys
page, wasm, out = sys.argv[1:4]
html = open(page, encoding="utf-8").read()
b64 = base64.b64encode(open(wasm, "rb").read()).decode("ascii")
marker = "/*MMMC_WASM_BASE64*/"
assert marker in html, "index.html lost its embed marker"
open(out, "w", encoding="utf-8").write(html.replace(marker, b64, 1))
PY

rm -rf "$OBJ"
echo "emulator: $VERSION+$GIT_REV -> $DIST/mmmc.wasm ($(wc -c < "$DIST/mmmc.wasm") bytes), $DIST/index.html"
