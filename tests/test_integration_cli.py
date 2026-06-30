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


def test_real_cli_reports_file_artifact(cli_path: Path, tmp_path: Path) -> None:
    output_path = tmp_path / "artifact.txt"
    state_path = tmp_path / "state.mat"
    code = (
        f"fid=fopen('{output_path.as_posix()}','w'); "
        "fprintf(fid,'artifact from baltamatica'); "
        "fclose(fid); "
        f"fprintf('BALTAMATICA_ARTIFACT=text/plain:{output_path.as_posix()}\\n')"
    )
    engine = CliEngine(executable=str(cli_path), timeout=30, state_file=state_path)

    result = run(engine.execute_code(code))

    assert result.success is True
    assert output_path.read_text(encoding="utf-8") == "artifact from baltamatica"
    assert result.artifacts is not None
    assert len(result.artifacts) == 1
    assert result.artifacts[0].path == str(output_path.resolve())
    assert result.artifacts[0].type == "text/plain"
    assert result.artifacts[0].exists is True
    assert result.artifacts[0].size > 0


def test_real_cli_runs_artifact_export_demo(cli_path: Path, tmp_path: Path) -> None:
    artifact_path = Path("/tmp/baltamatica_mcp_wave.csv")
    if artifact_path.exists():
        artifact_path.unlink()
    script_path = Path("examples/artifact_export_demo.m").resolve()
    engine = CliEngine(executable=str(cli_path), timeout=30, state_file=tmp_path / "state.mat")

    result = run(engine.run_script(str(script_path)))

    assert result.success is True
    assert result.artifacts is not None
    assert len(result.artifacts) == 1
    assert result.artifacts[0].type == "text/csv"
    assert result.artifacts[0].exists is True
    assert result.artifacts[0].size > 0
    assert artifact_path.read_text(encoding="utf-8").startswith("t,sin_t,cos_t")
