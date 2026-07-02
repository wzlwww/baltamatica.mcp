# BEX Protocol Design

This document defines the JSON protocol used between the Python MCP server and
the future Baltamatica BEX plugin. It is intentionally small for PR6: the Python
client can be tested against a mock TCP server, while the real C plugin remains
out of scope until PR7.

## Goals

- Keep the MCP tool contract identical across CLI and BEX backends.
- Use a human-readable protocol that is simple to debug from either Python or C.
- Preserve workspace state in the Baltamatica process that hosts the BEX plugin.
- Return small real numeric/logical arrays as structured JSON while leaving
  binary matrix transfer and high-performance serialization for later PRs.

## Transport

- TCP socket, bound to loopback by default.
- Default host: `127.0.0.1`.
- Default port: `31415`.
- Encoding: UTF-8.
- Framing: newline-delimited JSON. Each request and response is one JSON object
  followed by `\n`.
- Request handling: clients send one request at a time on a connection. The
  server may support pipelining later, but PR6 does not require it.
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
`error` string. The structured form above is preferred for the real plugin.

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

### `shutdown`

Development helper implemented by the PR7 C bridge. It is not exposed as an MCP
tool. It stops the blocking BEX listener cleanly.

Request:

```json
{"id":"stop","method":"shutdown","params":{}}
```

Response:

```json
{"id":"stop","success":true,"output":"shutting down","artifacts":[]}
```

### `list_variables`

Return metadata for variables currently present in the workspace.

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

Return one variable. The result stays aligned with CLI mode by returning text in
`output`. For numeric and logical arrays (real **and** complex, of any size) the
BEX response also carries a full-fidelity binary `value`: the raw column-major
bytes are streamed as base64 `data_b64` with a numpy-style `dtype`, so there is
no truncation. Unsupported values (char/string/struct/cell/…) still return
`output` text and set `value.supported` to `false`.

The Python client (`serializer.py`) decodes `data_b64` and presents it to the
model: small arrays are inlined as nested lists; large arrays get a summary,
preview, and a lossless `.npy` file artifact.

Request:

```json
{"id":"6","method":"get_variable","params":{"name":"A"}}
```

Wire response (bridge → Python):

```json
{
  "id": "6",
  "success": true,
  "output": "1 2\n3 4",
  "value": {
    "supported": true,
    "type": "ndarray",
    "class_name": "double",
    "dtype": "float64",
    "complexity": "real",
    "size": "2x2",
    "dims": [2, 2],
    "byte_order": "little",
    "encoding": "base64",
    "element_count": 4,
    "data_b64": "AAAAAAAA8D8AAAAAAAAIQAAAAAAAAABAAAAAAAAAEEA="
  },
  "artifacts": []
}
```

`dtype` follows numpy naming: `float64`/`float32`, `int8`…`int64`,
`uint8`…`uint64`, `bool`, and `complex128`/`complex64` (interleaved re,im, which
matches numpy's complex memory layout). `data_b64` holds the exact column-major
bytes in little-endian order.

Presented value (Python → model) for a small array:

```json
{
  "class_name": "double",
  "dtype": "float64",
  "complexity": "real",
  "shape": [2, 2],
  "size": "2x2",
  "element_count": 4,
  "truncated": false,
  "data": [[1, 2], [3, 4]]
}
```

### `set_variable`

Create or overwrite a variable in the base workspace from a base64 column-major
payload. Supports `float64` (double) and `bool` (logical) real matrices. The
payload is bounded by the request line size, so this is for modest arrays.

Request:

```json
{
  "id": "7",
  "method": "set_variable",
  "params": {
    "name": "A",
    "dtype": "float64",
    "dims": [2, 2],
    "data_b64": "AAAAAAAA8D8AAAAAAAAIQAAAAAAAAABAAAAAAAAAEEA="
  }
}
```

`data_b64` holds the exact little-endian column-major bytes for `dims` = `[rows,
cols]`. The Python client builds this from a scalar, 1-D list (row vector), or
2-D nested list. Response is a normal success/error result with empty `output`.

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

The real BEX plugin should use stable string codes:

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

- `execute_code`: `bxEvalString` for command execution, or `bxEvalIn` when a
  result object must be captured. `bxEvalString` returns `0` on success and `1`
  on failure, but cannot return expression values.
- `run_script`: evaluate a `run(...)` command or equivalent script execution.
- `clear_workspace`: evaluate `clear` in the target workspace.
- `list_variables`: `bxGetVariableNames`, followed by metadata lookup, then
  `bxFreeVariableNames` to release the returned variable-name array.
- `get_variable`: evaluate or lookup the variable and format the resulting
  `bxArray` with `bxArrayToCStr`.

SDK notes that matter for PR7:

- The interpreter must be running before command and workspace APIs are used.
- `bxEvalIn` accepts `"base"` and `"caller"` workspace names.
- Current workspace APIs operate on the current workspace where the BEX/plugin
  code is executing. A bridge that wants top-level interactive semantics should
  explicitly execute commands in the base workspace where possible.
- The SDK manual warns callers to validate command strings themselves; the BEX
  plugin should treat protocol input as untrusted.
- `bxArrayToCStr` can be slow for large arrays. The bridge caps text and
  structured JSON payload sizes; binary transfer remains a later PR.

The exact C implementation, SDK include paths, and build settings belong to PR7.
