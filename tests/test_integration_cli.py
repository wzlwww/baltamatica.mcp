from __future__ import annotations

import asyncio
import os
from pathlib import Path

import pytest

from baltamatica_mcp.backend_cli import CliEngine


def baltamatica_cli_path() -> Path | None:
    configured = os.environ.get("BALTAMATICA_CLI")
    if not configured:
        return None
    path = Path(configured).expanduser()
    if path.exists() and path.is_file():
        return path
    return None


pytestmark = pytest.mark.integration


def run(coro):
    return asyncio.run(coro)


@pytest.fixture()
def cli_path() -> Path:
    path = baltamatica_cli_path()
    if path is None:
        pytest.skip("Set BALTAMATICA_CLI to run real Baltamatica CLI integration tests.")
    return path


def test_real_cli_executes_code(cli_path: Path, tmp_path: Path) -> None:
    engine = CliEngine(executable=str(cli_path), timeout=30, state_file=tmp_path / "state.mat")

    result = run(engine.execute_code("disp(1 + 1)"))

    assert result.success is True
    assert result.error is None
    assert result.output.strip() == "2"


def test_real_cli_preserves_workspace_state(cli_path: Path, tmp_path: Path) -> None:
    engine = CliEngine(executable=str(cli_path), timeout=30, state_file=tmp_path / "state.mat")

    execute_result = run(engine.execute_code("A=[1 2;3 4]; b=42"))
    list_result = run(engine.list_variables())
    variable_result = run(engine.get_variable("A"))
    clear_result = run(engine.clear_workspace())
    cleared_list_result = run(engine.list_variables())

    assert execute_result.success is True
    assert list_result.success is True
    assert {variable.name for variable in list_result.variables} == {"A", "b"}
    assert variable_result.success is True
    assert "1   2" in variable_result.output
    assert "3   4" in variable_result.output
    assert clear_result.success is True
    assert cleared_list_result.success is True
    assert cleared_list_result.variables == []
