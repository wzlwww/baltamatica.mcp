from __future__ import annotations

import asyncio
import os
import sys
from pathlib import Path

import pytest

from baltamatica_mcp.backend_bex import BexEngine
from baltamatica_mcp.backend_cli import (
    CliEngine,
    detect_baltamatica_error,
    parse_artifacts,
    parse_whos_output,
)
from baltamatica_mcp.engine import EngineUnavailableError, create_engine


def write_executable(path: Path, body: str) -> Path:
    path.write_text(body, encoding="utf-8")
    path.chmod(0o755)
    return path


def write_python_executable(path: Path, body: str) -> Path:
    if os.name == "nt":
        script_path = path.with_suffix(".py")
        script_path.write_text(body, encoding="utf-8")
        wrapper_path = path.with_suffix(".cmd")
        wrapper_path.write_text(
            f'@echo off\r\n"{sys.executable}" "{script_path}" %*\r\n',
            encoding="utf-8",
        )
        return wrapper_path
    return write_executable(path, f"#!{sys.executable}\n{body}")


def run(coro):
    return asyncio.run(coro)


def test_create_engine_returns_cli_for_cli_and_auto() -> None:
    assert isinstance(create_engine("cli"), CliEngine)
    assert isinstance(create_engine("auto"), CliEngine)


def test_create_engine_returns_bex_for_bex_backend() -> None:
    engine = create_engine("bex")

    assert isinstance(engine, BexEngine)
    assert engine.backend == "bex"


def test_execute_code_runs_baltamatica_cli(tmp_path: Path) -> None:
    executable = write_python_executable(
        tmp_path / "fake-baltamatica",
        "import sys\n"
        "print('argv:' + ' '.join(sys.argv[1:]))\n",
    )
    state_file = tmp_path / "state.mat"
    engine = CliEngine(executable=str(executable), timeout=10, state_file=state_file)

    result = run(engine.execute_code("disp(1 + 1)"))

    assert result.success is True
    assert result.error is None
    assert "argv:-nodesktop -s" in result.output
    assert "disp(1 + 1)" in result.output
    assert f"save('{state_file.as_posix()}')" in result.output


def test_execute_code_returns_nonzero_exit_as_failed_result(tmp_path: Path) -> None:
    executable = write_python_executable(
        tmp_path / "fake-baltamatica",
        "import sys\n"
        "print('bad code', file=sys.stderr)\n"
        "sys.exit(7)\n",
    )
    engine = CliEngine(executable=str(executable), timeout=10, state_file=tmp_path / "state.mat")

    result = run(engine.execute_code("bad"))

    assert result.success is False
    assert result.output == "bad code"
    assert result.error == "Baltamatica CLI exited with code 7: bad code"
    assert result.artifacts == []


def test_execute_code_decodes_utf8_output_on_windows_codepages(tmp_path: Path) -> None:
    executable = write_python_executable(
        tmp_path / "fake-baltamatica",
        "import sys\n"
        "sys.stdout.buffer.write('错误使用函数 fprintf\\n'.encode('utf-8'))\n",
    )
    engine = CliEngine(executable=str(executable), timeout=10, state_file=tmp_path / "state.mat")

    result = run(engine.execute_code("bad"))

    assert result.success is False
    assert "错误使用函数 fprintf" in result.output
    assert result.error == "Baltamatica reported an error: 错误使用函数 fprintf"


def test_execute_code_filters_windows_log_cleanup_noise(tmp_path: Path) -> None:
    executable = write_python_executable(
        tmp_path / "fake-baltamatica",
        "print('2')\n"
        "print('Failed to delete old log: \"C:\\\\temp\\\\old.log\"')\n",
    )
    engine = CliEngine(executable=str(executable), timeout=10, state_file=tmp_path / "state.mat")

    result = run(engine.execute_code("disp(1+1)"))

    assert result.success is True
    assert result.output == "2"


