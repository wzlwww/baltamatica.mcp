#!/usr/bin/env bash
# Compile the BEX bridge for the current platform. Produces the artifact
# (mcp_bridge.bexmaci64 / .bexa64 / .bexw64) in the bex/ directory.
#
# Two strategies are tried:
#   1) the standalone `bex` compiler (fast; works on macOS);
#   2) the in-interpreter bex() function via `baltamatica -nodesktop -s ...`
#      (needed on some Linux builds where the standalone `bex` binary crashes
#      at startup, e.g. Ubuntu 24.04).
#
# Usage:
#   scripts/build_bex.sh
#   BALTAMATICA_CLI=/opt/Baltamatica/bin/baltamatica scripts/build_bex.sh
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
bex_dir="$repo_root/bex"

produced() { ls "$bex_dir"/mcp_bridge.bex* >/dev/null 2>&1; }

# --- strategy 1: standalone bex compiler ---
find_bex() {
    if [[ -n "${BALTAMATICA_CLI:-}" && -x "$(dirname "$BALTAMATICA_CLI")/bex" ]]; then
        echo "$(dirname "$BALTAMATICA_CLI")/bex"; return
    fi
    command -v bex 2>/dev/null && return
    for c in /Applications/Baltamatica.app/Contents/MacOS/bex /opt/Baltamatica/bin/bex; do
        [[ -x "$c" ]] && { echo "$c"; return; }
    done
    return 1
}

# --- strategy 2: interpreter bex() function ---
find_baltamatica() {
    if [[ -n "${BALTAMATICA_CLI:-}" && -x "$BALTAMATICA_CLI" ]]; then echo "$BALTAMATICA_CLI"; return; fi
    for c in /opt/Baltamatica/bin/baltamatica /Applications/Baltamatica.app/Contents/MacOS/baltamatica; do
        [[ -x "$c" ]] && { echo "$c"; return; }
    done
    command -v baltamatica 2>/dev/null && return
    return 1
}

rm -f "$bex_dir"/mcp_bridge.bex*
cd "$bex_dir"

if bex_bin="$(find_bex)"; then
    echo "[1/2] trying standalone compiler: $bex_bin"
    if "$bex_bin" mcp_bridge.c >/dev/null 2>&1 && produced; then
        echo "ok (standalone)"; ls -1 "$bex_dir"/mcp_bridge.bex*; exit 0
    fi
    echo "  standalone bex failed; falling back to the interpreter"
fi

balta="$(find_baltamatica)" || {
    echo "error: could not find bex or baltamatica; set BALTAMATICA_CLI." >&2; exit 1; }

echo "[2/2] compiling via interpreter: $balta"
# Baltamatica on Linux needs its bundled libs on LD_LIBRARY_PATH.
balta_dir="$(cd "$(dirname "$balta")/.." && pwd)"
export LD_LIBRARY_PATH="$balta_dir/lib:${LD_LIBRARY_PATH:-}"
QT_QPA_PLATFORM=offscreen "$balta" -nodesktop -s "cd('$bex_dir'); bex('mcp_bridge.c')" || true

produced || { echo "error: no mcp_bridge.bex* artifact was produced." >&2; exit 1; }
echo "ok (interpreter)"; ls -1 "$bex_dir"/mcp_bridge.bex*
