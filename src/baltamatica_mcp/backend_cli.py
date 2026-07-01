"""CLI backend for running Baltamatica commands through baltamaticaC.sh."""

from __future__ import annotations

import asyncio
import locale
import os
import re
import shutil
import subprocess
import tempfile
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from baltamatica_mcp.engine import (
    Artifact,
    ExecutionResult,
    EngineUnavailableError,
    VariableInfo,
    VariableListResult,
)

DEFAULT_EXECUTABLE = "baltamaticaC.sh"
ENV_EXECUTABLE = "BALTAMATICA_CLI"
ARTIFACT_PREFIX = "BALTAMATICA_ARTIFACT="
VAR_NAME_PATTERN = re.compile(r"^[A-Za-z]\w*$")
ERROR_OUTPUT_PATTERNS = (
    "\u672a\u5b9a\u4e49\u7684\u53d8\u91cf\u6216\u51fd\u6570",
    "\u672a\u5b9a\u4e49\u7684\u51fd\u6570",
    "\u662f\u672a\u5b9a\u4e49\u7684\u53d8\u91cf\u6216\u51fd\u6570",
    "\u4f4d\u4e8e\u8f93\u5165\u7684\u7b2c",
    "\u4f4d\u4e8e\u6587\u4ef6",
)
WHOS_ROW_PATTERN = re.compile(
    r"^\s*(?P<name>[A-Za-z]\w*)\s+"
    r"(?P<size>\S+)\s+"
    r"(?P<bytes>\d+)\s+"
    r"(?P<class_name>\S+)"
    r"(?:\s+(?P<attributes>.*?))?\s*$"
)


@dataclass(frozen=True)
class CliProcessResult:
    """Raw process result from the Baltamatica CLI."""

    returncode: int
    stdout: str
    stderr: str


class CliEngine:
    """Execute Baltamatica code by spawning the command-line runtime."""

    backend = "cli"

    def __init__(
        self,
        executable: str | None = None,
        timeout: float = 30.0,
        state_file: str | Path | None = None,
    ) -> None:
        self._configured_executable = executable
        self.timeout = timeout
        self.state_file = Path(state_file) if state_file else _default_state_file()

    async def execute_code(self, code: str) -> ExecutionResult:
        return await self._execute_command(self._wrap_stateful_code(code))

    async def run_script(self, file_path: str) -> ExecutionResult:
        script_command = f"run('{_escape_baltamatica_string(Path(file_path).as_posix())}')"
        return await self.execute_code(script_command)

    async def clear_workspace(self) -> ExecutionResult:
        if self.state_file.exists():
            self.state_file.unlink()
        return await self._execute_command("clear")

    async def list_variables(self) -> VariableListResult:
        result = await self._execute_command(self._wrap_readonly_code("whos"))
        if not result.success:
            return VariableListResult(
                success=False,
                variables=[],
                output=result.output,
                error=result.error,
            )
        return VariableListResult(
            success=True,
            variables=parse_whos_output(result.output),
            output=result.output,
        )

    async def get_variable(self, name: str) -> ExecutionResult:
        if not VAR_NAME_PATTERN.match(name):
            raise ValueError(f"Invalid variable name: {name}")
        return await self._execute_command(self._wrap_readonly_code(f"disp({name})"))

    def _resolve_executable(self) -> str:
        configured = self._configured_executable or os.environ.get(ENV_EXECUTABLE)
        if configured:
            path = Path(configured).expanduser()
            if path.is_absolute() or len(path.parts) > 1:
                if not path.exists():
                    raise EngineUnavailableError(
                        f"Baltamatica CLI executable does not exist: {path}"
                    )
                if not path.is_file():
                    raise EngineUnavailableError(f"Baltamatica CLI path is not a file: {path}")
                return str(path)
            resolved = shutil.which(configured)
            if resolved:
                return resolved
            raise EngineUnavailableError(
                f"Baltamatica CLI executable not found on PATH: {configured}"
            )

        resolved = shutil.which(DEFAULT_EXECUTABLE)
        if resolved:
            return resolved
        raise EngineUnavailableError(
            f"Baltamatica CLI executable not found. Set {ENV_EXECUTABLE} or pass --cli-executable."
        )

    async def _run_cli(self, argv: Sequence[str]) -> CliProcessResult:
        return await asyncio.to_thread(self._run_cli_sync, argv)

    async def _execute_command(self, code: str) -> ExecutionResult:
        process = await self._run_cli([self._resolve_executable(), "-nodesktop", "-s", code])
        output = _combine_output(process.stdout, process.stderr)
        artifacts = parse_artifacts(output)
        if process.returncode != 0:
            return ExecutionResult(
                success=False,
                output=output,
                error=_process_error(process),
                artifacts=artifacts,
            )
        output_error = detect_baltamatica_error(output)
        if output_error:
            return ExecutionResult(
                success=False,
                output=output,
                error=output_error,
                artifacts=artifacts,
            )
        return ExecutionResult(success=True, output=output, artifacts=artifacts)

    def _wrap_stateful_code(self, code: str) -> str:
        state_path = _escape_baltamatica_string(self.state_file.as_posix())
        return (
            f"if exist('{state_path}','file'), load('{state_path}'); end; "
            f"{code}; "
            f"save('{state_path}');"
        )

    def _wrap_readonly_code(self, code: str) -> str:
        state_path = _escape_baltamatica_string(self.state_file.as_posix())
        return f"if exist('{state_path}','file'), load('{state_path}'); end; {code};"

    def _run_cli_sync(self, argv: Sequence[str]) -> CliProcessResult:
        try:
            process = subprocess.run(
                argv,
                capture_output=True,
                check=False,
                timeout=self.timeout,
            )
        except subprocess.TimeoutExpired as exc:
            raise TimeoutError(
                f"Baltamatica CLI timed out after {self.timeout:g} seconds."
            ) from exc
        except OSError as exc:
            raise EngineUnavailableError(f"Failed to start Baltamatica CLI: {exc}") from exc

        return CliProcessResult(
            returncode=process.returncode,
            stdout=_decode_process_output(process.stdout),
            stderr=_decode_process_output(process.stderr),
        )


