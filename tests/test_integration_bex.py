"""Optional BEX integration tests.

Marked ``integration`` so they are skipped by the default `pytest -m "not
integration"` run. They exercise the real toolchain and a live bridge:

- the source compiles with Baltamatica's ``bex`` compiler (needs BALTAMATICA_CLI);
- a bridge already listening on 127.0.0.1:31415 answers a full round-trip.
"""

from __future__ import annotations

import asyncio
import os
import shutil
import socket
import subprocess
from pathlib import Path

import pytest

pytestmark = pytest.mark.integration

REPO = Path(__file__).resolve().parents[1]
BEX_DIR = REPO / "bex"


def _bex_compiler() -> Path | None:
    balta = os.environ.get("BALTAMATICA_CLI")
    if not balta:
        return None
    candidate = Path(balta).parent / "bex"
    return candidate if candidate.exists() else None


def _port_open(host: str, port: int, timeout: float = 0.5) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(timeout)
        try:
            sock.connect((host, port))
            return True
        except OSError:
            return False


def test_bex_source_compiles_with_bex_compiler(tmp_path) -> None:
    bex = _bex_compiler()
    if bex is None:
        pytest.skip("bex compiler not found (set BALTAMATICA_CLI to the baltamatica executable)")

    # Compile a copy so the repository artifact is not clobbered.
    for name in ("mcp_bridge.c", "mcp_protocol.h"):
        shutil.copy(BEX_DIR / name, tmp_path / name)

    result = subprocess.run(
        [str(bex), "mcp_bridge.c"],
        cwd=tmp_path,
        capture_output=True,
        text=True,
        timeout=180,
    )

    assert result.returncode == 0, f"bex compile failed:\n{result.stdout}\n{result.stderr}"
    produced = list(tmp_path.glob("mcp_bridge.bex*"))
    assert produced, f"no compiled BEX artifact produced; output:\n{result.stdout}"


def test_bex_bridge_roundtrip_if_running() -> None:
    if not _port_open("127.0.0.1", 31415):
        pytest.skip("no BEX bridge listening on 127.0.0.1:31415")

    from baltamatica_mcp.backend_bex import BexEngine

    async def exercise() -> None:
        engine = BexEngine(port=31415, timeout=10)
        try:
            await engine.clear_workspace()

            executed = await engine.execute_code("it_answer = 6 * 7;")
            assert executed.success

            got = await engine.get_variable("it_answer")
            assert got.success
            assert got.value["data"] == [42.0]

            was_set = await engine.set_variable("it_matrix", [[1, 2], [3, 4]])
            assert was_set.success

            got_matrix = await engine.get_variable("it_matrix")
            assert got_matrix.value["data"] == [[1.0, 2.0], [3.0, 4.0]]

            listed = await engine.list_variables()
            names = {v.name for v in listed.variables}
            assert {"it_answer", "it_matrix"} <= names
        finally:
            await engine.close()

    asyncio.run(exercise())
