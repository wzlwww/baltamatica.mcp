from __future__ import annotations

import base64
import struct

import pytest

from baltamatica_mcp.serializer import (
    MAX_INLINE_ELEMENTS,
    encode_for_set,
    present_binary_value,
    present_structured,
    to_baltamatica_literal,
)


def _wire(dtype: str, dims: list[int], raw: bytes, *, complexity: str = "real",
          class_name: str = "double") -> dict:
    element_count = 1
    for d in dims:
        element_count *= d
    return {
        "supported": True,
        "type": "ndarray",
        "class_name": class_name,
        "dtype": dtype,
        "complexity": complexity,
        "size": "x".join(str(d) for d in dims),
        "dims": dims,
        "element_count": element_count,
        "encoding": "base64",
        "byte_order": "little",
        "data_b64": base64.b64encode(raw).decode("ascii"),
    }


def test_real_double_matrix_reshapes_column_major_to_row_major() -> None:
    # Matrix [[1,2,3],[4,5,6]] stored column-major: 1,4,2,5,3,6
    raw = struct.pack("<6d", 1.0, 4.0, 2.0, 5.0, 3.0, 6.0)
    value, artifacts = present_binary_value(_wire("float64", [2, 3], raw), name="B")

    assert artifacts == []
    assert value["dtype"] == "float64"
    assert value["shape"] == [2, 3]
    assert value["data"] == [[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]]
    assert value["truncated"] is False


def test_row_vector_is_flat() -> None:
    raw = struct.pack("<3d", 7.0, 8.0, 9.0)
    value, _ = present_binary_value(_wire("float64", [1, 3], raw), name="v")
    assert value["data"] == [7.0, 8.0, 9.0]


def test_logical_decodes_to_bools() -> None:
    raw = bytes([1, 0, 1])
    value, _ = present_binary_value(
        _wire("bool", [1, 3], raw, class_name="logical"), name="L"
    )
    assert value["data"] == [True, False, True]


def test_integer_types_roundtrip() -> None:
    raw = struct.pack("<4i", -2, -1, 0, 100000)
    value, _ = present_binary_value(_wire("int32", [1, 4], raw, class_name="int32"), name="k")
    assert value["data"] == [-2, -1, 0, 100000]


def test_complex_decodes_to_real_imag_pairs() -> None:
    # two complex128 values: (1+2j), (3-4j) column-major interleaved re,im
    raw = struct.pack("<4d", 1.0, 2.0, 3.0, -4.0)
    value, _ = present_binary_value(
        _wire("complex128", [1, 2], raw, complexity="complex"), name="z"
    )
    assert value["complexity"] == "complex"
    assert value["data"] == [[1.0, 2.0], [3.0, -4.0]]


def test_non_finite_floats_become_strings() -> None:
    raw = struct.pack("<3d", float("nan"), float("inf"), float("-inf"))
    value, _ = present_binary_value(_wire("float64", [1, 3], raw), name="w")
    assert value["data"] == ["NaN", "Infinity", "-Infinity"]


def test_large_array_gets_summary_preview_and_npy_artifact(tmp_path, monkeypatch) -> None:
    monkeypatch.setattr("tempfile.gettempdir", lambda: str(tmp_path))
    n = MAX_INLINE_ELEMENTS + 50
    data = [float(i) for i in range(n)]
    raw = struct.pack(f"<{n}d", *data)
    value, artifacts = present_binary_value(_wire("float64", [n, 1], raw), name="big")

    assert value["truncated"] is True
    assert "data" not in value
    assert value["preview"] == data[:12]
    assert value["summary"]["min"] == 0.0
    assert value["summary"]["max"] == float(n - 1)
    assert value["summary"]["non_finite"] == 0

    assert len(artifacts) == 1
    art = artifacts[0]
    assert art.path.endswith("big.npy")
    assert art.exists is True
    assert art.size > 0
    with open(art.path, "rb") as fh:
        head = fh.read(8)
    assert head == b"\x93NUMPY\x01\x00"


def test_npy_artifact_loads_with_numpy_if_available(tmp_path, monkeypatch) -> None:
    np = pytest.importorskip("numpy")
    monkeypatch.setattr("tempfile.gettempdir", lambda: str(tmp_path))
    # 3x2 column-major matrix
    raw = struct.pack("<6d", 1.0, 2.0, 3.0, 4.0, 5.0, 6.0)  # cols: (1,2,3),(4,5,6)
    wire = _wire("float64", [3, 2], raw)
    wire["element_count"] = 6
    # force the large-array path
    monkeypatch.setattr("baltamatica_mcp.serializer.MAX_INLINE_ELEMENTS", 1)
    _, artifacts = present_binary_value(wire, name="M")
    loaded = np.load(artifacts[0].path)
    assert loaded.shape == (3, 2)
    assert loaded[:, 0].tolist() == [1.0, 2.0, 3.0]
    assert loaded[:, 1].tolist() == [4.0, 5.0, 6.0]


