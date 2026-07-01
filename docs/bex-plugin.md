# Minimal BEX Plugin

PR7 adds the first real BEX bridge source for `baltamatica.mcp`. The bridge is a
blocking BEX function that starts a loopback TCP listener inside a Baltamatica
process and speaks the JSON-over-TCP protocol from `docs/bex-protocol.md`.

## Status

Implemented:

- `execute_code`: evaluates the request `code` with `bxEvalString`.
- `run_script`: evaluates `run('<file_path>')`.
- `clear_workspace`: evaluates `clear`.
- `list_variables`: enumerates workspace variables with SDK metadata.
- `get_variable`: returns text output plus structured JSON for small real
  numeric/logical arrays.
- Structured JSON success and error responses.
- A development-only `shutdown` JSON method to stop the blocking listener.

Deferred:

- Binary matrix transfer.
- Complex type serialization beyond text fallback.
- Plugin-side image/file artifact discovery.

## Build With Baltamatica

From the Baltamatica command window:

```matlab
cd('/path/to/baltamatica.mcp/bex')
bex mcp_bridge.c
```

The generated file name is platform-specific:

- Windows: `mcp_bridge.bexw64`
- Linux: `mcp_bridge.bexa64`
- macOS: `mcp_bridge.bexmaci64`

If Baltamatica already loaded an older copy, clear it before rebuilding or
restarting:

```matlab
clear mcp_bridge
```

## Build With CMake

The SDK manual documents a CMake helper named `add_baltamatica_bex`. A typical
build is:

```bash
cmake -S bex -B bex/build -DBaltamatica_DIR=/path/to/baltamatica/cmake
cmake --build bex/build
```

On Windows the target links `ws2_32` for TCP sockets.

## Start The Bridge

Start the listener from Baltamatica:

```matlab
mcp_bridge
```

By default it listens on `127.0.0.1:31415`. You can pass a custom port:

```matlab
mcp_bridge(43141)
```

The PR7 implementation is blocking. Run it in a dedicated Baltamatica session
while the MCP server is connected.

Then start the Python MCP server:

```bash
python -m baltamatica_mcp --backend bex --bex-host 127.0.0.1 --bex-port 31415
```

## Stop The Bridge

For development, send a newline-delimited JSON `shutdown` request:

```json
{"id":"stop","method":"shutdown","params":{}}
```

You can also interrupt or close the dedicated Baltamatica session.

## SDK Notes

The source follows the Baltamatica SDK manual API v3.9:

- BEX entry point:
  `void bexFunction(int nlhs, bxArray *plhs[], int nrhs, const bxArray *prhs[])`
- Command evaluation:
  `int bxEvalString(const char *expr)`
- Error reporting for invalid BEX invocation:
  `bxErrMsgTxt(...)`

`bxEvalString` reports only success or failure, so command responses do not
capture console output yet. Variable reads use SDK workspace APIs and include a
bounded structured `value` payload for supported small arrays.
