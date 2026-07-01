"""Utility for stopping a running BEX bridge listener."""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time
from dataclasses import dataclass

from baltamatica_mcp.backend_bex import DEFAULT_BEX_HOST, DEFAULT_BEX_PORT


@dataclass(frozen=True)
class ShutdownResult:
    """Result from a BEX bridge shutdown attempt."""

    stopped: bool
    already_stopped: bool
    attempts: int
    response: str
    error: str | None = None


def shutdown_bridge(
    host: str = DEFAULT_BEX_HOST,
    port: int = DEFAULT_BEX_PORT,
    timeout: float = 2.0,
    attempts: int = 4,
    wait_timeout: float = 2.0,
    poll_interval: float = 0.2,
) -> ShutdownResult:
    """Send shutdown requests until the bridge listener releases its TCP port."""

    if port <= 0 or port > 65535:
        raise ValueError(f"Invalid BEX port: {port}")
    if timeout <= 0:
        raise ValueError(f"Invalid timeout: {timeout}")
    if attempts <= 0:
        raise ValueError(f"Invalid attempts: {attempts}")
    if wait_timeout <= 0:
        raise ValueError(f"Invalid wait timeout: {wait_timeout}")
    if poll_interval <= 0:
        raise ValueError(f"Invalid poll interval: {poll_interval}")

    if not _port_is_open(host, port, timeout):
        return ShutdownResult(
            stopped=True,
            already_stopped=True,
            attempts=0,
            response="",
        )

    last_response = ""
    last_error: str | None = None
    for attempt in range(1, attempts + 1):
        try:
            last_response = _send_shutdown_request(host, port, timeout)
            last_error = None
        except OSError as exc:
            last_error = str(exc)

        deadline = time.monotonic() + wait_timeout
        while time.monotonic() < deadline:
            if not _port_is_open(host, port, timeout):
                return ShutdownResult(
                    stopped=True,
                    already_stopped=False,
                    attempts=attempt,
                    response=last_response,
                    error=last_error,
                )
            time.sleep(poll_interval)

    return ShutdownResult(
        stopped=False,
        already_stopped=False,
        attempts=attempts,
        response=last_response,
        error=last_error,
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Stop a running Baltamatica BEX bridge.")
    parser.add_argument("--host", default=DEFAULT_BEX_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_BEX_PORT)
    parser.add_argument("--timeout", type=float, default=2.0)
    parser.add_argument("--attempts", type=int, default=4)
    parser.add_argument("--wait-timeout", type=float, default=2.0)
    parser.add_argument("--poll-interval", type=float, default=0.2)
    parser.add_argument("--json", action="store_true", help="Print machine-readable JSON.")
    args = parser.parse_args(argv)

    try:
        result = shutdown_bridge(
            host=args.host,
            port=args.port,
            timeout=args.timeout,
            attempts=args.attempts,
            wait_timeout=args.wait_timeout,
            poll_interval=args.poll_interval,
        )
    except Exception as exc:
        if args.json:
            print(json.dumps({"stopped": False, "error": str(exc)}, ensure_ascii=False))
        else:
            print(f"Failed to stop BEX bridge: {exc}", file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps(result.__dict__, ensure_ascii=False))
    elif result.already_stopped:
        print(f"BEX bridge is not listening on {args.host}:{args.port}.")
    elif result.stopped:
        print(f"BEX bridge stopped on {args.host}:{args.port} after {result.attempts} attempt(s).")
    else:
        print(
            f"BEX bridge is still listening on {args.host}:{args.port} "
            f"after {result.attempts} shutdown attempt(s).",
            file=sys.stderr,
        )
        if result.error:
            print(f"Last error: {result.error}", file=sys.stderr)
        return 1
    return 0


def _send_shutdown_request(host: str, port: int, timeout: float) -> str:
    payload = {"id": "shutdown", "method": "shutdown", "params": {}}
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(json.dumps(payload, separators=(",", ":")).encode("utf-8") + b"\n")
        chunks: list[bytes] = []
        while True:
            try:
                data = sock.recv(8192)
            except socket.timeout:
                break
            if not data:
                break
            chunks.append(data)
            if b"\n" in data:
                break
    return b"".join(chunks).decode("utf-8", errors="replace").strip()


def _port_is_open(host: str, port: int, timeout: float) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


if __name__ == "__main__":
    raise SystemExit(main())
