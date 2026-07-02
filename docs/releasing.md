# Releasing baltamatica-mcp

The Python package ships to PyPI; the compiled BEX bridge binaries ship as
GitHub Release assets (they cannot be built in ordinary CI because they require
a local Baltamatica install and its bundled `bex` compiler).

## 1. Pre-release checks

```bash
PYTHONPATH=src python3 -m pytest -q -m "not integration"
python3 -m ruff check src tests
PYTHONPATH=src python3 -m compileall -q src tests
```

If a Baltamatica install is available, also run the integration tests:

```bash
BALTAMATICA_CLI=/Applications/Baltamatica.app/Contents/MacOS/baltamatica \
  PYTHONPATH=src python3 -m pytest -q -m integration
```

## 2. Bump the version

Update `version` in `pyproject.toml` (this project follows a loose SemVer), and
note the changes in the README roadmap.

## 3. Build and publish the Python package

```bash
python3 -m build            # produces dist/*.tar.gz and dist/*.whl
python3 -m twine check dist/*
python3 -m twine upload dist/*   # needs a PyPI token
```

The wheel is pure Python (no compiled extension) and installs a `baltamatica-mcp`
console entry point. It does **not** bundle the BEX binaries.

## 4. Build the BEX bridge binaries (per platform)

On each target OS, with Baltamatica installed, run the helper (or the manual
command it wraps):

```bash
scripts/build_bex.sh
```

The script first tries the standalone `bex` compiler, then falls back to the
in-interpreter `bex()` function:

```bash
baltamatica -nodesktop -s "cd('<repo>/bex'); bex('mcp_bridge.c')"
```

The fallback is required on some Linux builds (e.g. Ubuntu 24.04) where the
standalone `bex` binary crashes at startup (`std::system_error` during glibc
`rseq` init) even though the interpreter itself runs fine headless. Run the
interpreter headless with `QT_QPA_PLATFORM=offscreen` and Baltamatica's `lib`
directory on `LD_LIBRARY_PATH` (the `baltamatica.sh` launcher sets this up).

This produces the platform artifact:

- macOS: `mcp_bridge.bexmaci64`
- Linux: `mcp_bridge.bexa64`
- Windows: `mcp_bridge.bexw64`

Build once per OS/architecture you want to support. (On Windows, use the bundled
`bex` in the install `lib` directory, or the CMake path in `bex/CMakeLists.txt`.)

## 5. Cut the GitHub Release

Tag `vX.Y.Z`, create a GitHub Release, and attach the three `mcp_bridge.bex*`
binaries so users can download a prebuilt bridge instead of compiling it. Include
the API version they were built against (currently BEX SDK v3.9 / Baltamatica
2025).
