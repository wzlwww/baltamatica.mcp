# BEX Plot Window Probe

PR6 verifies whether a BEX function can open a Baltamatica plot window from the interpreter that loads the BEX file.

## Why this probe exists

The SDK exposes evaluation APIs such as `bxEvalString`, `bxEvalIn`, and `bxCallBaltamatica`. For PR6 the goal is only to prove that a GUI-loaded BEX can reach the plotting stack and pop a figure window. Image export, screenshot capture, and MCP artifact return are intentionally out of scope.

## Build

From the repository root:

```bash
/Applications/Baltamatica.app/Contents/MacOS/bex bex/bex_plot_probe.c
```

The macOS compiler writes `bex_plot_probe.bexmaci64` in the current directory unless `-output` is provided.

## CLI Boundary Test

The CLI boundary test checks that the BEX file can be loaded and that the non-plot BEX SDK calls work:

```bash
/Applications/Baltamatica.app/Contents/MacOS/baltamatica -nodesktop -s \
  "addpath('/path/to/baltamatica.mcp'); bex_plot_probe"
```

The headless CLI runtime is expected to report `plot=1` because it does not expose the plotting stack. That is a known boundary, not a PR6 failure.

## GUI verification

Open Baltamatica Desktop, add the repository root to the path, and run:

```matlab
examples/bex_plot_probe_demo
```

Expected result:

- `plot=0` in `/tmp/baltamatica_mcp_bex_plot_probe.txt`
- A figure window titled `BEX plot probe`

If GUI `plot` is `0`, BEX can reach the plotting stack and pop a plot window.

Out of scope for PR6:

- Saving the figure to disk.
- Returning images through Codex or MCP.
- Discovering a stable `saveas`, `print`, `exportgraphics`, `getframe`, or equivalent export path.