def test_non_binary_value_passes_through() -> None:
    value = {"supported": False, "type": "unsupported", "class_name": "cell"}
    out, artifacts = present_binary_value(value, name="c")
    assert out is value
    assert artifacts == []


def test_present_structured_reshapes_nested_numeric_in_cell() -> None:
    # cell {[1 2;3 4], 'text'}; matrix stored column-major as [1,3,2,4]
    cell = {
        "type": "cell",
        "dims": [1, 2],
        "data": [
            {"type": "numeric_array", "dims": [2, 2], "data": [1, 3, 2, 4]},
            {"type": "char", "text": "text"},
        ],
    }
    out = present_structured(cell)
    assert out["data"][0]["data"] == [[1, 2], [3, 4]]  # row-major
    assert out["data"][1] == {"type": "char", "text": "text"}


def test_present_structured_reshapes_struct_fields() -> None:
    struct_node = {
        "type": "struct",
        "fields": ["xy"],
        "data": [{"xy": {"type": "numeric_array", "dims": [2, 2], "data": [1, 3, 2, 4]}}],
    }
    out = present_structured(struct_node)
    assert out["data"][0]["xy"]["data"] == [[1, 2], [3, 4]]


def test_encode_for_set_scalar_and_bool() -> None:
    assert encode_for_set(3.5) == ("float64", [1, 1], struct.pack("<d", 3.5))
    assert encode_for_set(True) == ("bool", [1, 1], b"\x01")


def test_encode_for_set_row_vector() -> None:
    dtype, dims, raw = encode_for_set([1, 2, 3])
    assert dtype == "float64"
    assert dims == [1, 3]
    assert raw == struct.pack("<3d", 1.0, 2.0, 3.0)


def test_encode_for_set_matrix_is_column_major() -> None:
    dtype, dims, raw = encode_for_set([[1, 2], [3, 4]])
    assert dtype == "float64"
    assert dims == [2, 2]
    assert raw == struct.pack("<4d", 1.0, 3.0, 2.0, 4.0)


def test_encode_for_set_bool_matrix() -> None:
    dtype, dims, raw = encode_for_set([[True, False, True]])
    assert dtype == "bool"
    assert dims == [1, 3]
    assert raw == bytes([1, 0, 1])


def test_encode_for_set_rejects_ragged_and_non_numeric() -> None:
    with pytest.raises(ValueError):
        encode_for_set([[1, 2], [3]])
    with pytest.raises(ValueError):
        encode_for_set(["a", "b"])


def test_to_baltamatica_literal() -> None:
    assert to_baltamatica_literal([[1, 2], [3, 4]]) == "[1.0  2.0; 3.0  4.0]"
    assert to_baltamatica_literal([1, 2, 3]) == "[1.0  2.0  3.0]"
    assert to_baltamatica_literal(True) == "true"
    assert to_baltamatica_literal(5) == "5.0"


def test_encode_for_set_integer_dtypes() -> None:
    dtype, dims, raw = encode_for_set([[1, 2], [3, 4]], "int32")
    assert dtype == "int32"
    assert dims == [2, 2]
    assert raw == struct.pack("<4i", 1, 3, 2, 4)  # column-major

    dtype, dims, raw = encode_for_set([10, 20, 255], "uint8")
    assert dtype == "uint8"
    assert raw == bytes([10, 20, 255])


def test_encode_for_set_integer_out_of_range() -> None:
    with pytest.raises(ValueError):
        encode_for_set([300], "uint8")


def test_encode_for_set_complex_interleaves_column_major() -> None:
    # 1x2 complex: (1+2i), (3+4i)
    dtype, dims, raw = encode_for_set({"real": [1, 3], "imag": [2, 4]}, "complex128")
    assert dtype == "complex128"
    assert dims == [1, 2]
    assert raw == struct.pack("<4d", 1.0, 2.0, 3.0, 4.0)  # re0,im0,re1,im1


def test_encode_for_set_complex_requires_real_imag_same_shape() -> None:
    with pytest.raises(ValueError):
        encode_for_set({"real": [1, 2], "imag": [1]}, "complex128")
    with pytest.raises(ValueError):
        encode_for_set([1, 2], "complex128")  # not a {real, imag} dict


def test_to_baltamatica_literal_with_dtype() -> None:
    assert to_baltamatica_literal([[1, 2], [3, 4]], "int32") == "int32([1.0  2.0; 3.0  4.0])"
    assert to_baltamatica_literal(
        {"real": [1, 3], "imag": [2, 4]}, "complex128"
    ) == "complex([1.0  3.0], [2.0  4.0])"