def _combine_output(stdout: str, stderr: str) -> str:
    output = stdout.strip()
    error_output = stderr.strip()
    if output and error_output:
        return f"{output}\n{error_output}"
    return output or error_output


def _decode_process_output(output: bytes | None) -> str:
    if not output:
        return ""
    for encoding in ("utf-8", locale.getpreferredencoding(False), "gbk"):
        try:
            return output.decode(encoding)
        except UnicodeDecodeError:
            continue
    return output.decode("utf-8", errors="replace")


def _process_error(process: CliProcessResult) -> str:
    message = process.stderr.strip() or process.stdout.strip()
    if message:
        return f"Baltamatica CLI exited with code {process.returncode}: {message}"
    return f"Baltamatica CLI exited with code {process.returncode}."


def detect_baltamatica_error(output: str) -> str | None:
    """Detect Baltamatica error text when the CLI exits with status 0."""

    plain_output = _strip_ansi(output)
    for pattern in ERROR_OUTPUT_PATTERNS:
        if pattern in plain_output:
            first_line = next(
                (line.strip() for line in plain_output.splitlines() if line.strip()),
                plain_output.strip(),
            )
            return f"Baltamatica reported an error: {first_line}"
    return None


def _strip_ansi(value: str) -> str:
    return re.sub(r"\x1b\[[0-9;]*m", "", value)


def _escape_baltamatica_string(value: str) -> str:
    return value.replace("'", "''")


def _default_state_file() -> Path:
    return Path(
        tempfile.NamedTemporaryFile(prefix="baltamatica_mcp_", suffix=".mat", delete=True).name
    )


def parse_whos_output(output: str) -> list[VariableInfo]:
    variables: list[VariableInfo] = []
    for line in output.splitlines():
        match = WHOS_ROW_PATTERN.match(line)
        if not match:
            continue
        variables.append(
            VariableInfo(
                name=match.group("name"),
                size=match.group("size"),
                bytes=int(match.group("bytes")),
                class_name=match.group("class_name"),
                attributes=(match.group("attributes") or "").strip(),
            )
        )
    return variables


def parse_artifacts(output: str) -> list[Artifact]:
    artifacts: list[Artifact] = []
    for line in output.splitlines():
        stripped = line.strip()
        if not stripped.startswith(ARTIFACT_PREFIX):
            continue
        spec = stripped.removeprefix(ARTIFACT_PREFIX).strip()
        if not spec:
            continue
        artifact_type, artifact_path = _parse_artifact_spec(spec)
        path = Path(artifact_path).expanduser().resolve()
        artifacts.append(
            Artifact(
                path=str(path),
                type=artifact_type,
                exists=path.exists() and path.is_file(),
                size=path.stat().st_size if path.exists() and path.is_file() else 0,
            )
        )
    return artifacts


def _parse_artifact_spec(spec: str) -> tuple[str, str]:
    if ":" not in spec:
        return _guess_artifact_type(spec), spec

    maybe_type, maybe_path = spec.split(":", 1)
    if "/" not in maybe_type:
        return _guess_artifact_type(spec), spec
    return maybe_type.strip(), maybe_path.strip()


def _guess_artifact_type(path: str) -> str:
    suffix = Path(path).suffix.lower()
    if suffix == ".png":
        return "image/png"
    if suffix in {".jpg", ".jpeg"}:
        return "image/jpeg"
    if suffix == ".svg":
        return "image/svg+xml"
    if suffix == ".pdf":
        return "application/pdf"
    if suffix == ".csv":
        return "text/csv"
    return "application/octet-stream"
