"""Engine abstraction for communicating with Baltamatica backends."""

from __future__ import annotations

from dataclasses import asdict, dataclass
from typing import Literal, Protocol


BackendName = Literal["auto", "cli", "bex"]


@dataclass(frozen=True)
class ExecutionResult:
    """Normalized result returned by all engine backends."""

    success: bool
    output: str = ""
    error: str | None = None

    def to_dict(self) -> dict[str, object]:
        return asdict(self)


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


def create_engine(backend: BackendName = "auto") -> BaltamaticaEngine:
    """Create the engine selected by CLI configuration.

    Phase 1 wires the MCP surface and keeps the backend boundary explicit.
    Concrete CLI and BEX implementations will replace this placeholder in
    follow-up PRs without changing the MCP tool contract.
    """

    if backend not in {"auto", "cli", "bex"}:
        raise ValueError(f"Unsupported backend: {backend}")
    return UnimplementedEngine(backend=backend)