def test_execute_code_parses_artifact_marker(tmp_path: Path) -> None:
    artifact_path = tmp_path / "plot.png"
    artifact_path.write_bytes(b"fake-png")
    executable = write_python_executable(
        tmp_path / "fake-baltamatica",
        "import sys\n"
        f"print('BALTAMATICA_ARTIFACT={artifact_path.as_posix()}')\n",
    )
    engine = CliEngine(executable=str(executable), timeout=10, state_file=tmp_path / "state.mat")

    result = run(engine.execute_code("disp(1)"))

    assert result.success is True
    assert result.artifacts is not None
    assert len(result.artifacts) == 1
    assert result.artifacts[0].path == str(artifact_path.resolve())
    assert result.artifacts[0].type == "image/png"
    assert result.artifacts[0].exists is True
    assert result.artifacts[0].size == len(b"fake-png")


def test_run_script_uses_run_command_with_escaped_path(tmp_path: Path) -> None:
    executable = write_python_executable(
        tmp_path / "fake-baltamatica",
        "import sys\n"
        "print(sys.argv[3])\n",
    )
    script_path = tmp_path / "name'with quote.m"
    script_path.write_text("disp(1)", encoding="utf-8")
    engine = CliEngine(executable=str(executable), timeout=10, state_file=tmp_path / "state.mat")

    result = run(engine.run_script(str(script_path)))

    assert result.success is True
    escaped_path = script_path.as_posix().replace("'", "''")
    assert f"run('{escaped_path}')" in result.output


def test_missing_configured_executable_raises_unavailable(tmp_path: Path) -> None:
    engine = CliEngine(executable=str(tmp_path / "missing"))

    with pytest.raises(EngineUnavailableError, match="does not exist"):
        run(engine.execute_code("disp(1)"))


