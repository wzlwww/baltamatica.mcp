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
    assert "bxCallBaltamatica(0, NULL, 1, (const bxArray **)args, \"eval\")" in source
    assert "mcp_eval_command(request->code)" in source
    assert 'strcmp(request->method, BALTAMATICA_MCP_METHOD_EXECUTE_CODE)' in source
    assert 'strcmp(request->method, BALTAMATICA_MCP_METHOD_RUN_SCRIPT)' in source
    assert 'strcmp(request->method, BALTAMATICA_MCP_METHOD_CLEAR_WORKSPACE)' in source
    assert 'strcmp(request->method, BALTAMATICA_MCP_METHOD_LIST_VARIABLES)' in source
    assert 'strcmp(request->method, BALTAMATICA_MCP_METHOD_GET_VARIABLE)' in source
    assert "BALTAMATICA_MCP_METHOD_SHUTDOWN" in source
    assert "mcp_array_to_json_value" in source
    assert 'mcp_print_bridge_message("ready", "at", port)' in source
    assert 'mcp_print_bridge_message("listening", "on", port)' in source
    assert 'mcp_print_bridge_message("stopped", "on", port)' in source
    assert 'bxPrintf(' in source
    assert '"MCP bridge %s %s %s:%d\\n"' in source
    assert "mcp_array_is_stop_command" in source
    assert "bxGetCharsRO(value)" in source
    assert "bxIsString(value)" in source
    assert "mcp_send_variable_binary" in source
    assert "mcp_stream_base64" in source
    assert "data_b64" in source
    assert "bxGetComplexDoublesRO" in source
    assert "bxGetString(value, i)" in source
    assert "mcp_send_shutdown_request" in source
    assert "mcp_set_close_on_exec(server_fd)" in source
    assert "mcp_array_is_background_command" in source
    assert "mcp_start_background_bridge" in source
    assert "mcp_background_thread_main" in source
    assert 'mcp_print_bridge_text("background listening on", port)' in source
    assert 'const char *request = "{\\"id\\":\\"stop\\",\\"method\\":\\"shutdown\\",\\"params\\":{}}\\n";' in source
    assert "mcp_bridge('%s') accepts at most one optional port argument." in source


def test_bex_protocol_header_matches_python_defaults() -> None:
    header = read_repo_file("bex/mcp_protocol.h")

    assert f"#define BALTAMATICA_MCP_DEFAULT_PORT {DEFAULT_BEX_PORT}" in header
    assert '#define BALTAMATICA_MCP_DEFAULT_HOST "127.0.0.1"' in header
    assert '#define BALTAMATICA_MCP_METHOD_EXECUTE_CODE "execute_code"' in header
    assert '#define BALTAMATICA_MCP_METHOD_LIST_VARIABLES "list_variables"' in header
    assert '#define BALTAMATICA_MCP_METHOD_GET_VARIABLE "get_variable"' in header
    assert '#define BALTAMATICA_MCP_ERROR_BAD_REQUEST "BAD_REQUEST"' in header
    assert '#define BALTAMATICA_MCP_ERROR_VARIABLE "VARIABLE_ERROR"' in header


def test_bex_cmake_uses_baltamatica_sdk_helper() -> None:
    cmake = read_repo_file("bex/CMakeLists.txt")

    assert "find_package(Baltamatica 4.1 REQUIRED)" in cmake
    assert "find_package(Threads REQUIRED)" in cmake
    assert "add_baltamatica_bex(mcp_bridge mcp_bridge.c)" in cmake
    assert "target_link_libraries(mcp_bridge PRIVATE ws2_32)" in cmake
    assert "target_link_libraries(mcp_bridge PRIVATE Threads::Threads)" in cmake


def test_bex_plugin_documentation_covers_manual_workflow() -> None:
    docs = read_repo_file("docs/bex-plugin.md")

    assert "bex mcp_bridge.c" in docs
    assert "mcp_bridge(43141)" in docs
    assert "mcp_bridge('stop')" in docs
    assert "--backend bex" in docs
    assert "list_variables" in docs
    assert "structured JSON" in docs
