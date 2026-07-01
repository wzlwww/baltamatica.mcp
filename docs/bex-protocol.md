# BEX Protocol Design

This document defines the JSON protocol used between the Python MCP server and
the Baltamatica BEX plugin. PR6 introduced the protocol and Python client; PR7
adds the first minimal C bridge in `bex/mcp_bridge.c`.

## Goals

- Keep the MCP tool contract identical across CLI and BEX backends.
- Use a human-readable protocol that is simple to debug from either Python or C.
- Preserve workspace state in the Baltamatica process that hosts the BEX plugin.
- Leave binary matrix transfer and high-performance serialization for later PRs.

## Transport

- TCP socket, bound to loopback by default.
- Default host: `127.0.0.1`.
- Default port: `31415`. The BEX function can be started on another port with
  `mcp_bridge(31416)`.
- Encoding: UTF-8.
- Framing: newline-delimited JSON. Each request and response is one JSON object
  followed by `\n`.
- Request handling: clients send one request at a time on a connection. The
  current C bridge runs its server loop on the BEX invocation thread, handles
  clients sequentially, and does not support pipelining.
- Timeout: the Python client applies one timeout to connect, write, and read the
  response.

## Envelope

Every request has an `id`, `method`, and `params` object.

```json
{"id":"1","method":"execute_code","params":{"code":"A = [1 2; 3 4]"}}
```

Every response echoes the same `id` and includes `success`.

```json
{"id":"1","success":true,"output":"","artifacts":[]}
```

Failed responses use the same envelope and include `error`.

```json
{
  "id": "1",
  "success": false,
  "output": "",
  "error": {"code": "EVAL_ERROR", "message": "Undefined function or variable x"}
}
```

For compatibility with simple test servers, the Python client also accepts an
`error` string. The structured form above is preferred for plugin responses.

## Methods

### `execute_code`

Execute code in the persistent Baltamatica workspace.

Request:

```json
{"id":"2","method":"execute_code","params":{"code":"x = 42; disp(x)"}}
```

Response:

```json
{"id":"2","success":true,"output":"42","artifacts":[]}
```

### `run_script`

Run a local `.m` script path from the BEX plugin process. The path must be an
absolute path supplied by the Python MCP server after it has validated that the
file exists and uses the `.m` extension.

Request:

```json
{"id":"3","method":"run_script","params":{"file_path":"/tmp/demo.m"}}
```

Response:

```json
{"id":"3","success":true,"output":"done","artifacts":[]}
```

### `clear_workspace`

Clear the persistent Baltamatica workspace.

Request:

```json
{"id":"4","method":"clear_workspace","params":{}}
```

Response:

```json
{"id":"4","success":true,"output":"","artifacts":[]}
```

### `list_variables`

Return metadata for variables currently present in the workspace.

Status: implemented in the BEX bridge for base-workspace variables. `bytes` is
estimated for common numeric and logical arrays.

Request:

```json
{"id":"5","method":"list_variables","params":{}}
```

Response:

```json
{
  "id": "5",
  "success": true,
  "output": "",
  "variables": [
    {"name":"A","size":"2x2","bytes":32,"class_name":"double","attributes":""}
  ]
}
```

### `get_variable`

Return a display representation of one variable. The result stays aligned with
CLI mode by returning text in `output`; structured JSON values are planned for a
later BEX serialization PR.

Status: implemented with `bxEvalIn("base", name, &value)` and
`bxArrayToCStr(...)`. Variable names are validated before evaluation.

Request:

```json
{"id":"6","method":"get_variable","params":{"name":"A"}}
```

Response:

```json
{"id":"6","success":true,"output":"1 2\n3 4","artifacts":[]}
```

### `status`

Return bridge lifecycle information. This is intended for diagnostics and
integration tests.

Request:

```json
{"id":"7","method":"status","params":{}}
```

Response:

```json
{"id":"7","success":true,"output":"MCP bridge ready","host":"127.0.0.1","port":31415,"artifacts":[]}
```

### `shutdown`

Ask the bridge server loop to exit and release the GUI command window.

