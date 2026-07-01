# PR Implementation Plan

This document tracks the incremental PR stack for building `baltamatica.mcp`.

## Completed PRs

### PR1: MCP Server Skeleton

Branch: `codex/mcp-server-skeleton`

Commit: `45a8155 feat(server): add MCP server skeleton`

Goal: turn the repository from placeholder files into a runnable MCP server surface.

Implemented:

- Added `FastMCP` server construction in `server.py`.
- Registered `execute_code` and `run_script`.
- Added backend selection flags: `--backend` and `--transport`.
- Added `ExecutionResult`, `BaltamaticaEngine`, and placeholder backend interfaces.
- Added server-level tests for tool registration, input validation, and argument parsing.

Verification:

- `PYTHONPATH=src pytest -q`
- `PYTHONPATH=src python3 -m compileall -q src tests`

### PR2: CLI Backend

Branch: `codex/cli-backend`

Commit: `5ad9151 feat(cli): add Baltamatica CLI backend`

Goal: make the MCP tools call a real Baltamatica command-line runtime.

Implemented:

- Added `backend_cli.py`.
- Implemented `CliEngine.execute_code()` via `baltamatica -nodesktop -s <code>`.
- Implemented `CliEngine.run_script()` by wrapping script paths with `run('...')`.
- Added `BALTAMATICA_CLI`, `--cli-executable`, and `--timeout` support.
- Added CLI tests with fake executables for success, failures, path lookup, env lookup, and timeout.

Verification:

- `PYTHONPATH=src pytest -q`
- `PYTHONPATH=src python3 -m compileall -q src tests`

### PR3: Workspace Variable Tools

Branch: `codex/variable-tools`

Commit: `ed81eb4 feat(cli): add workspace variable tools`

Goal: make CLI mode feel interactive by preserving workspace state across MCP calls.

Implemented:

- Added `.mat` state-file persistence for the CLI backend.
- Added MCP tools:
  - `clear_workspace`
  - `list_variables`
  - `get_variable`
- Added `VariableInfo` and `VariableListResult`.
- Implemented `whos` parsing for `list_variables`.
- Implemented `disp(<name>)` for `get_variable`.
- Added variable-name validation to avoid command injection.
- Added `--state-file` for deterministic state reuse.

Verification:

- `PYTHONPATH=src pytest -q`
- Real MCP smoke test:
  - execute `A=[1 2;3 4]; b=42`
  - list variables
  - get `A`
  - clear workspace

### PR4: Integration Tests, CI, and Demo

Branch: `codex/integration-ci`

Commits:

- `bff5969 test(cli): add integration tests and CI`
- `35691d8 test(cli): add numerical pipeline demo`

Goal: make the CLI implementation verifiable in both ordinary CI and real local Baltamatica environments.

Implemented:

- Added GitHub Actions CI for Python 3.10, 3.11, and 3.12.
- CI runs unit tests, `compileall`, and `ruff`.
- Added optional integration tests marked with `integration`.
- Integration tests run only when `BALTAMATICA_CLI` points to a real executable.
- Documented macOS executable path:
  `/Applications/Baltamatica.app/Contents/MacOS/baltamatica`
- Documented Codex MCP setup.
- Added `examples/numerical_pipeline_demo.m`:
  - SPD linear solve
  - power iteration
  - Monte Carlo estimate
  - report matrix generation
- Stabilized fake CLI tests by using Python fake executables.

Verification:

- `python3 -m ruff check src tests`
- `BALTAMATICA_CLI=/Applications/Baltamatica.app/Contents/MacOS/baltamatica PYTHONPATH=src pytest -q`
- `PYTHONPATH=src python3 -m compileall -q src tests`

### PR5: Artifact and Image Feedback

Branch: `codex/artifact-feedback`

Goal: allow scripts that generate plots or files to report artifacts back to the MCP client.

Implemented:

