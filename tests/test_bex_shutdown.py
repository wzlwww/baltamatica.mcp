from __future__ import annotations

import json
import socket
import threading
from collections.abc import Iterator
from contextlib import contextmanager

from baltamatica_mcp.bex_shutdown import main, shutdown_bridge


class ShutdownServer:
    def __init__(self, close_after: int = 1) -> None:
        self.close_after = close_after
        self.requests: list[dict[str, object]] = []
        self._ready = threading.Event()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self.port = 0

    def start(self) -> None:
        self._thread.start()
        assert self._ready.wait(timeout=2)

    def join(self) -> None:
        self._thread.join(timeout=2)

    def _serve(self) -> None:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind(("127.0.0.1", 0))
            server.listen()
            self.port = server.getsockname()[1]
            server.settimeout(0.1)
            self._ready.set()

            while not self._stop.is_set():
                try:
                    client, _ = server.accept()
                except TimeoutError:
                    continue
                with client:
                    data = client.recv(8192)
                    if data:
                        request = json.loads(data.decode("utf-8"))
                        self.requests.append(request)
                        response = {
                            "id": request["id"],
                            "success": True,
                            "output": "shutting down",
                            "artifacts": [],
                        }
                        client.sendall(
                            json.dumps(response, separators=(",", ":")).encode("utf-8") + b"\n"
                        )
                        if len(self.requests) >= self.close_after:
                            self._stop.set()


@contextmanager
def shutdown_server(close_after: int = 1) -> Iterator[ShutdownServer]:
    server = ShutdownServer(close_after=close_after)
    server.start()
    try:
        yield server
    finally:
        server._stop.set()
        server.join()


def test_shutdown_bridge_stops_mock_listener() -> None:
    with shutdown_server() as server:
        result = shutdown_bridge(
            port=server.port,
            timeout=0.2,
            wait_timeout=0.5,
            poll_interval=0.05,
        )

    assert result.stopped is True
    assert result.already_stopped is False
    assert result.attempts == 1
    assert server.requests == [{"id": "shutdown", "method": "shutdown", "params": {}}]


def test_shutdown_bridge_reports_already_stopped() -> None:
    result = shutdown_bridge(port=_unused_port(), timeout=0.1)

    assert result.stopped is True
    assert result.already_stopped is True
    assert result.attempts == 0


def test_shutdown_bridge_retries_until_listener_stops() -> None:
    with shutdown_server(close_after=2) as server:
        result = shutdown_bridge(
            port=server.port,
            timeout=0.2,
            attempts=3,
            wait_timeout=0.2,
            poll_interval=0.05,
        )

    assert result.stopped is True
    assert result.attempts == 2
    assert len(server.requests) == 2


def test_shutdown_main_prints_json(capsys) -> None:
    port = _unused_port()

    assert main(["--port", str(port), "--timeout", "0.1", "--json"]) == 0

    output = json.loads(capsys.readouterr().out)
    assert output["stopped"] is True
    assert output["already_stopped"] is True


def _unused_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]
