"""The Python dtypes must describe exactly the bytes the C++ structs write."""

from __future__ import annotations

import json
import subprocess

import pytest

import schema


@pytest.fixture(scope="module")
def cpp_schema(tool):
    result = subprocess.run([str(tool("backtest")), "--print-schema"], capture_output=True,
                            text=True, check=True)
    return json.loads(result.stdout)


def test_format_version_and_header(cpp_schema):
    assert cpp_schema["format_version"] == schema.FORMAT_VERSION
    assert cpp_schema["header_size"] == schema.FILE_HEADER.itemsize == 24
    assert schema.MAGIC.rstrip(b"\x00").decode() == cpp_schema["magic"]


@pytest.mark.parametrize("name", ["orders", "fills", "bbo", "risk"])
def test_record_layout_matches_field_by_field(cpp_schema, name):
    file_name, type_id, dtype = schema.RECORDS[name]
    cpp = cpp_schema["records"][name]
    assert cpp["file"] == file_name
    assert cpp["type"] == type_id
    assert cpp["size"] == dtype.itemsize

    python_fields = [(n, dtype.fields[n][1], dtype.fields[n][0].itemsize) for n in dtype.names]
    cpp_fields = [(f["name"], f["offset"], f["size"]) for f in cpp["fields"]]
    assert python_fields == cpp_fields
    # No gaps and no overlap: the fields tile the record exactly.
    end = 0
    for _, offset, size in cpp_fields:
        assert offset == end
        end = offset + size
    assert end == cpp["size"]


def test_enumerations_match(cpp_schema):
    assert set(cpp_schema["enums"]) == set(schema.ENUMS)
    for name, values in schema.ENUMS.items():
        assert cpp_schema["enums"][name] == values, name
