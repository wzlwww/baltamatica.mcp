"""Decode BEX binary variable payloads into AI-friendly presentations.

The BEX bridge streams numeric/logical variables as a base64 ``data_b64`` payload
of the raw column-major bytes plus a numpy-style ``dtype`` and ``dims``. This
module decodes that payload with the standard library (no numpy dependency) and
presents it sensibly:

- small arrays are inlined as nested lists of numbers;
- large arrays get a statistical summary, a short preview, and a lossless
  ``.npy`` file artifact (Fortran-order, exact bytes) so the full data stays
  retrievable without flooding the model's context.
"""

from __future__ import annotations

import base64
import math
import os
import struct
import tempfile
from typing import Any

from baltamatica_mcp.engine import Artifact

# Arrays with at most this many elements are inlined as literal numbers.
MAX_INLINE_ELEMENTS = 256
# Number of leading elements shown as a preview for large arrays.
PREVIEW_ELEMENTS = 12
# Above this element count we skip full min/max/mean to bound decode time.
STATS_LIMIT = 5_000_000

# dtype -> (struct code for one scalar component, component size, kind, npy descr)
# kind: "f" float, "i" signed int, "u" unsigned int, "b" bool, "c" complex
_DTYPES: dict[str, tuple[str, int, str, str]] = {
    "float64": ("d", 8, "f", "<f8"),
    "float32": ("f", 4, "f", "<f4"),
    "int8": ("b", 1, "i", "|i1"),
    "int16": ("h", 2, "i", "<i2"),
    "int32": ("i", 4, "i", "<i4"),
    "int64": ("q", 8, "i", "<i8"),
    "uint8": ("B", 1, "u", "|u1"),
    "uint16": ("H", 2, "u", "<u2"),
    "uint32": ("I", 4, "u", "<u4"),
    "uint64": ("Q", 8, "u", "<u8"),
    "bool": ("?", 1, "b", "|b1"),
    "complex128": ("d", 8, "c", "<c16"),  # two float64 components per element
    "complex64": ("f", 4, "c", "<c8"),  # two float32 components per element
}


def present_binary_value(value: dict[str, Any], *, name: str) -> tuple[dict[str, Any], list[Artifact]]:
    """Turn a wire ``value`` (with ``data_b64``) into an AI-facing value.

    Returns the presentation dict and any artifacts (e.g. a saved ``.npy``).
    Falls back to returning the input unchanged if it is not a decodable payload.
    """

    dtype = value.get("dtype")
    if dtype not in _DTYPES or "data_b64" not in value:
        return value, []

    code, comp_size, kind, descr = _DTYPES[dtype]
    dims = [int(d) for d in value.get("dims", [])]
    element_count = int(value.get("element_count", 0))
    raw = base64.b64decode(value.get("data_b64", ""))

    presented: dict[str, Any] = {
        "class_name": value.get("class_name", ""),
        "dtype": dtype,
        "complexity": value.get("complexity", "real"),
        "shape": dims,
        "size": value.get("size", ""),
        "element_count": element_count,
    }

    flat = _decode(raw, code, kind, element_count)

    if element_count <= MAX_INLINE_ELEMENTS:
        presented["data"] = _nest(flat, dims)
        presented["truncated"] = False
        return presented, []

    # Large array: summary + preview + lossless .npy artifact.
    presented["truncated"] = True
    presented["preview"] = flat[:PREVIEW_ELEMENTS]
    if kind in ("f", "i", "u") and element_count <= STATS_LIMIT:
        presented["summary"] = _real_summary(flat)
    elif kind == "c":
        presented["summary"] = {"note": "complex array; see .npy artifact for full data"}
    else:
        presented["summary"] = {"note": "array too large for inline statistics"}

    artifact = _write_npy_artifact(name, descr, dims, raw)
    presented["artifact"] = artifact.path
    return presented, [artifact]


