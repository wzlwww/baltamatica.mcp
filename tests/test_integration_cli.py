from __future__ import annotations

import asyncio
import json
import os
import shutil
import socket
import subprocess
import time
from pathlib import Path

import pytest

from baltamatica_mcp.backend_bex import BexEngine
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


@pytest.fixture()
def bex_compiler() -> Path:
    path = Path("/Applications/Baltamatica.app/Contents/MacOS/bex")
    if path.exists():
        return path

    configured = shutil.which("bex")
    if configured is None:
        pytest.skip("Set up the Baltamatica bex compiler to run BEX integration tests.")
    return Path(configured)


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


def test_real_cli_runs_bex_plot_probe(cli_path: Path, bex_compiler: Path) -> None:
    repo_root = Path(__file__).resolve().parents[1]
    source_path = repo_root / "bex" / "bex_plot_probe.c"
    status_path = Path("/tmp/baltamatica_mcp_bex_plot_probe.txt")
    compiled_path = repo_root / "bex_plot_probe.bexmaci64"
    if status_path.exists():
        status_path.unlink()

    subprocess.run(
        [str(bex_compiler), str(source_path)],
        cwd=repo_root,
        check=True,
        capture_output=True,
        text=True,
    )
    try:
        engine = CliEngine(executable=str(cli_path), timeout=30)
        result = run(
            engine.execute_code(f"addpath('{repo_root.as_posix()}'); status=bex_plot_probe()")
        )

        assert result.success is True
        assert status_path.exists()
        status = _read_probe_status(status_path)
        assert status["eval_expression"] == "0"
        assert status["evalin_expression"] == "0"
        assert status["call_sin"] == "0"
        assert status["call_plot"] == "1"
        assert "plot 是未定义的变量或函数" in result.output
    finally:
        if compiled_path.exists():
            compiled_path.unlink()


def test_real_bex_bridge_compiles(bex_compiler: Path) -> None:
    repo_root = Path(__file__).resolve().parents[1]
    source_path = repo_root / "bex" / "mcp_bridge.c"
    compiled_path = repo_root / "mcp_bridge.bexmaci64"

    subprocess.run(
        [str(bex_compiler), str(source_path)],
        cwd=repo_root,
        check=True,
        capture_output=True,
        text=True,
    )
    try:
        assert compiled_path.exists()
    finally:
        if compiled_path.exists():
            compiled_path.unlink()


def test_real_bex_bridge_executes_code_over_tcp(cli_path: Path, bex_compiler: Path) -> None:
    repo_root = Path(__file__).resolve().parents[1]
    source_path = repo_root / "bex" / "mcp_bridge.c"
    compiled_path = repo_root / "mcp_bridge.bexmaci64"

    if _tcp_port_is_listening("127.0.0.1", 31415):
        pytest.skip("BEX bridge port 31415 is already in use.")

    subprocess.run(
        [str(bex_compiler), str(source_path)],
        cwd=repo_root,
        check=True,
        capture_output=True,
        text=True,
    )

    proc = subprocess.Popen(
        [
            str(cli_path),
            "-nodesktop",
            "-s",
            f"addpath('{repo_root.as_posix()}'); mcp_bridge()",
        ],
        cwd=repo_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        _wait_for_tcp_port("127.0.0.1", 31415, proc)
        assign_result, expression_result, failure_result = run(_exercise_bex_bridge())
        _send_bridge_shutdown("127.0.0.1", 31415)

        stdout, stderr = proc.communicate(timeout=8)
        assert assign_result.success is True
        assert expression_result.success is True
        assert failure_result.success is False
        assert failure_result.error is not None
        assert "MCP bridge listening on 127.0.0.1:31415" in stdout
        assert stderr.strip() == ""
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.communicate(timeout=3)
        if compiled_path.exists():
            compiled_path.unlink()


def _read_probe_status(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key] = value
    return values


def _tcp_port_is_listening(host: str, port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(0.2)
        return sock.connect_ex((host, port)) == 0


def _wait_for_tcp_port(host: str, port: int, proc: subprocess.Popen[str]) -> None:
    last_error: OSError | None = None
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            stdout, stderr = proc.communicate(timeout=1)
            raise AssertionError(
                f"BEX bridge exited before listening.\nSTDOUT:\n{stdout}\nSTDERR:\n{stderr}"
            )
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return
        except OSError as exc:
            last_error = exc
            time.sleep(0.2)
    raise AssertionError(f"BEX bridge did not listen on {host}:{port}: {last_error}")


def _send_bridge_shutdown(host: str, port: int) -> None:
    payload = {"id": "shutdown", "method": "shutdown", "params": {}}
    with socket.create_connection((host, port), timeout=2) as sock:
        sock.settimeout(2)
        sock.sendall(json.dumps(payload, separators=(",", ":")).encode("utf-8") + b"\n")
        sock.recv(8192)


async def _exercise_bex_bridge():
    engine = BexEngine(port=31415, timeout=5)
    try:
        assign_result = await engine.execute_code("bex_bridge_smoke=7;")
        expression_result = await engine.execute_code("1+1;")
        failure_result = await engine.execute_code("undefined_bex_bridge_smoke_fn();")
        return assign_result, expression_result, failure_result
    finally:
        await engine.close()