Request:

```json
{"id":"8","method":"shutdown","params":{}}
```

## Artifact Schema

Commands can report generated files with the same normalized shape used by the
CLI backend.

```json
{
  "path": "/tmp/plot.png",
  "type": "image/png",
  "exists": true,
  "size": 12345
}
```

`path` should be absolute from the machine running the MCP server. `type` is a
MIME type. `exists` and `size` are produced by the BEX plugin when possible; the
Python client treats missing values as `false` and `0`.

## Error Codes

The BEX plugin should use stable string codes:

| Code | Meaning |
| --- | --- |
| `BAD_REQUEST` | Malformed JSON, missing fields, or unsupported method. |
| `EVAL_ERROR` | Baltamatica rejected or failed while executing code. |
| `SCRIPT_ERROR` | Script path cannot be run or script execution failed. |
| `VARIABLE_ERROR` | Variable listing or lookup failed. |
| `TIMEOUT` | Backend operation timed out inside the plugin. |
| `INTERNAL_ERROR` | Unexpected plugin failure. |

## Reconnect Behavior

The Python client keeps a persistent TCP connection while it is healthy. If the
connection is closed, the next request reconnects automatically. If a request is
sent but no response is received because the connection drops, the client may
open a fresh connection and retry once. Callers should still treat ambiguous
transport failures as unknown outcome failures, especially for non-idempotent
`execute_code` calls.

## BEX SDK Mapping

The protocol is designed around Baltamatica SDK manual API v3.9:

- `execute_code`: the PR7 bridge calls Baltamatica's `eval` function through
  `bxCallBaltamatica(0, NULL, 1, args, "eval")`, which supports statements such
  as assignments, `disp(...)`, and `clear`.
- `bxEvalIn` is useful for expression probes that capture a result object, but
  requesting an output object is not suitable for statements such as assignment
  or `disp(...)`.
- `run_script`: evaluate a `run(...)` command or equivalent script execution.
- `clear_workspace`: evaluate `clear` in the target workspace.
- `list_variables`: `bxGetVariableNames`, followed by `bxEvalIn("base", name,
  &value)` for metadata lookup, then `bxFreeVariableNames` to release the
  returned variable-name array.
- `get_variable`: `bxEvalIn("base", name, &value)` and format the resulting
  `bxArray` with `bxArrayToCStr`.

SDK notes that matter for PR7 and later:

- The interpreter must be running before command and workspace APIs are used.
- `bxEvalIn` accepts `"base"` and `"caller"` workspace names.
- Current workspace APIs operate on the current workspace where the BEX/plugin
  code is executing. A bridge that wants top-level interactive semantics should
  explicitly execute commands in the base workspace where possible.
- The SDK manual warns callers to validate command strings themselves; the BEX
  plugin should treat protocol input as untrusted.
- `bxArrayToCStr` can be slow for large arrays. The bridge uses a fixed output
  buffer and appends a truncation marker when the rendered value is too large.

## Current C Bridge Status

`bex/mcp_bridge.c` is the current experimental BEX bridge implementation:

- Listens on `127.0.0.1:31415`.
- Runs the server loop on the BEX invocation thread so interpreter and plotting
  APIs are not called from a background pthread.
- Implements `execute_code` by calling Baltamatica `eval` through
  `bxCallBaltamatica`.
- Implements `run_script` by evaluating `run('absolute/path.m')`.
- Implements `clear_workspace` by evaluating `clear;`.
- Implements `list_variables` with SDK workspace variable names and metadata.
- Implements `get_variable` with text output from `bxArrayToCStr`.
- Supports optional startup port selection through `mcp_bridge(port)`.
- Supports diagnostic `status` and lifecycle `shutdown` protocol methods.
- Does not capture console output yet; responses currently use an empty
  `output` field.
- Does not save or return plot images. When loaded inside the Baltamatica GUI,
  plotting commands are expected to open native Figure windows.
- Includes a debug-only `shutdown` method for automated smoke tests.

Because the bridge executes commands received over a TCP socket, it binds to
loopback only and should not be exposed to a network.
