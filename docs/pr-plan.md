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

### PR6: BEX Protocol Design and Plot Probe

Branch: `codex/bex-protocol-design`

Goal: define the Python-to-BEX protocol and verify whether a GUI-loaded BEX can reach Baltamatica plotting.

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
- Add `bex/bex_plot_probe.c` to test BEX SDK evaluation and `plot` calls.
- Add `examples/bex_plot_probe_demo.m` for GUI-side verification.
- Document that headless CLI can load BEX and run non-plot SDK calls, while `plot` is expected to fail there because the plotting stack is not available.

Verification:

- `PYTHONPATH=src pytest -q -m "not integration"`
- `PYTHONPATH=src python -m compileall -q src tests`
- `/Applications/Baltamatica.app/Contents/MacOS/bex bex/bex_plot_probe.c`
- `BALTAMATICA_CLI=/Applications/Baltamatica.app/Contents/MacOS/baltamatica PYTHONPATH=src pytest -q`
- GUI-loaded `bex_plot_probe()` returns `plot=0` and opens a native Figure window.

Out of scope:

- Long-running BEX TCP bridge.
- Binary matrix transfer.
- Saving or returning plot images.

### PR7: Minimal BEX Plugin

Goal: provide the first real BEX backend path.

Implemented:

- Replace the BEX placeholder with `bex/mcp_bridge.c`.
- Start a TCP socket listener inside the Baltamatica process that loads the BEX file.
- Run the listener on the BEX invocation thread so interpreter and GUI plotting APIs are not called from a background pthread.
- Bind to `127.0.0.1:31415`.
- Accept newline-delimited JSON requests compatible with `backend_bex.py`.
- Implement:
  - `execute_code` through Baltamatica `eval` via `bxCallBaltamatica`
  - `run_script` through `run('...')`
  - `clear_workspace` through `clear`
- Return structured success/error JSON responses.
- Add integration coverage that compiles `mcp_bridge.c` with the real BEX compiler.
- Add a real TCP smoke test that starts the bridge, executes code through `BexEngine`, verifies a failure response, and shuts the bridge down.
- Add a debug-only `shutdown` method for automated bridge smoke tests.
- Document the GUI bridge startup path in `README.md` and `docs/bex-protocol.md`.

Out of scope:

- High-performance binary variable transfer.
- Capturing console output from evaluated code.
- Saving or returning GUI figure images.

### PR8: BEX Variable Access and Serialization

Goal: make BEX mode feature-complete for variables.

Implemented:

- Implement `list_variables` in `bex/mcp_bridge.c`:
  - `bxGetVariableNames`
  - `bxEvalIn("base", name, &value)` for metadata lookup
  - dimensions through `bxGetDimensions`
  - class names through `bxTypeCStr`
  - estimated bytes for common numeric and logical arrays
- Implement `get_variable` with validated variable names and `bxArrayToCStr`.
- Add fixed output limits with a truncation marker for large rendered values.
- Extend the real BEX TCP integration test to verify:
  - variable assignment through `execute_code`
  - `list_variables` includes `A` and `b`
  - `A` reports `2x2`, `double`, and 32 estimated bytes
  - `get_variable("A")` returns matrix text

Verification:

- `BALTAMATICA_CLI=/Applications/Baltamatica.app/Contents/MacOS/baltamatica PYTHONPATH=src pytest -q tests/test_integration_cli.py::test_real_bex_bridge_executes_code_over_tcp`

Out of scope:

- Binary matrix transfer.
- Full structured JSON values for arrays, structs, cells, tables, and objects.
- Capturing console output from arbitrary evaluated code.

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
