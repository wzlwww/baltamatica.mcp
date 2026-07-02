"""MCP server entry point for Baltamatica."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any, Sequence

from mcp.server.fastmcp import FastMCP

from baltamatica_mcp.engine import (
    BackendName,
    BaltamaticaEngine,
    ExecutionResult,
    VariableListResult,
    create_engine,
)

SERVER_NAME = "baltamatica"


def _success_response(result: ExecutionResult, backend: BackendName) -> dict[str, object]:
    response = result.to_dict()
    response["backend"] = backend
    return response


def _variable_list_response(result: VariableListResult, backend: BackendName) -> dict[str, object]:
    response = result.to_dict()
    response["backend"] = backend
    return response


def _error_response(error: Exception, backend: BackendName) -> dict[str, object]:
    return {
        "success": False,
        "output": "",
        "error": str(error),
        "artifacts": [],
        "backend": backend,
    }


def _resolve_script_path(file_path: str) -> Path:
    path = Path(file_path).expanduser().resolve()
    if not path.exists():
        raise FileNotFoundError(f"Script file does not exist: {path}")
    if not path.is_file():
        raise ValueError(f"Script path is not a file: {path}")
    if path.suffix.lower() != ".m":
        raise ValueError(f"Script file must use .m extension: {path}")
    return path


def create_mcp_server(engine: BaltamaticaEngine | None = None) -> FastMCP:
    """Create and configure the Baltamatica MCP server."""

    selected_engine = engine or create_engine()
    mcp = FastMCP(
        SERVER_NAME,
        instructions=(
            "Execute Baltamatica code and scripts through the configured scientific "
            "computing backend."
        ),
    )

    @mcp.tool()
    async def execute_code(code: str) -> dict[str, object]:
        """Execute a Baltamatica code snippet and return output from the backend."""

        if not code.strip():
            return _error_response(ValueError("Code cannot be empty."), selected_engine.backend)

        try:
            result = await selected_engine.execute_code(code)
        except Exception as exc:
            return _error_response(exc, selected_engine.backend)
        return _success_response(result, selected_engine.backend)

    @mcp.tool()
    async def run_script(file_path: str) -> dict[str, object]:
        """Run a local Baltamatica `.m` script file."""

        try:
            script_path = _resolve_script_path(file_path)
            result = await selected_engine.run_script(str(script_path))
        except Exception as exc:
            return _error_response(exc, selected_engine.backend)
        return _success_response(result, selected_engine.backend)

    @mcp.tool()
    async def clear_workspace() -> dict[str, object]:
        """Clear variables from the Baltamatica workspace state."""

        try:
            result = await selected_engine.clear_workspace()
        except Exception as exc:
            return _error_response(exc, selected_engine.backend)
        return _success_response(result, selected_engine.backend)

    @mcp.tool()
    async def list_variables() -> dict[str, object]:
        """List variables in the Baltamatica workspace state."""

        try:
            result = await selected_engine.list_variables()
        except Exception as exc:
            return _error_response(exc, selected_engine.backend)
        return _variable_list_response(result, selected_engine.backend)

    @mcp.tool()
    async def get_variable(name: str) -> dict[str, object]:
        """Return a display representation of one Baltamatica variable."""

        if not name.strip():
            return _error_response(
                ValueError("Variable name cannot be empty."),
                selected_engine.backend,
            )

        try:
            result = await selected_engine.get_variable(name.strip())
        except Exception as exc:
            return _error_response(exc, selected_engine.backend)
        return _success_response(result, selected_engine.backend)

    @mcp.tool()
    async def set_variable(name: str, data: Any, dtype: str | None = None) -> dict[str, object]:
        """Create or overwrite a workspace variable.

        `data` may be a number, a boolean, a 1-D list (row vector), or a 2-D
        nested list (matrix). Without `dtype`, numbers become double and booleans
        become logical. `dtype` may request an integer type (`int8`..`uint64`),
        `float32`/`float64`, or a complex type (`complex64`/`complex128`, with
        `data={"real": <array>, "imag": <array>}`).
        """

        if not name.strip():
            return _error_response(
                ValueError("Variable name cannot be empty."),
                selected_engine.backend,
            )

        try:
            result = await selected_engine.set_variable(name.strip(), data, dtype)
        except Exception as exc:
            return _error_response(exc, selected_engine.backend)
        return _success_response(result, selected_engine.backend)

    return mcp


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run the Baltamatica MCP server.")
    parser.add_argument(
        "--backend",
        choices=("auto", "cli", "bex"),
        default="auto",
        help="Backend used to communicate with Baltamatica.",
    )
    parser.add_argument(
        "--transport",
        choices=("stdio", "sse", "streamable-http"),
        default="stdio",
        help="MCP transport to serve.",
    )
    parser.add_argument(
        "--cli-executable",
        default=None,
        help="Path/command for the Baltamatica launcher. Defaults to BALTAMATICA_CLI, "
        "then the launcher on PATH, then the standard install location.",
    )
    parser.add_argument(
        "--bex-host",
        default="127.0.0.1",
        help="Host for the BEX JSON TCP bridge.",
    )
    parser.add_argument(
        "--bex-port",
        type=int,
        default=31415,
        help="Port for the BEX JSON TCP bridge.",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=30.0,
        help="Seconds to wait for each Baltamatica CLI command.",
    )
    parser.add_argument(
        "--state-file",
        default=None,
        help="Optional .mat file used to persist CLI backend workspace state.",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> None:
    """Start the MCP server, or run the `install-bridge` helper subcommand."""

    raw = list(sys.argv[1:] if argv is None else argv)
    if raw and raw[0] == "install-bridge":
        from baltamatica_mcp.bridge_install import install_bridge_cli

        install_bridge_cli(raw[1:])
        return

    args = build_arg_parser().parse_args(raw)
    engine = create_engine(
        args.backend,
        cli_executable=args.cli_executable,
        bex_host=args.bex_host,
        bex_port=args.bex_port,
        timeout=args.timeout,
        state_file=args.state_file,
    )
    create_mcp_server(engine).run(transport=args.transport)


if __name__ == "__main__":
    main()