- Added an `Artifact` dataclass:
  - `path`
  - `type`
  - `exists`
  - `size`
- Extended `ExecutionResult` with `artifacts`.
- Parsed stdout lines such as:
  - `BALTAMATICA_ARTIFACT=/tmp/plot.png`
  - `BALTAMATICA_ARTIFACT=image/png:/tmp/plot.png`
- Added MIME type inference from common file extensions.
- Added `examples/artifact_export_demo.m`.
- Added unit and real integration tests for artifact parsing and reporting.

Verification:

- `python3 -m ruff check src tests`
- `BALTAMATICA_CLI=/Applications/Baltamatica.app/Contents/MacOS/baltamatica PYTHONPATH=src pytest -q`
- `PYTHONPATH=src python3 -m compileall -q src tests`

Out of scope:

- Binary payload transfer through MCP.
- Automatic figure capture without an explicit saved file.

### PR6: BEX Protocol Design

Branch: `codex/bex-protocol-design`

Goal: define the Python-to-BEX protocol before implementing the C plugin.

Implemented:

- Add `docs/bex-protocol.md`.
- Define request/response JSON schema for:
  - `execute_code`
  - `run_script`
  - `clear_workspace`
  - `list_variables`
  - `get_variable`
  - artifact reporting
- Add `backend_bex.py` client skeleton:
  - socket connection
  - request IDs
  - timeout handling
  - reconnect behavior
- Add tests with a mock BEX TCP server.

Verification:

- `PYTHONPATH=src pytest -q -m "not integration"`
- `PYTHONPATH=src python -m compileall -q src tests`

Out of scope:

- Real C plugin implementation.
- Binary matrix transfer.

### PR7: Minimal BEX Plugin

Branch: `codex/minimal-bex-plugin`

Goal: provide the first real BEX backend path.

Implemented:

- Add BEX C plugin files:
  - `mcp_bridge.c`
  - protocol header
  - build configuration
- Start a blocking loopback TCP socket listener inside a Baltamatica BEX function.
- Implement `execute_code` via `bxEvalString`.
- Implement minimal `run_script` and `clear_workspace` via `bxEvalString`.
- Return structured success/error responses to Python.
- Add local/manual integration instructions.

Verification:

- `python -m ruff check src tests`
- `python -m pytest -q`
- `python -m compileall -q src tests`

Out of scope:

- High-performance binary variable transfer.
- Full workspace serialization.
- Background listener thread lifecycle management.
- Capturing console output from `bxEvalString`.

### PR8: BEX Variable Access and Serialization

Goal: make BEX mode feature-complete for variables.

Branch: `codex/bex-structured-variables`

Implemented:

- Implemented BEX `list_variables` using SDK variable-name enumeration and
  metadata lookup.
- Implemented BEX `get_variable` using SDK workspace lookup.
- Preserved text output compatibility with CLI mode.
- Added structured JSON `value` payloads for small real numeric and logical
  arrays.
- Added element and output limits so large values are bounded and marked as
  truncated.
- Updated BEX protocol documentation, plugin documentation, and source tests.

Verification:

- `PYTHONPATH=src pytest -q tests/test_backend_bex.py tests/test_bex_sources.py tests/test_server.py`
- `PYTHONPATH=src pytest -q -m "not integration"`
- Manual Baltamatica GUI bridge smoke tests for numeric matrix, logical matrix,
  large vector truncation, and unsupported char fallback.

Out of scope:

- High-performance binary variable transfer.
- Complex type serialization beyond text fallback.
- Plugin-side image/file artifact discovery.

## Planned PRs

### PR9: Packaging and Release Readiness

Goal: prepare the project for external users.

Proposed implementation:

- Add release checklist.
- Add platform-specific installation notes.
- Add troubleshooting guide for CLI and BEX.
- Add PyPI metadata polish.
- Add example MCP configs for Codex, Claude Desktop, and Claude Code.
- Prepare GitHub Releases layout for future BEX binaries.