def test_environment_executable_is_used(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    executable = write_python_executable(
        tmp_path / "fake-baltamatica",
        "print('env-executable')\n",
    )
    monkeypatch.setenv("BALTAMATICA_CLI", str(executable))
    engine = CliEngine(timeout=10)

    result = run(engine.execute_code("disp(1)"))

    assert result.success is True
    assert result.output == "env-executable"


def test_timeout_kills_process(tmp_path: Path) -> None:
    executable = write_python_executable(
        tmp_path / "fake-baltamatica",
        "import time\n"
        "time.sleep(2)\n",
    )
    engine = CliEngine(executable=str(executable), timeout=0.1)

    with pytest.raises(TimeoutError, match="timed out"):
        run(engine.execute_code("disp(1)"))


def test_path_lookup_uses_command_name(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    executable_name = "baltamaticaC.cmd" if os.name == "nt" else "baltamaticaC.sh"
    executable = write_python_executable(
        tmp_path / executable_name,
        "print('path-lookup')\n",
    )
    monkeypatch.setenv("PATH", f"{tmp_path}{os.pathsep}{os.environ.get('PATH', '')}")
    engine = CliEngine(executable=executable.name, timeout=10)

    result = run(engine.execute_code("disp(1)"))

    assert result.success is True
    assert result.output == "path-lookup"


def test_clear_workspace_removes_state_file(tmp_path: Path) -> None:
    executable = write_python_executable(
        tmp_path / "fake-baltamatica",
        "import sys\n"
        "print(sys.argv[3])\n",
    )
    state_file = tmp_path / "state.mat"
    state_file.write_text("state", encoding="utf-8")
    engine = CliEngine(executable=str(executable), timeout=10, state_file=state_file)

    result = run(engine.clear_workspace())

    assert result.success is True
    assert result.output == "clear"
    assert not state_file.exists()


def test_list_variables_parses_whos_output(tmp_path: Path) -> None:
    executable = write_python_executable(
        tmp_path / "fake-baltamatica",
        "print('  Name  Size  Bytes  Class   Attributes')\n"
        "print()\n"
        "print('  A     2x2      32  double')\n"
        "print('  label 1x5      10  char    global')\n",
    )
    engine = CliEngine(executable=str(executable), timeout=10, state_file=tmp_path / "state.mat")

    result = run(engine.list_variables())

    assert result.success is True
    assert [variable.name for variable in result.variables] == ["A", "label"]
    assert result.variables[0].size == "2x2"
    assert result.variables[0].bytes == 32
    assert result.variables[0].class_name == "double"
    assert result.variables[1].attributes == "global"


def test_get_variable_uses_readonly_disp_command(tmp_path: Path) -> None:
    executable = write_python_executable(
        tmp_path / "fake-baltamatica",
        "import sys\n"
        "print(sys.argv[3])\n",
    )
    state_file = tmp_path / "state.mat"
    engine = CliEngine(executable=str(executable), timeout=10, state_file=state_file)

    result = run(engine.get_variable("A"))

    assert result.success is True
    assert "disp(A)" in result.output
    assert "save(" not in result.output


def test_get_variable_rejects_invalid_name(tmp_path: Path) -> None:
    engine = CliEngine(executable=str(tmp_path / "fake"), timeout=10)

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


def test_parse_artifacts_supports_explicit_type_and_missing_file(tmp_path: Path) -> None:
    output_path = tmp_path / "table.csv"

    artifacts = parse_artifacts(f"BALTAMATICA_ARTIFACT=text/csv:{output_path}\n")

    assert len(artifacts) == 1
    assert artifacts[0].path == str(output_path.resolve())
    assert artifacts[0].type == "text/csv"
    assert artifacts[0].exists is False
    assert artifacts[0].size == 0


def test_detect_baltamatica_error_strips_ansi_sequences() -> None:
    output = "\x1b[91m错误使用函数 fprintf\n位于文件 demo.m\x1b[0m"

    assert detect_baltamatica_error(output) == (
        "Baltamatica reported an error: 错误使用函数 fprintf"
    )


def test_resolve_executable_auto_detects_standard_location(tmp_path, monkeypatch) -> None:
    fake = tmp_path / "baltamatica"
    fake.write_text("#!/bin/sh\n")
    fake.chmod(0o755)
    monkeypatch.delenv("BALTAMATICA_CLI", raising=False)
    monkeypatch.setattr("baltamatica_mcp.backend_cli.shutil.which", lambda *_: None)
    monkeypatch.setattr("baltamatica_mcp.backend_cli.platform.system", lambda: "TestOS")
    monkeypatch.setattr(
        "baltamatica_mcp.backend_cli.COMMON_EXECUTABLES", {"TestOS": [str(fake)]}
    )
    engine = CliEngine()  # nothing configured, not on PATH
    assert engine._resolve_executable() == str(fake)


def test_resolve_executable_errors_when_nothing_found(monkeypatch) -> None:
    monkeypatch.delenv("BALTAMATICA_CLI", raising=False)
    monkeypatch.setattr("baltamatica_mcp.backend_cli.shutil.which", lambda *_: None)
    monkeypatch.setattr("baltamatica_mcp.backend_cli.platform.system", lambda: "TestOS")
    monkeypatch.setattr("baltamatica_mcp.backend_cli.COMMON_EXECUTABLES", {"TestOS": []})
    engine = CliEngine()
    with pytest.raises(EngineUnavailableError):
        engine._resolve_executable()


def test_cli_subprocess_env_forces_offscreen_on_headless_linux(monkeypatch) -> None:
    from baltamatica_mcp.backend_cli import _cli_subprocess_env

    monkeypatch.setattr("baltamatica_mcp.backend_cli.platform.system", lambda: "Linux")
    monkeypatch.delenv("DISPLAY", raising=False)
    monkeypatch.delenv("QT_QPA_PLATFORM", raising=False)
    assert _cli_subprocess_env()["QT_QPA_PLATFORM"] == "offscreen"


def test_cli_subprocess_env_keeps_display(monkeypatch) -> None:
    from baltamatica_mcp.backend_cli import _cli_subprocess_env

    monkeypatch.setattr("baltamatica_mcp.backend_cli.platform.system", lambda: "Linux")
    monkeypatch.setenv("DISPLAY", ":0")
    monkeypatch.delenv("QT_QPA_PLATFORM", raising=False)
    assert "QT_QPA_PLATFORM" not in _cli_subprocess_env()
