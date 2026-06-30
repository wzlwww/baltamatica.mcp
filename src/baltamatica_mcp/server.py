"""MCP server entry point for Baltamatica."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Sequence

from mcp.server.fastmcp import FastMCP

from baltamatica_mcp.engine import (
    BackendName,
    BaltamaticaEngine,
    ExecutionResult,
    create_engine,
)

SERVER_NAME = "baltamatica"


def _success_response(result: ExecutionResult, backend: BackendName) -> dict[str, object]:
    response = result.to_dict()
    response["backend"] = backend
    return response


def _error_response(error: Exception, backend: BackendName) -> dict[str, object]:
    return {
        "success": False,
        "output": "",
        "error": str(error),
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
    return parser


def main(argv: Sequence[str] | None = None) -> None:
    """Start the MCP server."""

    args = build_arg_parser().parse_args(argv)
    engine = create_engine(args.backend)
    create_mcp_server(engine).run(transport=args.transport)


if __name__ == "__main__":
    main()
