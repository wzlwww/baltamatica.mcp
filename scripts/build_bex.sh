#!/usr/bin/env bash
# Compile the BEX bridge for the current platform using Baltamatica's bundled
# `bex` compiler. The produced artifact (mcp_bridge.bexmaci64 / .bexa64 / .bexw64)
# is written into the bex/ directory.
#
# Usage:
#   scripts/build_bex.sh
#   BALTAMATICA_CLI=/path/to/baltamatica scripts/build_bex.sh
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
bex_dir="$repo_root/bex"

find_bex() {
    # 1) next to $BALTAMATICA_CLI
    if [[ -n "${BALTAMATICA_CLI:-}" ]]; then
        local candidate
        candidate="$(dirname "$BALTAMATICA_CLI")/bex"
        [[ -x "$candidate" ]] && { echo "$candidate"; return; }
    fi
    # 2) on PATH
    if command -v bex >/dev/null 2>&1; then
        command -v bex
        return
    fi
    # 3) common macOS install location
    local mac="/Applications/Baltamatica.app/Contents/MacOS/bex"
    [[ -x "$mac" ]] && { echo "$mac"; return; }
    return 1
}

bex_bin="$(find_bex)" || {
    echo "error: could not find the 'bex' compiler." >&2
    echo "Set BALTAMATICA_CLI to your baltamatica executable, or put bex on PATH." >&2
    exit 1
}

echo "Using bex compiler: $bex_bin"
cd "$bex_dir"
"$bex_bin" mcp_bridge.c

echo "Produced:"
ls -1 "$bex_dir"/mcp_bridge.bex* 2>/dev/null || {
    echo "error: no mcp_bridge.bex* artifact was produced." >&2
    exit 1
}
