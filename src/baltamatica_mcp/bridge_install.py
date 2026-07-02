"""Download the prebuilt BEX bridge binary for the current platform.

Exposed as `baltamatica-mcp install-bridge`, this fetches the right
`mcp_bridge.bex*` asset from the GitHub Release so BEX users don't have to pick
and download it by hand, and prints the two lines to paste into Baltamatica.
"""

from __future__ import annotations

import argparse
import platform
import stat
import urllib.request
from pathlib import Path

# platform.system() -> Release asset name
BRIDGE_ASSETS = {
    "Darwin": "mcp_bridge.bexmaci64",
    "Linux": "mcp_bridge.bexa64",
    "Windows": "mcp_bridge.bexw64",
}
RELEASES_URL = "https://github.com/wzlwww/baltamatica.mcp/releases"
DEFAULT_DIR = Path.home() / ".baltamatica-mcp"


def bridge_asset_name(system: str | None = None) -> str:
    system = system or platform.system()
    if system not in BRIDGE_ASSETS:
        raise SystemExit(
            f"No prebuilt BEX bridge for platform {system!r}. "
            "Compile it from source with scripts/build_bex.sh."
        )
    return BRIDGE_ASSETS[system]


def bridge_asset_url(asset: str, tag: str | None = None) -> str:
    where = f"download/{tag}" if tag else "latest/download"
    return f"{RELEASES_URL}/{where}/{asset}"


def download_bridge(dest_dir: Path, tag: str | None = None) -> Path:
    asset = bridge_asset_name()
    url = bridge_asset_url(asset, tag)
    dest_dir.mkdir(parents=True, exist_ok=True)
    dest = dest_dir / asset
    try:
        urllib.request.urlretrieve(url, dest)
    except OSError as exc:
        raise SystemExit(f"Failed to download {url}: {exc}") from exc
    dest.chmod(dest.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return dest


def install_bridge_cli(argv: list[str]) -> None:
    parser = argparse.ArgumentParser(
        prog="baltamatica-mcp install-bridge",
        description="Download the prebuilt BEX bridge binary for this platform.",
    )
    parser.add_argument("--dir", default=str(DEFAULT_DIR), help="Install directory (default: ~/.baltamatica-mcp).")
    parser.add_argument("--tag", default=None, help="Release tag to download from (default: latest).")
    args = parser.parse_args(argv)

    dest_dir = Path(args.dir).expanduser()
    asset = bridge_asset_name()
    print(f"Downloading {asset} -> {dest_dir} ...")
    dest = download_bridge(dest_dir, args.tag)
    print(f"Installed: {dest}\n")
    print("In the Baltamatica GUI command line, load and start the bridge:")
    print(f"  addpath('{dest_dir}'); savepath")
    print("  mcp_bridge('background')\n")
    print("Then point the MCP server at it:")
    print("  baltamatica-mcp --backend bex")
