from __future__ import annotations

from pathlib import Path

import pytest

from baltamatica_mcp.bridge_install import (
    bridge_asset_name,
    bridge_asset_url,
    download_bridge,
)


def test_bridge_asset_name_maps_platforms() -> None:
    assert bridge_asset_name("Darwin") == "mcp_bridge.bexmaci64"
    assert bridge_asset_name("Linux") == "mcp_bridge.bexa64"
    assert bridge_asset_name("Windows") == "mcp_bridge.bexw64"


def test_bridge_asset_name_rejects_unknown_platform() -> None:
    with pytest.raises(SystemExit):
        bridge_asset_name("Plan9")


def test_bridge_asset_url() -> None:
    assert bridge_asset_url("mcp_bridge.bexa64").endswith(
        "/releases/latest/download/mcp_bridge.bexa64"
    )
    assert "/releases/download/v0.2.0/" in bridge_asset_url("mcp_bridge.bexa64", "v0.2.0")


def test_download_bridge_writes_asset(tmp_path, monkeypatch) -> None:
    def fake_urlretrieve(url: str, dest: str) -> None:
        Path(dest).write_bytes(b"MZ\x00\x00")

    monkeypatch.setattr(
        "baltamatica_mcp.bridge_install.urllib.request.urlretrieve", fake_urlretrieve
    )
    monkeypatch.setattr("baltamatica_mcp.bridge_install.platform.system", lambda: "Linux")

    dest = download_bridge(tmp_path / "install")
    assert dest.name == "mcp_bridge.bexa64"
    assert dest.exists()
    assert dest.read_bytes().startswith(b"MZ")


def test_main_dispatches_install_bridge(monkeypatch) -> None:
    from baltamatica_mcp import server

    captured: dict[str, list[str]] = {}
    monkeypatch.setattr(
        "baltamatica_mcp.bridge_install.install_bridge_cli",
        lambda argv: captured.setdefault("argv", argv),
    )
    server.main(["install-bridge", "--dir", "/tmp/x"])
    assert captured["argv"] == ["--dir", "/tmp/x"]
