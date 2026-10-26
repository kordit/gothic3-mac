"""Chunk files are stable: a function added to the image changes the chunk it
lies in and no other, so the C compiler rebuilds that chunk alone."""

import os
import struct
import sys
from types import SimpleNamespace

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

import translate as T  # noqa: E402
from test_translate_driver import synthetic_image, translate_entry_fixture  # noqa: E402


def _fn(addr):
    return SimpleNamespace(addr=addr)


def test_a_chunk_is_an_aligned_span_of_addresses():
    fns = [_fn(a) for a in (0x00409000, 0x00401000, 0x00401010, 0x00405000)]
    bodies = {fn.addr: ["x"] for fn in fns}
    groups = T.chunk_groups(fns, bodies, span=0x4000)
    assert [(start, [f.addr for f in g]) for start, g in groups] == [
        (0x00400000, [0x00401000, 0x00401010]),
        (0x00404000, [0x00405000]),
        (0x00408000, [0x00409000]),
    ]


def test_an_oversized_span_is_halved_by_address():
    fns = [_fn(a) for a in (0x00400010, 0x00401000, 0x00402000, 0x00403000)]
    bodies = {fn.addr: ["x" * 99] for fn in fns}  # 100 bytes each
    groups = T.chunk_groups(fns, bodies, span=0x4000, limit=250)
    assert [(start, len(g)) for start, g in groups] == [(0x00400000, 2), (0x00402000, 2)]
    # A single function never splits, however large.
    assert T.chunk_groups(fns[:1], bodies, span=0x4000, limit=1) == [(0x00400000, fns[:1])]


def test_a_chunk_declares_what_it_names():
    text = "CALL_FN(00401000);\nvoid fn_00402000(X86 *c) { FN(00403000)(c); }\n"
    called, used = T.chunk_references(text)
    assert called == {0x00401000}
    assert used == {0x00401000, 0x00402000, 0x00403000}
    decl = T.declarations("recomp_", sorted(used), sorted(called))
    assert "void fn_00403000(X86 *c);" in decl
    assert "recomp_hk_00401000" in decl and "recomp_hk_00402000" not in decl


# Three functions in three spans; the first calls the second. CALL rel32 at
# 0x00401000 to 0x00405000, then RET.
CALLER, MIDDLE, LAST, ADDED = 0x00401000, 0x00405000, 0x00409000, 0x00405100


def _image(with_added):
    rel = MIDDLE - (CALLER + 5)
    blocks = {CALLER: b"\xe8" + struct.pack("<i", rel) + b"\xc3",
              MIDDLE: b"\xb8\x01\0\0\0\xc3", LAST: b"\xb8\x02\0\0\0\xc3"}
    if with_added:
        blocks[ADDED] = b"\xb8\x03\0\0\0\xc3"
    return synthetic_image(blocks, size=0x10000), blocks


def _translate(tmp_path, monkeypatch, name, image):
    work = tmp_path / name
    work.mkdir()
    img, blocks = image
    translate_entry_fixture(work, monkeypatch, img, blocks)
    out = work / "gen"
    return {p.name: p.read_text() for p in out.iterdir() if p.suffix in (".c", ".h")}


def test_adding_a_function_changes_only_its_own_chunk(tmp_path, monkeypatch):
    # Every image first: translating patches translate.Image.
    images = [_image(False), _image(False), _image(True)]
    first = _translate(tmp_path, monkeypatch, "a", images[0])
    again = _translate(tmp_path, monkeypatch, "b", images[1])
    grown = _translate(tmp_path, monkeypatch, "c", images[2])
    assert first == again  # the same input translates to the same bytes
    assert sorted(n for n in first if n.startswith("chunk")) == [
        "chunk.h", "chunk_00400000.c", "chunk_00404000.c", "chunk_00408000.c"]
    changed = sorted(n for n in grown if grown[n] != first.get(n))
    # table.c and funcs.h list every function; no chunk includes either.
    assert changed == ["chunk_00404000.c", "funcs.h", "table.c"]
    for name, text in grown.items():
        if name.startswith("chunk_") and name.endswith(".c"):
            assert '#include "funcs.h"' not in text
            assert "FIDX" not in text
    assert "recomp_hk_%08x" % MIDDLE in grown["chunk_00400000.c"]
