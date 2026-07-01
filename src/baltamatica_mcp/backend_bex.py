"""BEX backend client for the Baltamatica JSON-over-TCP bridge."""

from __future__ import annotations

import asyncio
import json
from typing import Any

from baltamatica_mcp.engine import (
    Artifact,
    EngineError,
    EngineUnavailableError,
    ExecutionResult,
    VariableInfo,
    VariableListResult,
)

DEFAULT_BEX_HOST = "127.0.0.1"
DEFAULT_BEX_PORT = 31415
DEFAULT_ARTIFACT_TYPE = "application/octet-stream"


class BexProtocolError(EngineError):
    """Raised when the BEX bridge returns malformed protocol data."""


class _BexConnectionLost(EngineUnavailableError):
    """Internal signal used to retry a dropped transport once."""


class BexEngine:
    """Client for a Baltamatica BEX plugin speaking newline-delimited JSON."""

    backend = "bex"

    def __init__(
        self,
        host: str = DEFAULT_BEX_HOST,
        port: int = DEFAULT_BEX_PORT,
        timeout: float = 30.0,
        reconnect: bool = True,
    ) -> None:
        if port <= 0 or port > 65535:
            raise ValueError(f"Invalid BEX port: {port}")
        if timeout <= 0:
            raise ValueError(f"Invalid BEX timeout: {timeout}")

        self.host = host
        self.port = port
        self.timeout = timeout
        self.reconnect = reconnect
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._request_counter = 0

    async def execute_code(self, code: str) -> ExecutionResult:
        response = await self._request("execute_code", {"code": code})
        return _execution_result_from_response(response)

    async def run_script(self, file_path: str) -> ExecutionResult:
        response = await self._request("run_script", {"file_path": file_path})
        return _execution_result_from_response(response)

    async def clear_workspace(self) -> ExecutionResult:
        response = await self._request("clear_workspace", {})
        return _execution_result_from_response(response)

    async def list_variables(self) -> VariableListResult:
        response = await self._request("list_variables", {})
        return VariableListResult(
            success=bool(response.get("success", False)),
            variables=_variables_from_response(response),
            output=_string_field(response, "output"),
            error=_error_from_response(response),
        )

    async def get_variable(self, name: str) -> ExecutionResult:
        response = await self._request("get_variable", {"name": name})
        return _execution_result_from_response(response)

    async def close(self) -> None:
        """Close the current TCP connection, if one is open."""

        writer = self._writer
        self._reader = None
        self._writer = None
        if writer is None:
            return
        writer.close()
        await writer.wait_closed()

    async def _request(self, method: str, params: dict[str, object]) -> dict[str, Any]:
        payload = {
            "id": self._next_request_id(),
            "method": method,
            "params": params,
        }

        try:
            return await self._send_once(payload)
        except _BexConnectionLost:
            if not self.reconnect:
                raise
            return await self._send_once(payload)

    async def _send_once(self, payload: dict[str, object]) -> dict[str, Any]:
        reader, writer = await self._ensure_connection()
        data = json.dumps(payload, separators=(",", ":")).encode("utf-8") + b"\n"

        try:
            writer.write(data)
            await asyncio.wait_for(writer.drain(), timeout=self.timeout)
            line = await asyncio.wait_for(reader.readline(), timeout=self.timeout)
        except asyncio.TimeoutError as exc:
            await self.close()
            raise TimeoutError(
                f"BEX request '{payload['method']}' timed out after {self.timeout:g} seconds."
            ) from exc
        except (ConnectionError, OSError) as exc:
            await self.close()
            raise _BexConnectionLost(f"BEX connection lost: {exc}") from exc

        if not line:
            await self.close()
            raise _BexConnectionLost("BEX connection closed before a response was received.")

        try:
            response = json.loads(line.decode("utf-8"))
        except json.JSONDecodeError as exc:
            raise BexProtocolError(f"BEX response is not valid JSON: {exc}") from exc

        if not isinstance(response, dict):
            raise BexProtocolError("BEX response must be a JSON object.")
        if response.get("id") != payload["id"]:
            raise BexProtocolError(
                f"BEX response id mismatch: expected {payload['id']!r}, "
                f"got {response.get('id')!r}."
            )
        return response

    async def _ensure_connection(self) -> tuple[asyncio.StreamReader, asyncio.StreamWriter]:
        if self._connection_is_usable():
            return self._reader, self._writer  # type: ignore[return-value]

        await self.close()
        try:
            self._reader, self._writer = await asyncio.wait_for(
                asyncio.open_connection(self.host, self.port),
                timeout=self.timeout,
            )
        except asyncio.TimeoutError as exc:
            raise EngineUnavailableError(
                f"Timed out connecting to BEX bridge at {self.host}:{self.port}."
            ) from exc
        except OSError as exc:
            raise EngineUnavailableError(
                f"Failed to connect to BEX bridge at {self.host}:{self.port}: {exc}"
            ) from exc
        return self._reader, self._writer

    def _connection_is_usable(self) -> bool:
        return (
            self._reader is not None
            and self._writer is not None
            and not self._writer.is_closing()
            and not self._reader.at_eof()
        )

    def _next_request_id(self) -> str:
        self._request_counter += 1
        return str(self._request_counter)


def _execution_result_from_response(response: dict[str, Any]) -> ExecutionResult:
    return ExecutionResult(
        success=bool(response.get("success", False)),
        output=_string_field(response, "output"),
        error=_error_from_response(response),
        artifacts=_artifacts_from_response(response),
    )


def _error_from_response(response: dict[str, Any]) -> str | None:
    error = response.get("error")
    if error is None:
        return None if response.get("success", False) else "BEX request failed."
    if isinstance(error, dict):
        code = str(error.get("code") or "ERROR")
        message = str(error.get("message") or "")
        return f"{code}: {message}" if message else code
    return str(error)


def _artifacts_from_response(response: dict[str, Any]) -> list[Artifact]:
    raw_artifacts = response.get("artifacts") or []
    if not isinstance(raw_artifacts, list):
        return []

    artifacts: list[Artifact] = []
    for item in raw_artifacts:
        if not isinstance(item, dict):
            continue
        path = str(item.get("path") or "")
        if not path:
            continue
        artifacts.append(
            Artifact(
                path=path,
                type=str(item.get("type") or DEFAULT_ARTIFACT_TYPE),
                exists=bool(item.get("exists", False)),
                size=_int_field(item.get("size")),
            )
        )
    return artifacts


def _variables_from_response(response: dict[str, Any]) -> list[VariableInfo]:
    raw_variables = response.get("variables") or []
    if not isinstance(raw_variables, list):
        return []

    variables: list[VariableInfo] = []
    for item in raw_variables:
        if not isinstance(item, dict):
            continue
        name = str(item.get("name") or "")
        if not name:
            continue
        variables.append(
            VariableInfo(
                name=name,
                size=str(item.get("size") or ""),
                bytes=_int_field(item.get("bytes")),
                class_name=str(item.get("class_name") or ""),
                attributes=str(item.get("attributes") or ""),
            )
        )
    return variables


def _string_field(data: dict[str, Any], key: str) -> str:
    value = data.get(key)
    return "" if value is None else str(value)


def _int_field(value: object) -> int:
    try:
        return int(value) if value is not None else 0
    except (TypeError, ValueError):
        return 0
