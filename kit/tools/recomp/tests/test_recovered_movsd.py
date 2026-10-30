"""Decode real bytes before emission: SSE MOVSD must not become string MOVSD.

The constructor fragment comes from the Stage A SDK DLL. Losing its operands
changed EDI, which ScriptInit kept as the component pointer across the call.
Check recovered/emitted C against the architectural register and memory
effects, including preservation of the component pointer in EDI.
Genuine string moves must retain their implicit operands.
"""
import ctypes as C
import struct

import capstone
import pytest
import test_translate_insns as harness


def setup(rng):
    return {"regs": harness.rand_regs(rng, ESI=harness.SCRATCH + 0x100,
                                      EDI=harness.SCRATCH + 0x800,
                                      ECX=harness.SCRATCH + 0x200)}


CASES = []
for index, (name, code) in enumerate([
    ("sdk_constructor", "56 89 ce 0f 57 c9 f2 0f 11 8e 84 00 00 00 5e c3"),
    ("scalar_load", "f2 0f 10 06 c3"),
    ("scalar_store", "f2 0f 11 06 c3"),
    ("scalar_register", "f2 0f 10 c1 c3"),
    ("string_move", "a5 c3"),
    ("repeated_string_move", "b9 04 00 00 00 f3 a5 c3"),
]):
    addr = harness.CODE_BASE + index * 0x100
    image = harness.T.Image.__new__(harness.T.Image)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    instructions = [image.to_insn(ci) for ci in md.disasm(bytes.fromhex(code), addr)]
    lines = [(i.addr - addr, i.raw.split(None, 1)[1]) for i in instructions]
    CASES.append(harness.Case(name, addr, lines, code, setup))


@pytest.fixture(scope="module", params=[False, True], ids=["context", "x87-locals"])
def built(request):
    old_cases, old_x87 = harness.CASES, harness.T.X87_LOCALS
    harness.CASES, harness.T.X87_LOCALS = CASES, request.param
    try:
        return harness.built.__wrapped__()
    finally:
        harness.CASES, harness.T.X87_LOCALS = old_cases, old_x87


@pytest.mark.parametrize("case", CASES, ids=lambda case: case.name)
def test_recovered_execution(case, built):
    native, errors = built
    assert case.name not in errors, errors.get(case.name)
    c = harness.X86()
    for i in range(8):
        c.r[i] = 0x12340000 + i
        for j in range(4): c.xmm[i][j] = 0x11223344 + i * 0x100 + j
    c.r[1] = harness.SCRATCH + 0x200
    c.r[6] = harness.SCRATCH + 0x100
    c.r[7] = harness.SCRATCH + 0x800
    c.r[4] = harness.ESP_INIT
    expected_regs = list(c.r)
    expected_regs[4] += 4
    expected_xmm = [list(row) for row in c.xmm]
    data = bytes((i * 17 + 3) & 255 for i in range(harness.SCRATCH_SIZE))
    expected = bytearray(data)
    if case.name == "sdk_constructor":
        expected[0x284:0x28c] = bytes(8)
        expected_xmm[1] = [0, 0, 0, 0]
    elif case.name == "scalar_load":
        expected_xmm[0] = list(struct.unpack("<II", data[0x100:0x108])) + [0, 0]
    elif case.name == "scalar_store":
        expected[0x100:0x108] = struct.pack("<II", *expected_xmm[0][:2])
    elif case.name == "scalar_register":
        expected_xmm[0][:2] = expected_xmm[1][:2]
    else:
        count = 4 if case.name == "repeated_string_move" else 1
        expected[0x800:0x800 + count * 4] = data[0x100:0x100 + count * 4]
        expected_regs[6] += count * 4
        expected_regs[7] += count * 4
        if count == 4: expected_regs[1] = 0
    native.zero(harness.STACK_BASE, harness.STACK_SIZE)
    native.write(harness.SCRATCH, data)
    native.write(c.r[4], struct.pack("<I", harness.MAGIC_RET))
    native.lib.harness_run(case.addr, C.byref(c))
    assert list(c.r) == expected_regs, "general registers changed incorrectly (including ESI/EDI)"
    assert [list(row) for row in c.xmm] == expected_xmm
    assert native.read(harness.SCRATCH, harness.SCRATCH_SIZE) == expected
