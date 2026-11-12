import re
import struct
from pathlib import Path

import pytest

cs = pytest.importorskip("capstone")


def test_trace_stub_words_disassemble():
    src = (Path(__file__).parent / "ane_h15_fwload.h").read_text()
    words = [int(w, 16) for w in re.findall(r"\t(0x[0-9a-f]{8}),\t/\*", src)]
    md = cs.Cs(cs.CS_ARCH_ARM64, cs.CS_MODE_ARM)
    text = [f"{i.mnemonic} {i.op_str}" for n, w in enumerate(words)
            for i in md.disasm(struct.pack("<I", w), 0x1A0 + 4 * n)]
    assert text[0] == "adrp x0, #0x300000"
    assert [t.split()[0] for t in text[1:5]] == ["mrs"] * 4
    assert {"esr_el1", "far_el1", "elr_el1", "spsr_el1"} == {t.split()[-1] for t in text[1:5]}
    assert text[-1] == "b #0x1cc"
    assert "mov x6, #0x5045" in text