def present_structured(node: Any) -> Any:
    """Recursively tidy a non-binary structured value (char/string/struct/cell).

    Nested numeric/logical leaves arrive as a bounded column-major ``data`` list;
    reshape them to row-major nested lists so they read like the top-level binary
    values. char/string nodes are returned as-is. Non-dict inputs pass through.
    """

    if not isinstance(node, dict):
        return node

    kind = node.get("type")
    if kind in ("numeric_array", "logical_array") and "data" in node and "dims" in node:
        reshaped = dict(node)
        reshaped["data"] = _nest(node["data"], [int(d) for d in node["dims"]])
        return reshaped
    if kind == "struct":
        reshaped = dict(node)
        reshaped["data"] = [
            {k: present_structured(v) for k, v in element.items()}
            for element in node.get("data", [])
            if isinstance(element, dict)
        ]
        return reshaped
    if kind == "cell":
        reshaped = dict(node)
        reshaped["data"] = [present_structured(v) for v in node.get("data", [])]
        return reshaped
    return node


def _decode(raw: bytes, code: str, kind: str, element_count: int) -> list[Any]:
    if element_count == 0:
        return []
    if kind == "c":
        comps = struct.unpack("<" + code * (element_count * 2), raw)
        return [_finite_pair(comps[2 * i], comps[2 * i + 1]) for i in range(element_count)]
    if kind == "b":
        return [b != 0 for b in raw[:element_count]]
    values = struct.unpack("<" + code * element_count, raw)
    if kind == "f":
        return [_finite(v) for v in values]
    return list(values)


def _finite(v: float) -> Any:
    """Keep JSON valid: map non-finite floats to explicit strings."""
    if math.isnan(v):
        return "NaN"
    if math.isinf(v):
        return "Infinity" if v > 0 else "-Infinity"
    return v


def _finite_pair(re: float, im: float) -> list[Any]:
    return [_finite(re), _finite(im)]


def _nest(flat: list[Any], dims: list[int]) -> list[Any]:
    """Column-major flat -> row-major nested for matrices; flat for vectors/nd."""
    if len(dims) == 2 and dims[0] != 1 and dims[1] != 1:
        m, n = dims
        return [[flat[j * m + i] for j in range(n)] for i in range(m)]
    return list(flat)


def _real_summary(flat: list[Any]) -> dict[str, Any]:
    finite = [v for v in flat if isinstance(v, (int, float)) and not isinstance(v, bool)]
    non_finite = len(flat) - len(finite)
    summary: dict[str, Any] = {"non_finite": non_finite}
    if finite:
        summary["min"] = min(finite)
        summary["max"] = max(finite)
        summary["mean"] = sum(finite) / len(finite)
    return summary


def _shape_repr(dims: list[int]) -> str:
    if len(dims) == 1:
        return f"({dims[0]},)"
    return "(" + ", ".join(str(d) for d in dims) + ")"


def _write_npy_artifact(name: str, descr: str, dims: list[int], raw: bytes) -> Artifact:
    out_dir = os.path.join(tempfile.gettempdir(), "baltamatica-mcp-vars")
    os.makedirs(out_dir, exist_ok=True)
    safe = "".join(c if (c.isalnum() or c == "_") else "_" for c in name) or "var"
    path = os.path.join(out_dir, f"{safe}.npy")

    shape = dims if dims else [len(raw)]
    header = "{'descr': %r, 'fortran_order': True, 'shape': %s, }" % (descr, _shape_repr(shape))
    prefix = b"\x93NUMPY\x01\x00"
    unpadded = len(prefix) + 2 + len(header) + 1
    pad = (64 - (unpadded % 64)) % 64
    header_bytes = (header + " " * pad + "\n").encode("latin1")

    with open(path, "wb") as fh:
        fh.write(prefix)
        fh.write(struct.pack("<H", len(header_bytes)))
        fh.write(header_bytes)
        fh.write(raw)

    return Artifact(
        path=path,
        type="application/x-npy",
        exists=True,
        size=os.path.getsize(path),
    )
