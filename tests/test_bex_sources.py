from __future__ import annotations

from pathlib import Path

from baltamatica_mcp.backend_bex import DEFAULT_BEX_PORT


ROOT = Path(__file__).resolve().parents[1]


def read_repo_file(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_bex_bridge_implements_sdk_entrypoint_and_minimal_methods() -> None:
    source = read_repo_file("bex/mcp_bridge.c")

    assert '#include "bex/bex.h"' in source
    assert '#include "mcp_protocol.h"' in source
    assert "void bexFunction(" in source
    assert "bxEvalString(request->code)" in source
    assert 'strcmp(request->method, BALTAMATICA_MCP_METHOD_EXECUTE_CODE)' in source
    assert 'strcmp(request->method, BALTAMATICA_MCP_METHOD_RUN_SCRIPT)' in source
    assert 'strcmp(request->method, BALTAMATICA_MCP_METHOD_CLEAR_WORKSPACE)' in source
    assert "BALTAMATICA_MCP_METHOD_SHUTDOWN" in source


def test_bex_protocol_header_matches_python_defaults() -> None:
    header = read_repo_file("bex/mcp_protocol.h")

    assert f"#define BALTAMATICA_MCP_DEFAULT_PORT {DEFAULT_BEX_PORT}" in header
    assert '#define BALTAMATICA_MCP_DEFAULT_HOST "127.0.0.1"' in header
    assert '#define BALTAMATICA_MCP_METHOD_EXECUTE_CODE "execute_code"' in header
    assert '#define BALTAMATICA_MCP_ERROR_BAD_REQUEST "BAD_REQUEST"' in header


def test_bex_cmake_uses_baltamatica_sdk_helper() -> None:
    cmake = read_repo_file("bex/CMakeLists.txt")

    assert "find_package(Baltamatica 4.1 REQUIRED)" in cmake
    assert "add_baltamatica_bex(mcp_bridge mcp_bridge.c)" in cmake
    assert "target_link_libraries(mcp_bridge PRIVATE ws2_32)" in cmake


def test_bex_plugin_documentation_covers_manual_workflow() -> None:
    docs = read_repo_file("docs/bex-plugin.md")

    assert "bex mcp_bridge.c" in docs
    assert "mcp_bridge(43141)" in docs
    assert "--backend bex" in docs
    assert "list_variables" in docs
    assert "Deferred to PR8" in docs
