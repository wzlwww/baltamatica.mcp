from __future__ import annotations

import asyncio
from pathlib import Path

from baltamatica_mcp.engine import ExecutionResult, VariableInfo, VariableListResult
from baltamatica_mcp.server import build_arg_parser, create_mcp_server


class FakeEngine:
    backend = "cli"

    def __init__(self) -> None:
        self.executed_code: list[str] = []
        self.ran_scripts: list[str] = []
        self.cleared = False
        self.listed = False
        self.requested_variables: list[str] = []
        self.set_variables: list[tuple[str, object]] = []

    async def execute_code(self, code: str) -> ExecutionResult:
        self.executed_code.append(code)
        return ExecutionResult(success=True, output=f"executed: {code}")

    async def run_script(self, file_path: str) -> ExecutionResult:
        self.ran_scripts.append(file_path)
        return ExecutionResult(success=True, output=f"ran: {file_path}")

    async def clear_workspace(self) -> ExecutionResult:
        self.cleared = True
        return ExecutionResult(success=True, output="cleared")

    async def list_variables(self) -> VariableListResult:
        self.listed = True
        return VariableListResult(
            success=True,
            variables=[VariableInfo(name="A", size="2x2", bytes=32, class_name="double")],
            output="whos output",
        )

    async def get_variable(self, name: str) -> ExecutionResult:
        self.requested_variables.append(name)
        return ExecutionResult(success=True, output=f"value: {name}")

    async def set_variable(self, name: str, data: object) -> ExecutionResult:
        self.set_variables.append((name, data))
        return ExecutionResult(success=True, output=f"set: {name}")


async def _call_tool_result(server, name: str, arguments: dict[str, object]) -> dict[str, object]:
    _, structured_result = await server.call_tool(name, arguments)
    return structured_result


def call_tool_result(server, name: str, arguments: dict[str, object]) -> dict[str, object]:
    return asyncio.run(_call_tool_result(server, name, arguments))


def test_server_registers_tools() -> None:
    server = create_mcp_server(FakeEngine())

    tools = asyncio.run(server.list_tools())

    assert {tool.name for tool in tools} == {
        "clear_workspace",
        "execute_code",
        "get_variable",
        "list_variables",
        "run_script",
        "set_variable",
    }


def test_execute_code_delegates_to_engine() -> None:
    engine = FakeEngine()
    server = create_mcp_server(engine)

    result = call_tool_result(server, "execute_code", {"code": "disp(1 + 1)"})

    assert result == {
        "success": True,
        "output": "executed: disp(1 + 1)",
        "error": None,
        "artifacts": [],
        "value": None,
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
        "artifacts": [],
        "value": None,
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


def test_clear_workspace_delegates_to_engine() -> None:
    engine = FakeEngine()
    server = create_mcp_server(engine)

    result = call_tool_result(server, "clear_workspace", {})

    assert result == {
        "success": True,
        "output": "cleared",
        "error": None,
        "artifacts": [],
        "value": None,
        "backend": "cli",
    }
    assert engine.cleared is True


def test_list_variables_delegates_to_engine() -> None:
    engine = FakeEngine()
    server = create_mcp_server(engine)

    result = call_tool_result(server, "list_variables", {})

    assert result == {
        "success": True,
        "variables": [
            {
                "name": "A",
                "size": "2x2",
                "bytes": 32,
                "class_name": "double",
                "attributes": "",
            }
        ],
        "output": "whos output",
        "error": None,
        "backend": "cli",
    }
    assert engine.listed is True


def test_get_variable_delegates_to_engine() -> None:
    engine = FakeEngine()
    server = create_mcp_server(engine)

    result = call_tool_result(server, "get_variable", {"name": "A"})

    assert result == {
        "success": True,
        "output": "value: A",
        "error": None,
        "artifacts": [],
        "value": None,
        "backend": "cli",
    }
    assert engine.requested_variables == ["A"]


def test_get_variable_rejects_empty_name() -> None:
    engine = FakeEngine()
    server = create_mcp_server(engine)

    result = call_tool_result(server, "get_variable", {"name": "   "})

    assert result["success"] is False
    assert result["backend"] == "cli"
    assert "Variable name cannot be empty" in str(result["error"])
    assert engine.requested_variables == []


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
            "--bex-host",
            "127.0.0.2",
            "--bex-port",
            "43141",
            "--timeout",
            "12.5",
            "--state-file",
            "/tmp/baltamatica-state.mat",
        ]
    )

    assert args.backend == "cli"
    assert args.transport == "sse"
    assert args.cli_executable == "/opt/baltamatica/bin/baltamaticaC.sh"
    assert args.bex_host == "127.0.0.2"
    assert args.bex_port == 43141
    assert args.timeout == 12.5
    assert args.state_file == "/tmp/baltamatica-state.mat"
