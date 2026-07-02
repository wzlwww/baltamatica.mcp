# BEX Bridge Usage and Troubleshooting

This guide covers the experimental BEX bridge used by the `--backend bex` mode.

## Build

From the repository root:

```bash
/Applications/Baltamatica.app/Contents/MacOS/bex bex/mcp_bridge.c
```

The compiler writes `mcp_bridge.bexmaci64` to the current directory.

## Start in the Interactive GUI

Open Baltamatica Desktop and run this in the command window:

```matlab
addpath('/path/to/baltamatica.mcp'); mcp_bridge()
```

The default bridge address is `127.0.0.1:31415`. To use a different port:

```matlab
addpath('/path/to/baltamatica.mcp'); mcp_bridge(31416)
```

While the bridge is running, the GUI command window is occupied by the BEX
function. This is expected. MCP requests are sent from another process over TCP.

Experimental background mode starts the listener on a worker thread and returns
the GUI command prompt immediately:

```matlab
addpath('/path/to/baltamatica.mcp'); mcp_bridge('background')
addpath('/path/to/baltamatica.mcp'); mcp_bridge('background', 31416)
```

This mode is intended for lifecycle testing. It makes `mcp_bridge('stop')`
callable from the same GUI session, but request handling also runs on the worker
thread and must be verified carefully for workspace, `eval`, and plotting
behavior.

Start the MCP server with the matching port:

```bash
PYTHONPATH=src python -m baltamatica_mcp --backend bex --bex-host 127.0.0.1 --bex-port 31415
```

## Stop

Once the GUI command prompt is available, stop a running bridge with:

```matlab
mcp_bridge('stop')
mcp_bridge('stop', 31416)
```

The bridge tracks its listening socket in process-global state, so `stop`
resolves the situation directly instead of relying on a live accept loop:

- **After Ctrl+C on a foreground `mcp_bridge()`** the accept loop is gone but the
  socket is still open. `stop` closes that socket directly and releases the port.
- **For a `mcp_bridge('background')` listener** `stop` wakes the worker's accept
  loop over the wire so it exits and cleans up.
- **For a bridge in another process/session** `stop` falls back to sending the
  shutdown request over TCP.

`stop` cannot run from the same command window while a foreground `mcp_bridge()`
is still blocking that prompt — press Ctrl+C first, or start the listener with
`mcp_bridge('background')` so the prompt stays free. Each command optionally
returns a numeric status (`0` = ok) as its single output, e.g.
`s = mcp_bridge('stop')`.

The bridge accepts a debug lifecycle request:

```json
{"id":"shutdown","method":"shutdown","params":{}}
```

The repository includes a small helper that sends this request and waits until
the listener port is released:

```bash
PYTHONPATH=src python -m baltamatica_mcp.bex_shutdown
```

Use `--host`, `--port`, `--attempts`, and `--timeout` when the bridge is running
on a non-default address. The GUI command window may not stop reliably with
Ctrl+C because the BEX function is blocked inside the TCP accept loop.

## Status Check

The bridge accepts a lightweight status request:

```json
{"id":"status","method":"status","params":{}}
```

Example response:

```json
{"id":"status","success":true,"output":"MCP bridge ready","host":"127.0.0.1","port":31415,"artifacts":[]}
```

## Threading and Concurrency

How SDK/interpreter calls are dispatched depends on the run mode:

- **Foreground `mcp_bridge()`** runs the accept loop *on the interpreter
  thread*. Every `bxCallBaltamatica` / `bxEvalIn` happens on that thread, so
  there is no cross-thread interpreter access — the safest model — but the GUI
  command line is blocked while it runs.
- **Background `mcp_bridge('background')`** runs the accept loop on a detached
  worker thread. The command line stays free, but interpreter calls happen on
  the worker thread.

In both modes the bridge **serializes all requests**: it accepts one client
connection at a time and processes that client's requests one by one, so no two
bridge requests ever touch the interpreter concurrently. Empirically this is
stable — a soak of 800 sequential mixed requests and two concurrent client
connections (execute_code / set_variable / get_variable / list_variables)
completed with zero errors and intact workspace state.

The one remaining concurrency hazard is **background mode plus manual GUI use**:
if you type in the GUI command window while the MCP server is serving requests,
the interpreter is driven from two threads at once (the GUI main thread and the
bridge worker), and Baltamatica's interpreter is not known to be thread-safe.

Recommendations:

- While the AI is driving a background bridge, don't also type commands in the
  GUI command window.
- If you need to work in the GUI concurrently, or want the strongest safety
  guarantee, use foreground `mcp_bridge()` (interpreter calls on the main
  thread) and stop it with `python -m baltamatica_mcp.bex_shutdown`.
- Multiple MCP clients are safe: the bridge queues and serializes them.

## Plotting

For GUI plotting, start `mcp_bridge()` inside an already open interactive
Baltamatica GUI command window. This path has been verified with:

```matlab
x=0:0.1:6.28; y=sin(x); plot(x,y);
```

Launching Baltamatica with `-desktop -s "mcp_bridge()"` can start the TCP bridge,
but it does not provide the same plotting context. In that mode `plot(...)` may
still fail with "undefined variable or function".

## Troubleshooting

### Port already in use

Check the listener:

```bash
lsof -nP -iTCP:31415 -sTCP:LISTEN
```

Either stop the existing bridge or start a new one on a different port:

```matlab
mcp_bridge(31416)
```

Older bridge builds could leave the port held by a Baltamatica child process
after the bridge loop stopped. Rebuild the BEX file and restart Baltamatica so
new listener sockets are marked close-on-exec.

After Ctrl+C on a foreground `mcp_bridge()`, the listening socket is tracked in
process-global state, so `mcp_bridge('stop')` closes it directly and a fresh
`mcp_bridge()` reclaims the leaked socket instead of failing to bind. Restarting
Baltamatica should only be necessary if the BEX file was unloaded (for example
with `clear mcp_bridge`) while the socket was still leaked, since that discards
the tracked handle.

### `plot` is undefined

Make sure the bridge was started from the interactive GUI command window, not
from `-nodesktop` or `-desktop -s`.

### Command window is blocked

That is normal while the bridge is serving TCP requests. Send the `shutdown`
request to release it.

### Compiled BEX file location

The BEX compiler writes `mcp_bridge.bexmaci64` in the current working directory
unless an output option is provided. The generated binary is ignored by git.

### BEX variable value output is truncated

`get_variable` always returns text generated by `bxArrayToCStr` in `output`.
For small real numeric and logical arrays it also returns a structured `value`
object with dimensions and column-major `data`. Large structured arrays are
truncated to keep JSON responses bounded. High-volume binary matrix transfer is
still planned for a later PR.
