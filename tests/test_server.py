from __future__ import annotations

import asyncio
from pathlib import Path

from baltamatica_mcp.engine import ExecutionResult
from baltamatica_mcp.server import build_arg_parser, create_mcp_server


class FakeEngine:
    backend = "cli"

    def __init__(self) -> None:
        self.executed_code: list[str] = []
        self.ran_scripts: list[str] = []

    async def execute_code(self, code: str) -> ExecutionResult:
        self.executed_code.append(code)
        return ExecutionResult(success=True, output=f"executed: {code}")

    async def run_script(self, file_path: str) -> ExecutionResult:
        self.ran_scripts.append(file_path)
        return ExecutionResult(success=True, output=f"ran: {file_path}")


async def _call_tool_result(server, name: str, arguments: dict[str, object]) -> dict[str, object]:
    _, structured_result = await server.call_tool(name, arguments)
    return structured_result


def call_tool_result(server, name: str, arguments: dict[str, object]) -> dict[str, object]:
    return asyncio.run(_call_tool_result(server, name, arguments))


def test_server_registers_phase_one_tools() -> None:
    server = create_mcp_server(FakeEngine())

    tools = asyncio.run(server.list_tools())

    assert {tool.name for tool in tools} == {"execute_code", "run_script"}


def test_execute_code_delegates_to_engine() -> None:
    engine = FakeEngine()
    server = create_mcp_server(engine)

    result = call_tool_result(server, "execute_code", {"code": "disp(1 + 1)"})

    assert result == {
        "success": True,
        "output": "executed: disp(1 + 1)",
        "error": None,
        "backend": "cli",
    }
    assert engine.executed_code == ["disp(1 + 1)"]


def test_execute_code_rejects_empty_code() -> None:
    engine = FakeEngine()
    server = create_mcp_server(engine)

    result = call_tool_result(server, "execute_code", {"code": "   "})

    assert result["success"] is False
    assert result["backend"] == "cli"
    assert "Code cannot be empty" in str(result["error"])
    assert engine.executed_code == []


def test_run_script_delegates_resolved_path_to_engine() -> None:
    engine = FakeEngine()
    server = create_mcp_server(engine)
    script_path = Path("tests/fixtures/sample_script.m").resolve()

    result = call_tool_result(server, "run_script", {"file_path": str(script_path)})

    assert result == {
        "success": True,
        "output": f"ran: {script_path}",
        "error": None,
        "backend": "cli",
    }
    assert engine.ran_scripts == [str(script_path)]


def test_run_script_rejects_missing_script() -> None:
    engine = FakeEngine()
    server = create_mcp_server(engine)

    result = call_tool_result(server, "run_script", {"file_path": "missing.m"})

    assert result["success"] is False
    assert result["backend"] == "cli"
    assert "Script file does not exist" in str(result["error"])
    assert engine.ran_scripts == []


def test_run_script_rejects_non_m_script(tmp_path: Path) -> None:
    script_path = tmp_path / "script.txt"
    script_path.write_text("disp(1)", encoding="utf-8")
    engine = FakeEngine()
    server = create_mcp_server(engine)

    result = call_tool_result(server, "run_script", {"file_path": str(script_path)})

    assert result["success"] is False
    assert result["backend"] == "cli"
    assert "must use .m extension" in str(result["error"])
    assert engine.ran_scripts == []


def test_arg_parser_defaults_to_auto_stdio() -> None:
    args = build_arg_parser().parse_args([])

    assert args.backend == "auto"
    assert args.transport == "stdio"


def test_arg_parser_accepts_backend_and_transport() -> None:
    args = build_arg_parser().parse_args(
        [
            "--backend",
            "cli",
            "--transport",
            "sse",
            "--cli-executable",
            "/opt/baltamatica/bin/baltamaticaC.sh",
            "--timeout",
            "12.5",
        ]
    )

    assert args.backend == "cli"
    assert args.transport == "sse"
    assert args.cli_executable == "/opt/baltamatica/bin/baltamaticaC.sh"
    assert args.timeout == 12.5
