from __future__ import annotations

import asyncio
import os
from pathlib import Path

import pytest

from baltamatica_mcp.backend_cli import CliEngine
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
    engine = CliEngine(executable=str(executable), timeout=2)

    result = run(engine.execute_code("disp(1 + 1)"))

    assert result.success is True
    assert result.error is None
    assert "argv:-nodesktop -s disp(1 + 1)" in result.output


def test_execute_code_returns_nonzero_exit_as_failed_result(tmp_path: Path) -> None:
    executable = write_executable(
        tmp_path / "fake-baltamatica",
        "#!/bin/sh\n"
        "echo 'bad code' >&2\n"
        "exit 7\n",
    )
    engine = CliEngine(executable=str(executable), timeout=2)

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
    engine = CliEngine(executable=str(executable), timeout=2)

    result = run(engine.run_script(str(script_path)))

    assert result.success is True
    escaped_path = script_path.as_posix().replace("'", "''")
    assert result.output == f"run('{escaped_path}')"


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
