"""Engine abstraction for communicating with Baltamatica backends."""

from __future__ import annotations

from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Literal, Protocol


BackendName = Literal["auto", "cli", "bex"]


@dataclass(frozen=True)
class Artifact:
    """File artifact produced by a Baltamatica command."""

    path: str
    type: str
    exists: bool
    size: int

    def to_dict(self) -> dict[str, object]:
        return asdict(self)


@dataclass(frozen=True)
class ExecutionResult:
    """Normalized result returned by all engine backends."""

    success: bool
    output: str = ""
    error: str | None = None
    artifacts: list[Artifact] | None = None

    def to_dict(self) -> dict[str, object]:
        return {
            "success": self.success,
            "output": self.output,
            "error": self.error,
            "artifacts": [artifact.to_dict() for artifact in self.artifacts or []],
        }


@dataclass(frozen=True)
class VariableInfo:
    """Metadata for one variable in the Baltamatica workspace."""

    name: str
    size: str
    bytes: int
    class_name: str
    attributes: str = ""

    def to_dict(self) -> dict[str, object]:
        return asdict(self)


@dataclass(frozen=True)
class VariableListResult:
    """Normalized result for workspace variable listing."""

    success: bool
    variables: list[VariableInfo]
    output: str = ""
    error: str | None = None

    def to_dict(self) -> dict[str, object]:
        return {
            "success": self.success,
            "variables": [variable.to_dict() for variable in self.variables],
            "output": self.output,
            "error": self.error,
        }


class EngineError(RuntimeError):
    """Base error raised by Baltamatica engines."""


class EngineUnavailableError(EngineError):
    """Raised when the selected backend has not been configured or implemented."""


class BaltamaticaEngine(Protocol):
    """Common interface used by the MCP server tools."""

    backend: BackendName

    async def execute_code(self, code: str) -> ExecutionResult:
        """Execute one Baltamatica code snippet."""

    async def run_script(self, file_path: str) -> ExecutionResult:
        """Run a Baltamatica `.m` script file."""

    async def clear_workspace(self) -> ExecutionResult:
        """Clear the persisted workspace state."""

    async def list_variables(self) -> VariableListResult:
        """List variables in the persisted workspace state."""

    async def get_variable(self, name: str) -> ExecutionResult:
        """Get a display representation of one variable."""


class UnimplementedEngine:
    """Placeholder backend used until CLI and BEX engines land in later PRs."""

    def __init__(self, backend: BackendName = "auto") -> None:
        self.backend = backend

    async def execute_code(self, code: str) -> ExecutionResult:
        raise EngineUnavailableError(
            f"Baltamatica backend '{self.backend}' is not implemented yet. "
            "The MCP server skeleton is ready; install a backend implementation next."
        )

    async def run_script(self, file_path: str) -> ExecutionResult:
        raise EngineUnavailableError(
            f"Baltamatica backend '{self.backend}' is not implemented yet. "
            "The MCP server skeleton is ready; install a backend implementation next."
        )

    async def clear_workspace(self) -> ExecutionResult:
        raise EngineUnavailableError(
            f"Baltamatica backend '{self.backend}' is not implemented yet. "
            "The MCP server skeleton is ready; install a backend implementation next."
        )

    async def list_variables(self) -> VariableListResult:
        raise EngineUnavailableError(
            f"Baltamatica backend '{self.backend}' is not implemented yet. "
            "The MCP server skeleton is ready; install a backend implementation next."
        )

    async def get_variable(self, name: str) -> ExecutionResult:
        raise EngineUnavailableError(
            f"Baltamatica backend '{self.backend}' is not implemented yet. "
            "The MCP server skeleton is ready; install a backend implementation next."
        )


def create_engine(
    backend: BackendName = "auto",
    *,
    cli_executable: str | None = None,
    bex_host: str = "127.0.0.1",
    bex_port: int = 31415,
    timeout: float = 30.0,
    state_file: str | Path | None = None,
) -> BaltamaticaEngine:
    """Create the engine selected by CLI configuration.

    CLI mode runs Baltamatica through the command-line runtime. BEX mode connects
    to the JSON-over-TCP bridge implemented by the Baltamatica plugin.
    """

    if backend not in {"auto", "cli", "bex"}:
        raise ValueError(f"Unsupported backend: {backend}")
    if backend in {"auto", "cli"}:
        from baltamatica_mcp.backend_cli import CliEngine

        return CliEngine(executable=cli_executable, timeout=timeout, state_file=state_file)

    from baltamatica_mcp.backend_bex import BexEngine

    return BexEngine(host=bex_host, port=bex_port, timeout=timeout)
