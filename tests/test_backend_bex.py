from __future__ import annotations

import asyncio
import json
from contextlib import suppress
from collections.abc import Awaitable, Callable
from typing import Any

import pytest

from baltamatica_mcp.backend_bex import BexEngine, BexProtocolError
from baltamatica_mcp.engine import EngineUnavailableError


JsonResponse = dict[str, Any] | None
Responder = Callable[[dict[str, Any], int], JsonResponse | Awaitable[JsonResponse]]


def run(coro):
    return asyncio.run(coro)


async def start_mock_bex_server(
    responder: Responder,
) -> tuple[asyncio.AbstractServer, list[dict[str, Any]], int]:
    requests: list[dict[str, Any]] = []

    async def handle_client(
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
    ) -> None:
        with suppress(ConnectionError, OSError):
            while True:
                line = await reader.readline()
                if not line:
                    break

                request = json.loads(line.decode("utf-8"))
                requests.append(request)
                response = responder(request, len(requests))
                if asyncio.iscoroutine(response):
                    response = await response
                if response is None:
                    break

                writer.write(json.dumps(response, separators=(",", ":")).encode("utf-8") + b"\n")
                await writer.drain()

        writer.close()
        with suppress(ConnectionError, OSError):
            await writer.wait_closed()

    server = await asyncio.start_server(handle_client, "127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]
    return server, requests, port


def response_for(request: dict[str, Any], **fields: object) -> dict[str, Any]:
    response: dict[str, Any] = {"id": request["id"], "success": True}
    response.update(fields)
    return response


async def close_server(server: asyncio.AbstractServer) -> None:
    server.close()
    await server.wait_closed()


def test_execute_code_sends_json_request_and_parses_result() -> None:
    async def exercise():
        server, requests, port = await start_mock_bex_server(
            lambda request, _: response_for(
                request,
                output="42",
                artifacts=[
                    {
                        "path": "/tmp/plot.png",
                        "type": "image/png",
                        "exists": True,
                        "size": 12,
                    }
                ],
            )
        )
        engine = BexEngine(port=port, timeout=1)
        try:
            result = await engine.execute_code("disp(6 * 7)")
        finally:
            await engine.close()
            await close_server(server)
        return result, requests

    result, requests = run(exercise())

    assert result.success is True
    assert result.output == "42"
    assert result.error is None
    assert result.artifacts is not None
    assert result.artifacts[0].path == "/tmp/plot.png"
    assert result.artifacts[0].type == "image/png"
    assert result.artifacts[0].exists is True
    assert result.artifacts[0].size == 12
    assert requests == [
        {"id": "1", "method": "execute_code", "params": {"code": "disp(6 * 7)"}}
    ]


def test_run_script_and_clear_workspace_use_expected_methods() -> None:
    async def exercise():
        server, requests, port = await start_mock_bex_server(
            lambda request, _: response_for(request, output=request["method"])
        )
        engine = BexEngine(port=port, timeout=1)
        try:
            script_result = await engine.run_script("/tmp/demo.m")
            clear_result = await engine.clear_workspace()
        finally:
            await engine.close()
            await close_server(server)
        return script_result, clear_result, requests

    script_result, clear_result, requests = run(exercise())

    assert script_result.output == "run_script"
    assert clear_result.output == "clear_workspace"
    assert requests == [
        {"id": "1", "method": "run_script", "params": {"file_path": "/tmp/demo.m"}},
        {"id": "2", "method": "clear_workspace", "params": {}},
    ]


def test_list_variables_parses_variable_metadata() -> None:
    async def exercise():
        server, requests, port = await start_mock_bex_server(
            lambda request, _: response_for(
                request,
                output="whos",
                variables=[
                    {
                        "name": "A",
                        "size": "2x2",
                        "bytes": 32,
                        "class_name": "double",
                        "attributes": "global",
                    }
                ],
            )
        )
        engine = BexEngine(port=port, timeout=1)
        try:
            result = await engine.list_variables()
        finally:
            await engine.close()
            await close_server(server)
        return result, requests

    result, requests = run(exercise())

    assert result.success is True
    assert result.output == "whos"
    assert len(result.variables) == 1
    assert result.variables[0].name == "A"
    assert result.variables[0].size == "2x2"
    assert result.variables[0].bytes == 32
    assert result.variables[0].class_name == "double"
    assert result.variables[0].attributes == "global"
    assert requests == [{"id": "1", "method": "list_variables", "params": {}}]


def test_get_variable_returns_failed_execution_result_for_bex_error() -> None:
    async def exercise():
        server, requests, port = await start_mock_bex_server(
            lambda request, _: response_for(
                request,
                success=False,
                output="",
                error={"code": "VARIABLE_ERROR", "message": "missing variable A"},
            )
        )
        engine = BexEngine(port=port, timeout=1)
        try:
            result = await engine.get_variable("A")
        finally:
            await engine.close()
            await close_server(server)
        return result, requests

    result, requests = run(exercise())

    assert result.success is False
    assert result.error == "VARIABLE_ERROR: missing variable A"
    assert requests == [{"id": "1", "method": "get_variable", "params": {"name": "A"}}]


def test_connection_drop_reconnects_and_retries_request_once() -> None:
    async def exercise():
        async def responder(request: dict[str, Any], count: int) -> JsonResponse:
            if count == 2:
                return response_for(request, output="after reconnect")
            return None

        server, requests, port = await start_mock_bex_server(responder)
        engine = BexEngine(port=port, timeout=1)
        try:
            result = await engine.execute_code("disp(1)")
        finally:
            await engine.close()
            await close_server(server)
        return result, requests

    result, requests = run(exercise())

    assert result.success is True
    assert result.output == "after reconnect"
    assert len(requests) == 2
    assert requests[0] == requests[1]


def test_timeout_closes_connection_and_raises_timeout() -> None:
    async def exercise():
        async def responder(_: dict[str, Any], __: int) -> JsonResponse:
            await asyncio.sleep(1)
            return {"id": "late", "success": True}

        server, _, port = await start_mock_bex_server(responder)
        engine = BexEngine(port=port, timeout=0.05)
        try:
            with pytest.raises(TimeoutError, match="timed out"):
                await engine.execute_code("pause(1)")
        finally:
            await engine.close()
            await close_server(server)

    run(exercise())


def test_mismatched_response_id_raises_protocol_error() -> None:
    async def exercise():
        server, _, port = await start_mock_bex_server(
            lambda request, _: response_for(request, id="unexpected")
        )
        engine = BexEngine(port=port, timeout=1)
        try:
            with pytest.raises(BexProtocolError, match="id mismatch"):
                await engine.execute_code("disp(1)")
        finally:
            await engine.close()
            await close_server(server)

    run(exercise())


def test_missing_bex_server_raises_unavailable() -> None:
    async def exercise():
        server = await asyncio.start_server(lambda _r, _w: None, "127.0.0.1", 0)
        port = server.sockets[0].getsockname()[1]
        await close_server(server)

        engine = BexEngine(port=port, timeout=0.1)
        with pytest.raises(EngineUnavailableError):
            await engine.execute_code("disp(1)")

    run(exercise())
