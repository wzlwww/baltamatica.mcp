from __future__ import annotations

import asyncio
import os
from pathlib import Path

import pytest

from baltamatica_mcp.backend_cli import CliEngine, parse_whos_output
from baltamatica_mcp.engine import EngineUnavailableError, create_engine


def write_executable(path: Path, body: str) -> Path:
    path.write_text(body, encoding="utf-8")
    path.chmod(0o755)
    return path


def run(coro):
    return asyncio.run(coro)


def test_create_engine_returns_cli_for_cli_and_auto() -> None:
    assert isinstance(create_engine("cli"), CliEngine)
    assert isinstance(create_engine("auto"), CliEngine)


def test_create_engine_keeps_bex_unimplemented() -> None:
    engine = create_engine("bex")

    assert engine.backend == "bex"


def test_execute_code_runs_baltamatica_cli(tmp_path: Path) -> None:
    executable = write_executable(
        tmp_path / "fake-baltamatica",
        "#!/bin/sh\n"
        "printf 'argv:%s\\n' \"$*\"\n",
    )
    state_file = tmp_path / "state.mat"
    engine = CliEngine(executable=str(executable), timeout=2, state_file=state_file)

    result = run(engine.execute_code("disp(1 + 1)"))

    assert result.success is True
    assert result.error is None
    assert "argv:-nodesktop -s" in result.output
    assert "disp(1 + 1)" in result.output
    assert f"save('{state_file.as_posix()}')" in result.output


def test_execute_code_returns_nonzero_exit_as_failed_result(tmp_path: Path) -> None:
    executable = write_executable(
        tmp_path / "fake-baltamatica",
        "#!/bin/sh\n"
        "echo 'bad code' >&2\n"
        "exit 7\n",
    )
    engine = CliEngine(executable=str(executable), timeout=2, state_file=tmp_path / "state.mat")

    result = run(engine.execute_code("bad"))

    assert result.success is False
    assert result.output == "bad code"
    assert result.error == "Baltamatica CLI exited with code 7: bad code"


def test_run_script_uses_run_command_with_escaped_path(tmp_path: Path) -> None:
    executable = write_executable(
        tmp_path / "fake-baltamatica",
        "#!/bin/sh\n"
        "printf '%s\\n' \"$3\"\n",
    )
    script_path = tmp_path / "name'with quote.m"
    script_path.write_text("disp(1)", encoding="utf-8")
    engine = CliEngine(executable=str(executable), timeout=2, state_file=tmp_path / "state.mat")

    result = run(engine.run_script(str(script_path)))

    assert result.success is True
    escaped_path = script_path.as_posix().replace("'", "''")
    assert f"run('{escaped_path}')" in result.output


def test_missing_configured_executable_raises_unavailable(tmp_path: Path) -> None:
    engine = CliEngine(executable=str(tmp_path / "missing"))

    with pytest.raises(EngineUnavailableError, match="does not exist"):
        run(engine.execute_code("disp(1)"))


def test_environment_executable_is_used(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    executable = write_executable(
        tmp_path / "fake-baltamatica",
        "#!/bin/sh\n"
        "echo env-executable\n",
    )
    monkeypatch.setenv("BALTAMATICA_CLI", str(executable))
    engine = CliEngine(timeout=2)

    result = run(engine.execute_code("disp(1)"))

    assert result.success is True
    assert result.output == "env-executable"


def test_timeout_kills_process(tmp_path: Path) -> None:
    executable = write_executable(
        tmp_path / "fake-baltamatica",
        "#!/bin/sh\n"
        "sleep 2\n",
    )
    engine = CliEngine(executable=str(executable), timeout=0.1)

    with pytest.raises(TimeoutError, match="timed out"):
        run(engine.execute_code("disp(1)"))


def test_path_lookup_uses_command_name(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    executable = write_executable(
        tmp_path / "baltamaticaC.sh",
        "#!/bin/sh\n"
        "echo path-lookup\n",
    )
    monkeypatch.setenv("PATH", f"{tmp_path}{os.pathsep}{os.environ.get('PATH', '')}")
    engine = CliEngine(executable=executable.name, timeout=2)

    result = run(engine.execute_code("disp(1)"))

    assert result.success is True
    assert result.output == "path-lookup"


def test_clear_workspace_removes_state_file(tmp_path: Path) -> None:
    executable = write_executable(
        tmp_path / "fake-baltamatica",
        "#!/bin/sh\n"
        "printf '%s\\n' \"$3\"\n",
    )
    state_file = tmp_path / "state.mat"
    state_file.write_text("state", encoding="utf-8")
    engine = CliEngine(executable=str(executable), timeout=2, state_file=state_file)

    result = run(engine.clear_workspace())

    assert result.success is True
    assert result.output == "clear"
    assert not state_file.exists()


def test_list_variables_parses_whos_output(tmp_path: Path) -> None:
    executable = write_executable(
        tmp_path / "fake-baltamatica",
        "#!/bin/sh\n"
        "cat <<'EOF'\n"
        "  Name  Size  Bytes  Class   Attributes\n"
        "\n"
        "  A     2x2      32  double\n"
        "  label 1x5      10  char    global\n"
        "EOF\n",
    )
    engine = CliEngine(executable=str(executable), timeout=2, state_file=tmp_path / "state.mat")

    result = run(engine.list_variables())

    assert result.success is True
    assert [variable.name for variable in result.variables] == ["A", "label"]
    assert result.variables[0].size == "2x2"
    assert result.variables[0].bytes == 32
    assert result.variables[0].class_name == "double"
    assert result.variables[1].attributes == "global"


def test_get_variable_uses_readonly_disp_command(tmp_path: Path) -> None:
    executable = write_executable(
        tmp_path / "fake-baltamatica",
        "#!/bin/sh\n"
        "printf '%s\\n' \"$3\"\n",
    )
    state_file = tmp_path / "state.mat"
    engine = CliEngine(executable=str(executable), timeout=2, state_file=state_file)

    result = run(engine.get_variable("A"))

    assert result.success is True
    assert "disp(A)" in result.output
    assert "save(" not in result.output


def test_get_variable_rejects_invalid_name(tmp_path: Path) -> None:
    engine = CliEngine(executable=str(tmp_path / "fake"), timeout=2)

    with pytest.raises(ValueError, match="Invalid variable name"):
        run(engine.get_variable("A; clear"))


def test_parse_whos_output_ignores_non_rows() -> None:
    variables = parse_whos_output(
        "  Name  Size  Bytes  Class   Attributes\n"
        "\n"
        "  A     2x2      32  double\n"
        "not a row\n"
        "  b     1x1       8  double\n"
    )

    assert [variable.name for variable in variables] == ["A", "b"]
