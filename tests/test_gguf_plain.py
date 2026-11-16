#!/usr/bin/env python3
"""Host check for qwen_m2_decode.open_gguf: the reader maps the file as a plain ndarray (np.memmap makes every slice a subclass instance) and returns
exactly the fields and tensors the stock GGUFReader returns. No device."""

import sys
from pathlib import Path

import numpy as np
import pytest

gguf = pytest.importorskip("gguf")
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from qwen_m2_decode import open_gguf


@pytest.fixture
def model(tmp_path):
    path = tmp_path / "tiny.gguf"
    writer = gguf.GGUFWriter(str(path), "qwen3")
    writer.add_uint32("qwen3.attention.key_length", 128)
    writer.add_uint32("qwen3.rope.dimension_count", 64)
    writer.add_float32("qwen3.rope.freq_base", 1000000.0)
    writer.add_tensor("token_embd.weight", np.arange(24, dtype=np.float32).reshape(4, 6))
    writer.write_header_to_file()
    writer.write_kv_data_to_file()
    writer.write_tensors_to_file()
    writer.close()
    return path


def test_open_gguf_maps_the_file_as_a_plain_ndarray(model):
    reader = open_gguf(model)
    assert type(reader.data) is np.ndarray
    assert type(next(t for t in reader.tensors if t.name == "token_embd.weight").data) is np.ndarray


def test_open_gguf_reads_what_the_stock_reader_reads(model):
    stock, plain = gguf.GGUFReader(model), open_gguf(model)
    assert list(plain.fields) == list(stock.fields)
    for name in stock.fields:
        assert [np.asarray(p).tolist() for p in plain.fields[name].parts] == [np.asarray(p).tolist() for p in stock.fields[name].parts]
    assert [t.name for t in plain.tensors] == [t.name for t in stock.tensors]
    for a, b in zip(plain.tensors, stock.tensors):
        assert a.tensor_type == b.tensor_type and np.array_equal(np.asarray(a.data), np.asarray(b.data))
