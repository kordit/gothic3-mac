"""Instruction forms the first corpus never used, checked against Unicorn.

    .venv/bin/python -m pytest -q tools/recomp/tests/test_translate_insns.py

Each case is a synthetic listing in the export grammar plus the machine code
it describes.  The listing goes through the translator exactly as a game
function does, the emitted C is compiled with tools/recomp/tests/harness.c
into one shared library, and the same initial registers and memory are run
through the native code and through Unicorn.  Registers, the defined
arithmetic flags and the scratch memory both sides wrote must agree.

No game is needed: the cases are their own machine code.  A translator that
cannot emit a case fails that case's test with the translator's error, and
the remaining cases still build and run.  clang must be on PATH (a skip
otherwise)."""

import ctypes as C
import os
import platform
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile

import pytest
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_INTR, UcError
from unicorn.x86_const import (
    UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
    UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI,
    UC_X86_REG_EFLAGS, UC_X86_REG_FPCW, UC_X86_REG_FPSW, UC_X86_REG_FPTAG,
    UC_X86_REG_MXCSR)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "tools/recomp"))
import translate as T  # noqa: E402

# Guest layout shared by both engines.  The code lives in its own page range,
# the stack and scratch are zeroed before every run, and the cave holds the
# return address a RET lands on, which is where emulation stops.
CODE_BASE, CODE_SIZE = 0x0D010000, 0x00020000
STACK_BASE, STACK_SIZE = 0x0EF00000, 0x00100000
ESP_INIT = 0x0EFFFF00
SCRATCH, SCRATCH_SIZE = 0x0E100000, 0x00010000
CAVE, CAVE_SIZE = 0x0DEAC000, 0x1000
MAGIC_RET = CAVE

EFLAGS_COMPARED = 0x0CC5   # CF PF ZF SF DF OF
EFLAGS_MISC_INIT = 0x202
FPU_CW_INIT = 0x037F
FPU_TAG_INIT = 0xFFFF
CS_SELECTOR = 0x1B          # the flat user-mode code selector Windows hands a process

UC_REGS = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
           UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI]
REG_NAMES = ["EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"]


class X86(C.Structure):
    _fields_ = [
        ("r", C.c_uint32 * 8), ("eip", C.c_uint32),
        ("eflags_cf", C.c_uint32), ("eflags_zf", C.c_uint32),
        ("eflags_sf", C.c_uint32), ("eflags_of", C.c_uint32),
        ("eflags_pf", C.c_uint32), ("eflags_af", C.c_uint32),
        ("eflags_df", C.c_uint32), ("eflags_misc", C.c_uint32),
        ("st", C.c_double * 8), ("st_bits", C.c_uint64 * 8),
        ("st_exact", C.c_uint8 * 8), ("fpu_top", C.c_uint32),
        ("fpu_cw", C.c_uint16), ("fpu_sw", C.c_uint16),
        ("fpu_tag", C.c_uint16),
        ("fs_base", C.c_uint32),
        ("xmm", (C.c_uint32 * 4) * 8),
        ("mm", C.c_uint64 * 8),
    ]


# ------------------------------------------------------------------ cases --

class Case(object):
    """One synthetic function.

    `lines` are (offset, listing text) pairs; `code` is the hex of the same
    bytes.  `setup(rng)` returns a dict with optional `regs` (name -> value),
    `mem` (list of (address, bytes)) and `args` (dwords pushed above the
    return address).  `ignore` lists (address, length) ranges of scratch the
    comparison skips; `check(native, emu)` adds case-specific assertions where
    exact bytes are the wrong measure (x87 results)."""

    def __init__(self, name, addr, lines, code, setup=None, ignore=(), check=None,
                 expect_intr=None, entries=()):
        self.name = name
        self.addr = addr
        self.lines = lines
        self.code = bytes.fromhex(code.replace(" ", ""))
        self.setup = setup or (lambda rng: {})
        self.ignore = ignore
        self.check = check
        self.expect_intr = expect_intr
        self.entries = entries

    def listing(self):
        return "\n".join("%08x  %s" % (self.addr + off, text) for off, text in self.lines) + "\n"


def rand_regs(rng, **fixed):
    regs = {name: rng.getrandbits(32) for name in REG_NAMES if name != "ESP"}
    regs.update(fixed)
    return regs


def cmpxchg_setup(rng):
    """Exercise both the matching and nonmatching accumulator paths."""
    mem = rng.getrandbits(32)
    eax = mem if rng.random() < 0.5 else rng.getrandbits(32)
    return {"regs": rand_regs(rng, ECX=SCRATCH + 0x40, EAX=eax),
            "mem": [(SCRATCH + 0x40, struct.pack("<I", mem))]}


def cmpxchg8b_setup(rng):
    """Compare equal qwords and mismatches confined to either dword."""
    mem = rng.getrandbits(64)
    expected = mem
    if rng.random() < 0.5:
        expected ^= 1 << rng.randrange(64)
    return {"regs": rand_regs(rng, EDI=SCRATCH + 0x40,
                              EAX=expected & 0xffffffff, EDX=expected >> 32),
            "mem": [(SCRATCH + 0x40, struct.pack("<Q", mem))]}


def loop_setup(rng):
    # ECX = 0 would spin 2^32 times; the CRT never enters a LOOP that way.
    return {"regs": rand_regs(rng, ECX=rng.randint(1, 40))}


def cmps_setup(rng):
    """Two dword arrays that agree for a random prefix, ECX covering them."""
    count = rng.randint(1, 12)
    same = rng.randint(0, count)          # == count: the arrays are equal
    left = [rng.getrandbits(32) for _ in range(count)]
    right = list(left)
    if same < count:
        right[same] ^= rng.randint(1, 0xFFFFFFFF)
    a, b = SCRATCH + 0x100, SCRATCH + 0x800
    return {"regs": rand_regs(rng, ECX=count, ESI=a, EDI=b),
            "mem": [(a, struct.pack("<%dI" % count, *left)),
                    (b, struct.pack("<%dI" % count, *right))]}


def cmpsw_setup(rng):
    count = rng.randint(1, 12)
    same = rng.randint(0, count)
    left = [rng.getrandbits(16) for _ in range(count)]
    right = list(left)
    if same < count:
        right[same] ^= rng.randint(1, 0xFFFF)
    a, b = SCRATCH + 0x100, SCRATCH + 0x800
    return {"regs": rand_regs(rng, ECX=count, ESI=a, EDI=b),
            "mem": [(a, struct.pack("<%dH" % count, *left)),
                    (b, struct.pack("<%dH" % count, *right))]}


def scas_setup(rng, width):
    count = rng.randint(1, 12)
    needle = rng.getrandbits(width * 8)
    hay = [rng.getrandbits(width * 8) for _ in range(count)]
    if rng.random() < 0.7:
        hay[rng.randrange(count)] = needle
    fmt = {2: "H", 4: "I"}[width]
    a = SCRATCH + 0x100
    return {"regs": rand_regs(rng, ECX=count, EDI=a, EAX=needle | (rng.getrandbits(32) << (width * 8))),
            "mem": [(a, struct.pack("<%d%s" % (count, fmt), *hay))]}


def seg_store_setup(rng):
    return {"regs": rand_regs(rng, ECX=SCRATCH + 0x40)}


def check_seg_store(native, emu):
    # Unicorn has no GDT, so its CS is not Windows's; the translator stores
    # the selector a 32-bit Windows process sees, which is what a CONTEXT
    # record written by the CRT's exception path expects.
    assert struct.unpack("<H", bytes(native[SCRATCH + 0x40:SCRATCH + 0x42]))[0] == CS_SELECTOR


def one_double(rng, lo, hi):
    return {"args": list(struct.unpack("<II", struct.pack("<d", rng.uniform(lo, hi))))}


def fptan_setup(rng):
    return one_double(rng, -1.5, 1.5)


def fsave_setup(rng):
    # ECX names the save area; the argument is the value on the x87 stack.
    d = one_double(rng, -1e6, 1e6)
    d["regs"] = rand_regs(rng, ECX=SCRATCH + 0x1000)
    return d


def rd_double(mem, addr):
    return struct.unpack("<d", bytes(mem[addr:addr + 8]))[0]


def check_fptan(native, emu):
    esp = ESP_INIT
    for who, mem in (("native", native), ("unicorn", emu)):
        assert rd_double(mem, esp + 0xc) == 1.0, "%s: FPTAN did not push 1.0" % who
    n, u = rd_double(native, esp + 0x14), rd_double(emu, esp + 0x14)
    assert abs(n - u) <= 1e-12 * max(1.0, abs(u)), "tan: native %r unicorn %r" % (n, u)


def check_fsave(native, emu):
    area = SCRATCH + 0x1000 + 8
    # Control word, status word and tag word, then the eight registers as
    # 80-bit values.  The exception pointers in between (FIP, FCS, FDP, FDS)
    # are compared by the generic scratch check only outside the ignored
    # range: the translator does not track them.
    for lo, hi, what in ((0, 12, "environment"), (28, 108, "registers")):
        assert bytes(native[area + lo:area + hi]) == bytes(emu[area + lo:area + hi]), (
            "FSAVE %s image differs: native %s unicorn %s"
            % (what, bytes(native[area + lo:area + hi]).hex(), bytes(emu[area + lo:area + hi]).hex()))


def check_x87_constants(native, emu):
    # FLDL2E is pushed last, so it is the first value FSTP stores.
    for mem in (native, emu):
        l2e = rd_double(mem, SCRATCH + 0x100)
        ln2 = rd_double(mem, SCRATCH + 0x108)
        assert abs(l2e - 1.4426950408889634) < 1e-15, l2e
        assert abs(ln2 - 0.6931471805599453) < 1e-15, ln2


def fbstp_setup(rng):
    value = rng.randint(-999999999, 999999999)
    return {"regs": rand_regs(rng, ECX=SCRATCH + 0x200),
            "mem": [(SCRATCH + 0x210, struct.pack("<i", value))]}


def sse_scalar_setup(rng):
    """Two doubles worth dividing and comparing, and an integer to convert.
    The second is never zero, and both are plain values: this checks the
    translation, not what the host does with a NaN."""
    a = rng.uniform(-1e6, 1e6)
    b = rng.choice([rng.uniform(1.0, 1e3), -rng.uniform(1.0, 1e3), a])
    return {"regs": dict(rand_regs(rng), ESI=SCRATCH + 0x300, EDI=SCRATCH + 0x100,
                         EDX=rng.randrange(-10000, 10000) & 0xFFFFFFFF),
            "mem": [(SCRATCH + 0x300, struct.pack("<dd", a, b)),
                    (SCRATCH + 0x100, b"\x00" * 0x100)]}


def sse_packed_setup(rng):
    """Two vectors of four plain floats, the second never zero and sometimes
    equal to the first lane by lane, so the comparisons go both ways."""
    a = [rng.uniform(-100.0, 100.0) for _ in range(4)]
    b = [rng.choice([rng.uniform(1.0, 50.0), -rng.uniform(1.0, 50.0), x]) for x in a]
    return {"regs": dict(rand_regs(rng), ESI=SCRATCH + 0x300, EDI=SCRATCH + 0x100),
            "mem": [(SCRATCH + 0x300, struct.pack("<8f", *a, *b)),
                    (SCRATCH + 0x100, b"\x00" * 0x100)]}


def matrix_pair_setup(rng):
    """Two 4x4 float matrices of plain values, EAX at the first (also the
    result) and ECX at the second."""
    vals = [rng.uniform(-4.0, 4.0) for _ in range(32)]
    return {"regs": dict(rand_regs(rng), EAX=SCRATCH + 0x300, ECX=SCRATCH + 0x340),
            "mem": [(SCRATCH + 0x300, struct.pack("<32f", *vals))]}


def box_point_setup(rng):
    """A box (min then max corner) at ECX and a point argument inside it, outside
    it or on one of its faces, where an inclusive test has to say yes."""
    lo = [rng.uniform(-50.0, 0.0) for _ in range(3)]
    hi = [x + rng.uniform(1.0, 50.0) for x in lo]
    point = [rng.uniform(a - 10.0, b + 10.0) for a, b in zip(lo, hi)]
    if rng.random() < 0.3:
        axis = rng.randrange(3)
        point[axis] = rng.choice([lo[axis], hi[axis]])
    return {"regs": dict(rand_regs(rng), ECX=SCRATCH + 0x300),
            "mem": [(SCRATCH + 0x300, struct.pack("<6f", *lo, *hi)),
                    (SCRATCH + 0x340, struct.pack("<3f", *point))],
            "args": [SCRATCH + 0x340]}


def string_copy_setup(rng):
    """A source and destination in the scratch area that overlap in either
    direction, touch or lie apart, unaligned as often as not, with a count from
    zero up and random bytes under both: the REP forms' fast paths have to give
    what the element-by-element copy gives, a forward overlap included."""
    src = SCRATCH + 0x2000 + rng.randrange(0x100)
    delta = rng.choice([rng.randrange(-0x90, 0x90), rng.randrange(-0x1000, 0x1000), 0, 4, -4, 1, -1])
    count = rng.choice([0, 1, 2, rng.randrange(64), rng.randrange(400)])
    data = bytes(rng.getrandbits(8) for _ in range(0x1800))
    return {"regs": rand_regs(rng, ESI=src, EDI=src + delta, ECX=count),
            "mem": [(SCRATCH + 0x1000, data)]}


CASES = [
    # Gothic 3's memmove and the CRTs' memcpy and memset run REP MOVSD and
    # REP STOSD over whole buffers; the runtime copies a forward-safe range in
    # one go instead of a dword at a time. Overlap, direction and count are
    # random so that the fast path and the element loop are both exercised.
    Case("REP MOVSD/MOVSB forward and backward, overlapping", 0x0D02E800,
         [(0, "CLD"),
          (1, "PUSH ESI"),
          (2, "PUSH EDI"),
          (3, "PUSH ECX"),
          (4, "MOVSD.REP ES:EDI,ESI"),
          (6, "POP ECX"),
          (7, "POP EDI"),
          (8, "POP ESI"),
          (9, "ADD EDI,0x3000"),
          (15, "MOVSB.REP ES:EDI,ESI"),
          (17, "MOV ECX,0x9"),
          (22, "STD"),
          (23, "MOVSD.REP ES:EDI,ESI"),
          (25, "CLD"),
          (26, "RET")],
         "fc 56 57 51 f3 a5 59 5f 5e 81 c7 00 30 00 00 f3 a4 b9 09 00 00 00 fd f3 a5 fc c3",
         string_copy_setup),
    # memmove shifting an array up: from the last element down, the two
    # ranges overlapping either way round.
    Case("REP MOVSD backward over random overlap", 0x0D02EB00,
         [(0, "STD"),
          (1, "MOVSD.REP ES:EDI,ESI"),
          (3, "CLD"),
          (4, "RET")],
         "fd f3 a5 fc c3",
         string_copy_setup),
    Case("REP STOSD backward", 0x0D02EB80,
         [(0, "STD"),
          (1, "STOSD.REP ES:EDI"),
          (3, "CLD"),
          (4, "RET")],
         "fd f3 ab fc c3",
         string_copy_setup),
    Case("REP STOSD/STOSB forward and backward", 0x0D02EA00,
         [(0, "CLD"),
          (1, "PUSH EDI"),
          (2, "PUSH ECX"),
          (3, "STOSD.REP ES:EDI"),
          (5, "POP ECX"),
          (6, "POP EDI"),
          (7, "ADD EDI,0x2000"),
          (13, "STOSB.REP ES:EDI"),
          (15, "STD"),
          (16, "MOV ECX,0x7"),
          (21, "STOSD.REP ES:EDI"),
          (23, "CLD"),
          (24, "RET")],
         "fc 57 51 f3 ab 59 5f 81 c7 00 20 00 00 f3 aa fd b9 07 00 00 00 f3 ab fc c3",
         string_copy_setup),
    # A listing that misdecodes data as code can produce the port string
    # forms. A user-mode guest never reaches one, so what matters is that the
    # build survives them and that they move the pointer and count the way
    # every other string form does: the harness's port shim reads as zero.
    # A Delphi runtime's FillChar and Move use SSE2 unconditionally: it
    # predates every CPU the compiler supports, so there is no feature test
    # to fail and no scalar path to fall back to. Only data movement is
    # modelled, so memory is where the result has to agree.
    Case("SSE2 moves a vector through memory and broadcasts a dword", 0x0D02E000,
         [(0, "MOVUPS XMM0,xmmword ptr [ESI]"),
          (3, "MOVUPS xmmword ptr [EDI],XMM0"),
          (6, "MOVD XMM1,ECX"),
          (10, "PSHUFD XMM1,XMM1,0x0"),
          (15, "MOVUPS xmmword ptr [EDI + 0x10],XMM1"),
          (19, "MOVQ XMM2,qword ptr [ESI + 0x8]"),
          (24, "MOVQ qword ptr [EDI + 0x20],XMM2"),
          (29, "RET")],
         "0f 10 06 0f 11 07 66 0f 6e c9 66 0f 70 c9 00 0f 11 4f 10 f3 0f 7e 56 08 "
         "66 0f d6 57 20 c3",
         lambda rng: {"regs": dict(rand_regs(rng), ESI=SCRATCH + 0x200,
                                   EDI=SCRATCH + 0x100, ECX=0xa5b6c7d8),
                      "mem": [(SCRATCH + 0x200, bytes(range(0x40, 0x50))),
                              (SCRATCH + 0x100, b"\x00" * 0x30)]}),
    # The RTL's Move and FillChar address their vectors through a base and an
    # index, and copy register to register between loads, so those forms carry
    # the same weight as the simple ones.
    Case("SSE2 through indexed addressing and between registers", 0x0D02F000,
         [(0, "MOVUPS XMM1,xmmword ptr [ESI]"),
          (3, "MOVUPS xmmword ptr [ECX + EDX*0x1],XMM1"),
          (7, "MOVAPS XMM0,XMM1"),
          (10, "MOVD XMM0,EDX"),
          (14, "PSHUFD XMM2,XMM0,0x55"),
          (19, "MOVAPS xmmword ptr [EDI + EDX*0x1 + 0x10],XMM2"),
          (24, "MOVQ XMM1,qword ptr [ESI + 0x8]"),
          (29, "MOVQ qword ptr [EDI + EDX*0x1 + 0x20],XMM0"),
          (35, "RET")],
         "0f 10 0e 0f 11 0c 11 0f 28 c1 66 0f 6e c2 66 0f 70 d0 55 0f 29 54 17 10 "
         "f3 0f 7e 4e 08 66 0f d6 44 17 20 c3",
         lambda rng: {"regs": dict(rand_regs(rng), ESI=SCRATCH + 0x300,
                                   ECX=SCRATCH + 0x100, EDI=SCRATCH + 0x200,
                                   EDX=0x10),
                      "mem": [(SCRATCH + 0x300, bytes(range(0x10, 0x20))),
                              (SCRATCH + 0x100, b"\x00" * 0x180)]}),
    # Wine's own i386 DLLs, and every MSVC runtime since about 2005, use SSE2
    # for things that have nothing to do with floating point: memset builds
    # its fill pattern with PUNPCK and stores it with MOVUPS, and locale code
    # moves a pair of dwords through MOVD/PUNPCKLDQ. None of it asks CPUID
    # first. The lanes land in memory, which is where both engines are read.
    Case("SSE2 lane, unpack and shuffle forms", 0x0D02F800,
         [(0, "MOVUPS XMM0,xmmword ptr [ESI]"),
          (3, "MOVUPS XMM1,xmmword ptr [ESI + 0x10]"),
          (7, "MOVAPD XMM2,XMM0"),
          (11, "PUNPCKLDQ XMM2,XMM1"),
          (15, "MOVUPS xmmword ptr [EDI],XMM2"),
          (18, "MOVAPD XMM3,XMM0"),
          (22, "PUNPCKLQDQ XMM3,XMM1"),
          (26, "MOVUPS xmmword ptr [EDI + 0x10],XMM3"),
          (30, "MOVAPD XMM4,XMM0"),
          (34, "PXOR XMM4,XMM1"),
          (38, "MOVUPS xmmword ptr [EDI + 0x20],XMM4"),
          (42, "MOVAPD XMM5,XMM0"),
          (46, "PCMPEQD XMM5,XMM1"),
          (50, "MOVUPS xmmword ptr [EDI + 0x30],XMM5"),
          (54, "MOVDDUP XMM6,XMM1"),
          (58, "MOVUPS xmmword ptr [EDI + 0x40],XMM6"),
          (62, "MOVAPD XMM7,XMM0"),
          (66, "SHUFPS XMM7,XMM1,0x93"),
          (70, "MOVUPS xmmword ptr [EDI + 0x50],XMM7"),
          (74, "MOVAPD XMM2,XMM0"),
          (78, "MOVHPD XMM2,qword ptr [ESI + 0x8]"),
          (83, "MOVUPS xmmword ptr [EDI + 0x60],XMM2"),
          (87, "MOVLPD qword ptr [EDI + 0x70],XMM1"),
          (92, "RET")],
         "0f 10 06 0f 10 4e 10 66 0f 28 d0 66 0f 62 d1 0f 11 17 66 0f 28 d8 66 0f 6c d9 "
         "0f 11 5f 10 66 0f 28 e0 66 0f ef e1 0f 11 67 20 66 0f 28 e8 66 0f 76 e9 "
         "0f 11 6f 30 f2 0f 12 f1 0f 11 77 40 66 0f 28 f8 0f c6 f9 93 0f 11 7f 50 "
         "66 0f 28 d0 66 0f 16 56 08 0f 11 57 60 66 0f 13 4f 70 c3",
         lambda rng: {"regs": dict(rand_regs(rng), ESI=SCRATCH + 0x300, EDI=SCRATCH + 0x100),
                      "mem": [(SCRATCH + 0x300, bytes(rng.randrange(256) for _ in range(0x20))),
                              (SCRATCH + 0x100, b"\x00" * 0x100)]}),
    # Gothic 3's SharedBase.dll 1001c750, bCBox::Contains: FCOMIP and the
    # flags it sets, branched on with JA and JBE, as the game ships it.
    Case("Gothic 3 bCBox::Contains (FCOMIP)", 0x0D02E600,
         [
          (0, 'MOV EDX,dword ptr [ESP + 0x4]'),
          (4, 'FLD float ptr [EDX]'),
          (6, 'FSTP float ptr [ESP + 0x4]'),
          (10, 'FLD float ptr [ESP + 0x4]'),
          (14, 'FLD float ptr [ECX]'),
          (16, 'FCOMIP ST0,ST1'),
          (18, 'JA 0xd02e63c'),
          (20, 'FLD float ptr [EDX + 0x4]'),
          (23, 'FSTP float ptr [ESP + 0x4]'),
          (27, 'FLD float ptr [ESP + 0x4]'),
          (31, 'FLD float ptr [ECX + 0x4]'),
          (34, 'FCOMIP ST0,ST1'),
          (36, 'JA 0xd02e63a'),
          (38, 'FLD float ptr [EDX + 0x8]'),
          (41, 'FSTP float ptr [ESP + 0x4]'),
          (45, 'FLD float ptr [ESP + 0x4]'),
          (49, 'FLD float ptr [ECX + 0x8]'),
          (52, 'FCOMIP ST0,ST1'),
          (54, 'JBE 0xd02e643'),
          (56, 'FSTP ST0'),
          (58, 'FSTP ST0'),
          (60, 'FSTP ST0'),
          (62, 'XOR AL,AL'),
          (64, 'RET 0x4'),
          (67, 'FLD float ptr [ECX + 0xc]'),
          (70, 'FXCH ST3'),
          (72, 'FCOMIP ST0,ST3'),
          (74, 'FSTP ST2'),
          (76, 'JBE 0xd02e657'),
          (78, 'FSTP ST1'),
          (80, 'XOR AL,AL'),
          (82, 'FSTP ST0'),
          (84, 'RET 0x4'),
          (87, 'FLD float ptr [ECX + 0x10]'),
          (90, 'FXCH'),
          (92, 'FCOMIP ST0,ST1'),
          (94, 'FSTP ST0'),
          (96, 'JA 0xd02e63c'),
          (98, 'FLD float ptr [ECX + 0x14]'),
          (101, 'FXCH'),
          (103, 'FCOMIP ST0,ST1'),
          (105, 'FSTP ST0'),
          (107, 'JA 0xd02e63e'),
          (109, 'MOV AL,0x1'),
          (111, 'RET 0x4')],
         "8b 54 24 04 d9 02 d9 5c 24 04 d9 44 24 04 d9 01 df f1 77 28 d9 42 04 d9 5c 24 04 d9 44 24 04 d9 41 04 df f1 77 14 d9 42 08 d9 5c 24 04 d9 44 24 04 d9 41 08 df f1 76 0b dd d8 dd d8 dd d8 32 c0 c2 04 00 d9 41 0c d9 cb df f3 dd da 76 09 dd d9 32 c0 dd d8 c2 04 00 d9 41 10 d9 c9 df f1 dd d8 77 da d9 41 14 d9 c9 df f1 dd d8 77 d1 b0 01 c2 04 00",
         box_point_setup),
    # Gothic 3's Engine.dll 10299740, a 4x4 matrix product in SSE that its
    # render code calls with the view and projection: the whole function,
    # as the game ships it, run on both engines.
    Case("Gothic 3 SSE matrix product", 0x0D02E400,
         [
          (0, 'PUSH EBP'),
          (1, 'MOV EBP,ESP'),
          (3, 'AND ESP,0xfffffff0'),
          (6, 'SUB ESP,0x20'),
          (9, 'MOVUPS XMM0,xmmword ptr [EAX]'),
          (12, 'MOVUPS XMM7,xmmword ptr [ECX + 0x30]'),
          (16, 'MOVUPS XMM6,xmmword ptr [ECX + 0x10]'),
          (20, 'MOVUPS XMM4,xmmword ptr [ECX]'),
          (23, 'MOVUPS XMM5,xmmword ptr [ECX + 0x20]'),
          (27, 'MOVUPS XMM1,xmmword ptr [EAX + 0x10]'),
          (31, 'MOVUPS XMM2,xmmword ptr [EAX + 0x20]'),
          (35, 'MOVUPS XMM3,xmmword ptr [EAX + 0x30]'),
          (39, 'MOVAPS xmmword ptr [ESP + 0x10],XMM7'),
          (44, 'MOVAPS xmmword ptr [ESP],XMM6'),
          (48, 'MOVAPS XMM7,XMM0'),
          (51, 'SHUFPS XMM7,XMM0,0x55'),
          (55, 'MULPS XMM7,XMM6'),
          (58, 'MOVAPS XMM6,XMM0'),
          (61, 'SHUFPS XMM6,XMM0,0x0'),
          (65, 'MULPS XMM6,XMM4'),
          (68, 'ADDPS XMM7,XMM6'),
          (71, 'MOVAPS XMM6,XMM0'),
          (74, 'SHUFPS XMM6,XMM0,0xaa'),
          (78, 'MULPS XMM6,XMM5'),
          (81, 'ADDPS XMM7,XMM6'),
          (84, 'MOVAPS XMM6,XMM0'),
          (87, 'SHUFPS XMM6,XMM0,0xff'),
          (91, 'MOVAPS XMM0,xmmword ptr [ESP + 0x10]'),
          (96, 'MULPS XMM6,XMM0'),
          (99, 'ADDPS XMM7,XMM6'),
          (102, 'MOVUPS xmmword ptr [EAX],XMM7'),
          (105, 'MOVAPS XMM6,XMM1'),
          (108, 'SHUFPS XMM6,XMM1,0x55'),
          (112, 'MULPS XMM6,xmmword ptr [ESP]'),
          (116, 'MOVAPS XMM7,XMM1'),
          (119, 'SHUFPS XMM7,XMM1,0x0'),
          (123, 'MULPS XMM7,XMM4'),
          (126, 'ADDPS XMM6,XMM7'),
          (129, 'MOVAPS XMM7,XMM1'),
          (132, 'SHUFPS XMM7,XMM1,0xaa'),
          (136, 'MULPS XMM7,XMM5'),
          (139, 'ADDPS XMM6,XMM7'),
          (142, 'MOVAPS XMM7,XMM1'),
          (145, 'SHUFPS XMM7,XMM1,0xff'),
          (149, 'MOVAPS XMM1,xmmword ptr [ESP]'),
          (153, 'MULPS XMM7,XMM0'),
          (156, 'ADDPS XMM6,XMM7'),
          (159, 'MOVUPS xmmword ptr [EAX + 0x10],XMM6'),
          (163, 'MOVAPS XMM6,XMM2'),
          (166, 'SHUFPS XMM6,XMM2,0x55'),
          (170, 'MULPS XMM6,XMM1'),
          (173, 'MOVAPS XMM7,XMM2'),
          (176, 'SHUFPS XMM7,XMM2,0x0'),
          (180, 'MULPS XMM7,XMM4'),
          (183, 'ADDPS XMM6,XMM7'),
          (186, 'MOVAPS XMM7,XMM2'),
          (189, 'SHUFPS XMM7,XMM2,0xaa'),
          (193, 'MULPS XMM7,XMM5'),
          (196, 'ADDPS XMM6,XMM7'),
          (199, 'MOVAPS XMM7,XMM2'),
          (202, 'SHUFPS XMM7,XMM2,0xff'),
          (206, 'MOVAPS XMM2,XMM3'),
          (209, 'SHUFPS XMM2,XMM3,0x55'),
          (213, 'MULPS XMM2,XMM1'),
          (216, 'MOVAPS XMM1,XMM3'),
          (219, 'SHUFPS XMM1,XMM3,0x0'),
          (223, 'MULPS XMM1,XMM4'),
          (226, 'ADDPS XMM2,XMM1'),
          (229, 'MOVAPS XMM1,XMM3'),
          (232, 'SHUFPS XMM1,XMM3,0xaa'),
          (236, 'MULPS XMM1,XMM5'),
          (239, 'ADDPS XMM2,XMM1'),
          (242, 'MOVAPS XMM1,XMM3'),
          (245, 'MULPS XMM7,XMM0'),
          (248, 'SHUFPS XMM1,XMM3,0xff'),
          (252, 'ADDPS XMM6,XMM7'),
          (255, 'MULPS XMM1,XMM0'),
          (258, 'MOVUPS xmmword ptr [EAX + 0x20],XMM6'),
          (262, 'ADDPS XMM2,XMM1'),
          (265, 'MOVUPS xmmword ptr [EAX + 0x30],XMM2'),
          (269, 'MOV ESP,EBP'),
          (271, 'POP EBP'),
          (272, 'RET')],
         "55 8b ec 83 e4 f0 83 ec 20 0f 10 00 0f 10 79 30 0f 10 71 10 0f 10 21 0f 10 69 20 0f 10 48 10 0f 10 50 20 0f 10 58 30 0f 29 7c 24 10 0f 29 34 24 0f 28 f8 0f c6 f8 55 0f 59 fe 0f 28 f0 0f c6 f0 00 0f 59 f4 0f 58 fe 0f 28 f0 0f c6 f0 aa 0f 59 f5 0f 58 fe 0f 28 f0 0f c6 f0 ff 0f 28 44 24 10 0f 59 f0 0f 58 fe 0f 11 38 0f 28 f1 0f c6 f1 55 0f 59 34 24 0f 28 f9 0f c6 f9 00 0f 59 fc 0f 58 f7 0f 28 f9 0f c6 f9 aa 0f 59 fd 0f 58 f7 0f 28 f9 0f c6 f9 ff 0f 28 0c 24 0f 59 f8 0f 58 f7 0f 11 70 10 0f 28 f2 0f c6 f2 55 0f 59 f1 0f 28 fa 0f c6 fa 00 0f 59 fc 0f 58 f7 0f 28 fa 0f c6 fa aa 0f 59 fd 0f 58 f7 0f 28 fa 0f c6 fa ff 0f 28 d3 0f c6 d3 55 0f 59 d1 0f 28 cb 0f c6 cb 00 0f 59 cc 0f 58 d1 0f 28 cb 0f c6 cb aa 0f 59 cd 0f 58 d1 0f 28 cb 0f 59 f8 0f c6 cb ff 0f 58 f7 0f 59 c8 0f 11 70 20 0f 58 d1 0f 11 50 30 8b e5 5d c3",
         matrix_pair_setup),
    # Packed float arithmetic: Gothic 3's engine multiplies matrices with
    # MULPS and ADDPS without asking CPUID first.
    Case("SSE packed float arithmetic, shuffles and comparisons", 0x0D02EC00,
         [(0, "MOVUPS XMM0,xmmword ptr [ESI]"),
          (3, "MOVUPS XMM1,xmmword ptr [ESI + 0x10]"),
          (7, "MOVAPS XMM2,XMM0"), (10, "ADDPS XMM2,XMM1"),
          (13, "MOVUPS xmmword ptr [EDI],XMM2"),
          (16, "MOVAPS XMM3,XMM0"), (19, "SUBPS XMM3,XMM1"),
          (22, "MOVUPS xmmword ptr [EDI + 0x10],XMM3"),
          (26, "MOVAPS XMM4,XMM0"), (29, "MULPS XMM4,XMM1"),
          (32, "MOVUPS xmmword ptr [EDI + 0x20],XMM4"),
          (36, "MOVAPS XMM5,XMM0"), (39, "DIVPS XMM5,XMM1"),
          (42, "MOVUPS xmmword ptr [EDI + 0x30],XMM5"),
          (46, "MOVAPS XMM6,XMM0"), (49, "MINPS XMM6,XMM1"),
          (52, "MOVUPS xmmword ptr [EDI + 0x40],XMM6"),
          (56, "MOVAPS XMM7,XMM0"), (59, "MAXPS XMM7,XMM1"),
          (62, "MOVUPS xmmword ptr [EDI + 0x50],XMM7"),
          (66, "MOVAPS XMM2,XMM0"), (69, "MULPS XMM2,xmmword ptr [ESI + 0x10]"),
          (73, "MOVUPS xmmword ptr [EDI + 0x60],XMM2"),
          (77, "MOVAPS XMM3,XMM0"), (80, "MOVHLPS XMM3,XMM1"),
          (83, "MOVUPS xmmword ptr [EDI + 0x70],XMM3"),
          (87, "MOVAPS XMM4,XMM0"), (90, "MOVLHPS XMM4,XMM1"),
          (93, "MOVUPS xmmword ptr [EDI + 0x80],XMM4"),
          (100, "MOVAPS XMM5,XMM0"), (103, "UNPCKLPS XMM5,XMM1"),
          (106, "MOVUPS xmmword ptr [EDI + 0x90],XMM5"),
          (113, "MOVAPS XMM6,XMM0"), (116, "UNPCKHPS XMM6,XMM1"),
          (119, "MOVUPS xmmword ptr [EDI + 0xa0],XMM6"),
          (126, "MOVAPS XMM7,XMM0"), (129, "CMPLTPS XMM7,XMM1"),
          (133, "MOVUPS xmmword ptr [EDI + 0xb0],XMM7"),
          (140, "MOVAPS XMM2,XMM0"), (143, "MULPD XMM2,XMM1"),
          (147, "MOVUPS xmmword ptr [EDI + 0xc0],XMM2"),
          (154, "MOVAPS XMM3,XMM0"), (157, "CMPNLESD XMM3,XMM1"),
          (162, "MOVUPS xmmword ptr [EDI + 0xd0],XMM3"),
          (169, "RET")],
         "0f 10 06 0f 10 4e 10 0f 28 d0 0f 58 d1 0f 11 17 0f 28 d8 0f 5c d9 0f 11 5f 10 "
         "0f 28 e0 0f 59 e1 0f 11 67 20 0f 28 e8 0f 5e e9 0f 11 6f 30 0f 28 f0 0f 5d f1 "
         "0f 11 77 40 0f 28 f8 0f 5f f9 0f 11 7f 50 0f 28 d0 0f 59 56 10 0f 11 57 60 "
         "0f 28 d8 0f 12 d9 0f 11 5f 70 0f 28 e0 0f 16 e1 0f 11 a7 80 00 00 00 "
         "0f 28 e8 0f 14 e9 0f 11 af 90 00 00 00 0f 28 f0 0f 15 f1 0f 11 b7 a0 00 00 00 "
         "0f 28 f8 0f c2 f9 01 0f 11 bf b0 00 00 00 0f 28 d0 66 0f 59 d1 0f 11 97 c0 00 00 00 "
         "0f 28 d8 f2 0f c2 d9 06 0f 11 9f d0 00 00 00 c3",
         sse_packed_setup),
    # The scalar double forms the same runtimes use for ordinary arithmetic,
    # and the one SSE form that writes the guest's flags.
    Case("SSE2 scalar double arithmetic, conversions and COMISD", 0x0D02FC00,
         [(0, "MOVSD XMM0,qword ptr [ESI]"),
          (4, "MOVSD XMM1,qword ptr [ESI + 0x8]"),
          (9, "MOVAPD XMM2,XMM0"),
          (13, "ADDSD XMM2,XMM1"),
          (17, "MOVSD qword ptr [EDI],XMM2"),
          (21, "MOVAPD XMM2,XMM0"),
          (25, "SUBSD XMM2,XMM1"),
          (29, "MOVSD qword ptr [EDI + 0x8],XMM2"),
          (34, "MOVAPD XMM2,XMM0"),
          (38, "MULSD XMM2,XMM1"),
          (42, "MOVSD qword ptr [EDI + 0x10],XMM2"),
          (47, "MOVAPD XMM2,XMM0"),
          (51, "DIVSD XMM2,XMM1"),
          (55, "MOVSD qword ptr [EDI + 0x18],XMM2"),
          (60, "SQRTSD XMM3,XMM1"),
          (64, "MOVSD qword ptr [EDI + 0x20],XMM3"),
          (69, "CVTSI2SD XMM4,EDX"),
          (73, "MOVSD qword ptr [EDI + 0x28],XMM4"),
          (78, "CVTTSD2SI EAX,XMM0"),
          (82, "CVTSD2SS XMM5,XMM0"),
          (86, "MOVSS dword ptr [EDI + 0x30],XMM5"),
          (91, "CVTSS2SD XMM6,XMM5"),
          (95, "MOVSD qword ptr [EDI + 0x38],XMM6"),
          (100, "COMISD XMM0,XMM1"),
          (104, "RET")],
         "f2 0f 10 06 f2 0f 10 4e 08 66 0f 28 d0 f2 0f 58 d1 f2 0f 11 17 66 0f 28 d0 "
         "f2 0f 5c d1 f2 0f 11 57 08 66 0f 28 d0 f2 0f 59 d1 f2 0f 11 57 10 66 0f 28 d0 "
         "f2 0f 5e d1 f2 0f 11 57 18 f2 0f 51 d9 f2 0f 11 5f 20 f2 0f 2a e2 f2 0f 11 67 28 "
         "f2 0f 2c c0 f2 0f 5a e8 f3 0f 11 6f 30 f3 0f 5a f5 f2 0f 11 77 38 66 0f 2f c1 c3",
         sse_scalar_setup),
    # A conversion's other operand is as often memory or a general register
    # as it is an XMM one, CWDE turns up in the same runtimes, and a guest
    # that reads MXCSR gets the one rounding mode this kit models.
    Case("SSE conversions through memory, CWDE and MXCSR", 0x0D02F000 + 0x600,
         [(0, "CVTTSD2SI EAX,qword ptr [ESI]"),
          (4, "MOV dword ptr [EDI],EAX"),
          (6, "CVTSI2SD XMM0,dword ptr [ESI + 0x10]"),
          (11, "MOVSD qword ptr [EDI + 0x8],XMM0"),
          (16, "MOVSX ECX,AX"),
          (19, "CWDE"),
          (20, "MOV dword ptr [EDI + 0x10],EAX"),
          (23, "STMXCSR dword ptr [EDI + 0x18]"),
          (27, "RET")],
         "f2 0f 2c 06 89 07 f2 0f 2a 46 10 f2 0f 11 47 08 0f bf c8 98 89 47 10 0f ae 5f 18 c3",
         lambda rng: {"regs": dict(rand_regs(rng), ESI=SCRATCH + 0x300, EDI=SCRATCH + 0x100),
                      "mem": [(SCRATCH + 0x300,
                               struct.pack("<d", rng.uniform(-1e6, 1e6))
                               + struct.pack("<q", 0)
                               + struct.pack("<i", rng.randrange(-100000, 100000))),
                              (SCRATCH + 0x100, b"\x00" * 0x40)]}),
    # The single precision truncation reads a float, from an XMM register or
    # memory; read as a double, a float's bits make a denormal and every
    # conversion came out 0 (Engine CalculateRectIndices 1040a690).
    Case("CVTTSS2SI from an XMM register and from memory", 0x0D02F000 + 0x700,
         [(0, "MOVSS XMM0,dword ptr [ESI]"),
          (4, "CVTTSS2SI EAX,XMM0"),
          (8, "MOV dword ptr [EDI],EAX"),
          (10, "CVTTSS2SI ECX,dword ptr [ESI + 0x4]"),
          (15, "MOV dword ptr [EDI + 0x4],ECX"),
          (18, "RET")],
         "f3 0f 10 06 f3 0f 2c c0 89 07 f3 0f 2c 4e 04 89 4f 04 c3",
         lambda rng: {"regs": dict(rand_regs(rng), ESI=SCRATCH + 0x300, EDI=SCRATCH + 0x100),
                      "mem": [(SCRATCH + 0x300,
                               struct.pack("<ff", rng.uniform(-1e6, 1e6), rng.uniform(-1e6, 1e6))
                               + struct.pack("<q", 0)),
                              (SCRATCH + 0x100, b"\x00" * 0x10)]}),
    Case("Port string forms store the port read and advance", 0x0D02D000,
         [(0, "INSD ES:EDI,DX"), (1, "INSD.REP ES:EDI,DX"), (3, "OUTSD ESI,DX"),
          (4, "RET")],
         "6d f3 6d 6f c3",
         lambda rng: {"regs": dict(rand_regs(rng), EDI=SCRATCH + 0x100, ESI=SCRATCH + 0x200,
                                   ECX=2, EDX=0x3f8),
                      "mem": [(SCRATCH + 0x100, b"\xaa" * 32),
                              (SCRATCH + 0x200, struct.pack("<I", 0x11223344))]}),
    Case("Vtable adapter RET enters the method with Self and the caller return", 0x0D029000,
         [(0, "ADD EAX,-0x8"), (3, "PUSH EAX"), (4, "MOV EAX,dword ptr [EAX]"),
          (6, "MOV EAX,dword ptr [EAX + 0x8]"), (9, "XCHG dword ptr [ESP],EAX"),
          (12, "RET"), (13, "MOV EAX,0x12345678"), (18, "RET")],
         "83 c0 f8 50 8b 00 8b 40 08 87 04 24 c3 b8 78 56 34 12 c3",
         lambda rng: {"regs": rand_regs(rng, EAX=SCRATCH + 0x108),
                      "mem": [(SCRATCH + 0x100, struct.pack("<I", SCRATCH + 0x200)),
                              (SCRATCH + 0x208, struct.pack("<I", 0x0D02900D))]},
         entries=(0x0D02900D,)),
    Case("Cleanup RET follows a nonadjacent pushed continuation", 0x0D01EC00,
         [(0, "PUSH EBP"), (1, "MOV EBP,ESP"), (3, "PUSH 0x0d01ec11"),
          (8, "LEA EAX,[EBP]"), (11, "MOV EDX,0x3"),
          (16, "RET"), (17, "POP EBP"), (18, "RET")],
         "55 89 e5 68 11 ec 01 0d 8d 45 00 ba 03 00 00 00 c3 5d c3",
         lambda rng: {"regs": rand_regs(rng)}),
    Case("Cleanup RET immediate adjusts ESP before the continuation", 0x0D01ED00,
         [(0, "PUSH 0x1234"), (5, "PUSH 0x0d01ed0e"), (10, "NOP"),
          (11, "RET 0x4"), (14, "INC EAX"), (15, "RET 0x8")],
         "68 34 12 00 00 68 0e ed 01 0d 90 c2 04 00 40 c2 08 00",
         lambda rng: {"regs": rand_regs(rng), "args": [11, 22]}),
    Case("Variable argument cleanup returns through a popped address", 0x0D01EA00,
         [(0, "POP EAX"), (1, "LEA ESP,[ESP + EDX*0x4]"), (4, "JMP EAX")],
         "58 8d 24 94 ff e0",
         lambda rng: {"regs": rand_regs(rng, EDX=3), "args": [11, 22, 33]}),
    Case("Popped EDX returns after removing two arguments", 0x0D01EB00,
         [(0, "POP EDX"), (1, "ADD ESP,0x8"), (4, "JMP EDX")],
         "5a 83 c4 08 ff e2",
         lambda rng: {"regs": rand_regs(rng), "args": [11, 22]}),
    Case("PUSH immediate RET reaches the epilogue before returning", 0x0D01E800,
         [(0, "PUSH EBP"), (1, "MOV EBP,ESP"),
          (3, "PUSH 0x0d01e809"), (8, "RET"), (9, "POP EBP"), (10, "RET")],
         "55 89 e5 68 09 e8 01 0d c3 5d c3"),
    Case("A branch to the shared RET returns to its own caller", 0x0D01E900,
         [(0, "JMP 0x0d01e90a"), (5, "PUSH 0x0d01e90b"),
          (10, "RET"), (11, "INC EAX"), (12, "RET")],
         "e9 05 00 00 00 68 0b e9 01 0d c3 40 c3"),
    Case("LOOP counts ECX down and branches while it is not zero", 0x0D010000,
         [(0x0, "XOR EAX,EAX"), (0x2, "INC EAX"), (0x3, "LOOP 0x0d010002"), (0x5, "RET")],
         "31 C0  40  E2 FD  C3", loop_setup),
    Case("INT3 is a breakpoint the runtime notes and steps over", 0x0D011000,
         [(0x0, "INT3"), (0x1, "MOV EAX,0x1234"), (0x6, "RET")],
         "CC  B8 34 12 00 00  C3", expect_intr=3),
    Case("STC and CLC set and clear the carry", 0x0D012000,
         [(0x0, "STC"), (0x1, "SETC AL"), (0x4, "CLC"), (0x5, "SETC BL"), (0x8, "RET")],
         "F9  0F 92 C0  F8  0F 92 C3  C3", lambda rng: {"regs": rand_regs(rng)}),
    Case("REPE CMPSD compares dwords until they differ", 0x0D013000,
         [(0x0, "CMPSD.REPE ES:EDI,ESI"), (0x2, "RET")],
         "F3 A7  C3", cmps_setup),
    Case("REPNE CMPSD compares dwords until they match", 0x0D014000,
         [(0x0, "CMPSD.REPNE ES:EDI,ESI"), (0x2, "RET")],
         "F2 A7  C3", cmps_setup),
    Case("CMPSW compares one word", 0x0D015000,
         [(0x0, "CMPSW ES:EDI,ESI"), (0x2, "RET")],
         "66 A7  C3", cmpsw_setup),
    Case("REPE CMPSW compares words until they differ", 0x0D016000,
         [(0x0, "CMPSW.REPE ES:EDI,ESI"), (0x3, "RET")],
         "F3 66 A7  C3", cmpsw_setup),
    Case("REPNE SCASD scans dwords for EAX", 0x0D017000,
         [(0x0, "SCASD.REPNE ES:EDI,EAX"), (0x2, "RET")],
         "F2 AF  C3", lambda rng: scas_setup(rng, 4)),
    Case("REPE SCASW scans words while they equal AX", 0x0D018000,
         [(0x0, "SCASW.REPE ES:EDI,AX"), (0x3, "RET")],
         "F3 66 AF  C3", lambda rng: scas_setup(rng, 2)),
    Case("PUSH and POP of 16-bit registers move ESP by two", 0x0D019000,
         [(0x0, "PUSH AX"), (0x2, "PUSH CX"), (0x4, "POP DX"), (0x6, "POP BX"), (0x8, "RET")],
         "66 50  66 51  66 5A  66 5B  C3", lambda rng: {"regs": rand_regs(rng)}),
    Case("MOV to memory from CS stores the code selector", 0x0D01A000,
         [(0x0, "MOV word ptr [ECX],CS"), (0x2, "RET")],
         "8C 09  C3", seg_store_setup, ignore=((SCRATCH + 0x40, 2),), check=check_seg_store),
    Case("FPTAN replaces ST(0) with its tangent and pushes 1.0", 0x0D01B000,
         [(0x0, "FLD double ptr [ESP + 0x4]"), (0x4, "FPTAN"),
          (0x6, "FSTP double ptr [ESP + 0xc]"), (0xa, "FSTP double ptr [ESP + 0x14]"),
          (0xe, "RET")],
         "DD 44 24 04  D9 F2  DD 5C 24 0C  DD 5C 24 14  C3", fptan_setup, check=check_fptan),
    Case("FSAVE writes the 108-byte state and reinitialises; FRSTOR brings it back", 0x0D01C000,
         [(0x0, "FLD1"), (0x2, "FLD double ptr [ESP + 0x4]"), (0x6, "FSAVE [ECX + 0x8]"),
          (0xa, "FLD1"), (0xc, "FSTP double ptr [ESP + 0xc]"),
          (0x10, "FRSTOR [ECX + 0x8]"), (0x13, "FSTP double ptr [ESP + 0x14]"),
          (0x17, "FSTP double ptr [ESP + 0x1c]"), (0x1b, "RET")],
         "D9 E8  DD 44 24 04  9B DD 71 08  D9 E8  DD 5C 24 0C  DD 61 08  DD 5C 24 14  DD 5C 24 1C  C3",
         fsave_setup, ignore=((SCRATCH + 0x1000 + 8 + 12, 16),), check=check_fsave),
    Case("PUSHF and POPF move the low 16 flag bits through the stack", 0x0D01E000,
         [(0x0, "STC"), (0x1, "PUSHF"), (0x3, "POP AX"), (0x5, "CLC"), (0x6, "PUSH AX"),
          (0x8, "POPF"), (0xa, "SETC BL"), (0xd, "RET")],
         "F9  66 9C  66 58  F8  66 50  66 9D  0F 92 C3  C3", lambda rng: {"regs": rand_regs(rng)}),
    Case("FNINIT empties the stack and resets the control word", 0x0D01D000,
         [(0x0, "FLD1"), (0x2, "FNINIT"), (0x4, "FNSTCW word ptr [ESP + 0x4]"),
          (0x8, "FLD1"), (0xa, "FSTP double ptr [ESP + 0xc]"), (0xe, "RET")],
         "D9 E8  DB E3  D9 7C 24 04  D9 E8  DD 5C 24 0C  C3",
         lambda rng: {"args": [0, 0, 0, 0, 0, 0]}),
    Case("Computed address enters an unrolled fill", 0x0D01EE00,
         [(0, "MOV CH,CL"), (2, "TEST EDX,EDX"), (4, "JLE 0xd01ee28"),
          (6, "MOV byte ptr [EDX + EAX*0x1 + -0x1],CL"), (10, "AND EDX,0xfffffffe"),
          (13, "NEG EDX"), (15, "LEA EDX,[EDX*0x2 + 0xd01ee28]"),
          (22, "JMP EDX"), (24, "MOV word ptr [EAX + 0x6],CX"),
          (28, "MOV word ptr [EAX + 0x4],CX"), (32, "MOV word ptr [EAX + 0x2],CX"),
          (36, "MOV word ptr [EAX],CX"), (39, "RET"), (40, "RET")],
         "88cd 85d2 7e22 884c02ff 83e2fe f7da 8d1455 28ee010d "
         "ffe2 66894806 66894804 66894802 668908 c3 c3",
         lambda rng: {"regs": rand_regs(rng, EAX=SCRATCH, ECX=rng.randrange(256),
                                        EDX=rng.randrange(9))}),
    Case("Scaled LEA selects each unrolled store entry", 0x0D01EF00,
         [(0, "LEA EAX,[ECX*0x4 + 0xd01ef09]"), (7, "JMP EAX"),
          (9, "MOV word ptr [EDI + 0x6],DX"), (13, "MOV word ptr [EDI + 0x4],DX"),
          (17, "MOV word ptr [EDI + 0x2],DX"), (21, "MOV word ptr [EDI],DX"), (25, "RET")],
         "8d048d 09ef010d ffe0 66895706 66895704 66895702 66895700 c3",
         lambda rng: {"regs": rand_regs(rng, EDI=SCRATCH, ECX=rng.randrange(4))}),
    Case("CMPXCHG stores when EAX equals the destination and loads EAX otherwise", 0x0D01F000,
         [(0x0, "CMPXCHG.LOCK dword ptr [ECX],EDX"), (0x4, "SETZ BL"), (0x7, "RET")],
         "F0 0F B1 11  0F 94 C3  C3", cmpxchg_setup),
    Case("XADD exchanges and adds", 0x0D020000,
         [(0x0, "XADD.LOCK dword ptr [ECX],EDX"), (0x4, "RET")],
         "F0 0F C1 11  C3",
         lambda rng: {"regs": rand_regs(rng, ECX=SCRATCH + 0x40),
                      "mem": [(SCRATCH + 0x40, struct.pack("<I", rng.getrandbits(32)))]}),
    # Session 16: the atomic forms (x86.h lk_*) against Unicorn, including counters that wrap.
    Case("LOCK INC adds one atomically and sets ZF/SF/OF but not CF", 0x0D02F100,
         [(0x0, "INC.LOCK dword ptr [ECX]"), (0x3, "SETZ BL"), (0x6, "RET")],
         "F0 FF 01  0F 94 C3  C3",
         lambda rng: {"regs": rand_regs(rng, ECX=SCRATCH + 0x40),
                      "mem": [(SCRATCH + 0x40, struct.pack("<I", rng.choice(
                          [0xffffffff, 0x7fffffff, 0, rng.getrandbits(32)])))]}),
    Case("LOCK DEC subtracts one atomically (a reference count reaching zero)", 0x0D02F200,
         [(0x0, "DEC.LOCK dword ptr [ECX]"), (0x3, "SETZ BL"), (0x6, "RET")],
         "F0 FF 09  0F 94 C3  C3",
         lambda rng: {"regs": rand_regs(rng, ECX=SCRATCH + 0x40),
                      "mem": [(SCRATCH + 0x40, struct.pack("<I", rng.choice(
                          [1, 0, 0x80000000, rng.getrandbits(32)])))]}),
    Case("LOCK BTS sets a bit atomically, the old bit in CF, the index past the dword", 0x0D02F300,
         [(0x0, "BTS.LOCK dword ptr [ECX],EDX"), (0x4, "SETC BL"), (0x7, "RET")],
         "F0 0F AB 11  0F 92 C3  C3",
         lambda rng: {"regs": rand_regs(rng, ECX=SCRATCH + 0x40, EDX=rng.randrange(64)),
                      "mem": [(SCRATCH + 0x40, struct.pack("<II", rng.getrandbits(32), rng.getrandbits(32)))]}),
    Case("LOCK BTR clears a bit atomically, the old bit in CF", 0x0D02F400,
         [(0x0, "BTR.LOCK dword ptr [ECX],EDX"), (0x4, "SETC BL"), (0x7, "RET")],
         "F0 0F B3 11  0F 92 C3  C3",
         lambda rng: {"regs": rand_regs(rng, ECX=SCRATCH + 0x40, EDX=rng.randrange(64)),
                      "mem": [(SCRATCH + 0x40, struct.pack("<II", rng.getrandbits(32), rng.getrandbits(32)))]}),
    Case("XCHG with memory swaps (locked without the prefix)", 0x0D02F500,
         [(0x0, "XCHG dword ptr [ECX],EDX"), (0x2, "RET")],
         "87 11  C3",
         lambda rng: {"regs": rand_regs(rng, ECX=SCRATCH + 0x40),
                      "mem": [(SCRATCH + 0x40, struct.pack("<I", rng.getrandbits(32)))]}),
    Case("PAUSE is a hint and changes nothing", 0x0D021000,
         [(0x0, "PAUSE"), (0x2, "RET")],
         "F3 90  C3", lambda rng: {"regs": rand_regs(rng)}),
    Case("CMC complements the carry", 0x0D022000,
         [(0x0, "STC"), (0x1, "CMC"), (0x2, "SETC BL"), (0x5, "CMC"),
          (0x6, "SETC CL"), (0x9, "RET")],
         "F9  F5  0F 92 C3  F5  0F 92 C1  C3", lambda rng: {"regs": rand_regs(rng)}),
    Case("STMXCSR stores the default MXCSR", 0x0D023000,
         [(0x0, "STMXCSR dword ptr [ECX]"), (0x3, "RET")],
         "0F AE 19  C3", lambda rng: {"regs": rand_regs(rng, ECX=SCRATCH + 0x80)}),
    Case("FLDLN2 and FLDL2E push the x87 constants", 0x0D024000,
         [(0x0, "FLDLN2"), (0x2, "FLDL2E"), (0x4, "FSTP double ptr [ECX]"),
          (0x6, "FSTP double ptr [ECX + 0x8]"), (0x9, "RET")],
         "D9 ED  D9 EA  DD 19  DD 59 08  C3", lambda rng: {"regs": rand_regs(rng, ECX=SCRATCH + 0x100)},
         ignore=((SCRATCH + 0x100, 16),), check=check_x87_constants),
    Case("FCLEX clears the status word like FNCLEX", 0x0D025000,
         [(0x0, "FLDZ"), (0x2, "FLDZ"), (0x4, "FDIVP ST1,ST0"), (0x6, "FCLEX"),
          (0x9, "FNSTSW word ptr [ECX]"), (0xb, "FSTP double ptr [ECX + 0x8]"), (0xe, "RET")],
         "D9 EE  D9 EE  DE F9  9B DB E2  DD 39  DD 59 08  C3",
         lambda rng: {"regs": rand_regs(rng, ECX=SCRATCH + 0x100)}, ignore=((SCRATCH + 0x108, 8),)),
    Case("FBSTP stores packed BCD and pops", 0x0D026000,
         [(0x0, "FILD dword ptr [ECX + 0x10]"), (0x3, "FBSTP tword ptr [ECX]"), (0x5, "RET")],
         "DB 41 10  DF 31  C3", fbstp_setup),
    Case("CMPXCHG8B compares EDX:EAX and stores ECX:EBX changing only ZF", 0x0D027000,
         # Materialize CMP's flags: Unicorn otherwise corrupts its lazy
         # flag path through CMPXCHG8B. PUSHFD after it also checks preserved AF.
         [(0x0, "CMP ESI,EBP"), (0x2, "PUSHFD"), (0x3, "POPFD"),
          (0x4, "CMPXCHG8B.LOCK qword ptr [EDI]"),
          (0x8, "PUSHFD"), (0x9, "POP ESI"), (0xa, "RET")],
         "39 EE  9C  9D  F0 0F C7 0F  9C  5E  C3", cmpxchg8b_setup),
    Case("EMMS changes no integer registers or flags", 0x0D028000,
         [(0x0, "CMP ESI,EBP"), (0x2, "EMMS"), (0x4, "RET")],
         "39 EE  0F 77  C3", lambda rng: {"regs": rand_regs(rng)}),
]


def integer_copy_setup(rng, value=None):
    return {"regs": rand_regs(rng, EAX=SCRATCH, EDX=SCRATCH + 64),
            "mem": [(SCRATCH, struct.pack("<Q", rng.getrandbits(64) if value is None else value))]}


# x87 has enough mantissa bits to copy any signed qword without rounding.
# Include fixed pointer/string-like patterns and a fresh random value each run.
CASES += [
    Case("FILD FISTP qword exact copy %s" % name, 0x0D02A000 + i * 0x100,
         [(0, "FILD qword ptr [EAX]"), (2, "FISTP qword ptr [EDX]"), (4, "RET")],
         "df28 df3a c3", lambda rng, value=value: integer_copy_setup(rng, value))
    for i, (name, value) in enumerate([
        ("pattern", 0x0102030405060708), ("minus one", 0xffffffffffffffff),
        ("near minimum", 0x8000000000000001), ("random", None),
        ("UTF16", int.from_bytes("Out ".encode("utf-16le"), "little"))])
]
CASES += [
    Case("FILD FISTP copies an overlapping 26-byte resource string", 0x0D02B000,
         [(0, "FILD qword ptr [EAX + 0x12]"), (3, "FILD qword ptr [EAX]"),
          (5, "FILD qword ptr [EAX + 0x8]"), (8, "FILD qword ptr [EAX + 0x10]"),
          (11, "FISTP qword ptr [EDX + 0x10]"), (14, "FISTP qword ptr [EDX + 0x8]"),
          (17, "FISTP qword ptr [EDX]"), (19, "FISTP qword ptr [EDX + 0x12]"), (22, "RET")],
         "df68 12 df28 df68 08 df68 10 df7a 10 df7a 08 df3a df7a 12 c3",
         lambda rng: {"regs": rand_regs(rng, EAX=SCRATCH, EDX=SCRATCH + 64),
                      "mem": [(SCRATCH, "Out of memory".encode("utf-16le"))]}),
    Case("FILD FXCH preserves both exact integers", 0x0D02B100,
         [(0, "FILD qword ptr [EAX]"), (2, "FILD qword ptr [EAX + 0x8]"),
          (5, "FXCH ST1"), (7, "FISTP qword ptr [EDX]"),
          (9, "FISTP qword ptr [EDX + 0x8]"), (12, "RET")],
         "df28 df6808 d9c9 df3a df7a08 c3",
         lambda rng: {"regs": rand_regs(rng, EAX=SCRATCH, EDX=SCRATCH + 64),
                      "mem": [(SCRATCH, struct.pack("<QQ", rng.getrandbits(64), rng.getrandbits(64)))]}),
    Case("FILD FLD ST duplicates exact integer metadata", 0x0D02B200,
         [(0, "FILD qword ptr [EAX]"), (2, "FLD ST0"),
          (4, "FISTP qword ptr [EDX]"), (6, "FISTP qword ptr [EDX + 0x8]"), (9, "RET")],
         "df28 d9c0 df3a df7a08 c3", integer_copy_setup),
    Case("FILD FST ST copies exact integer metadata", 0x0D02B300,
         [(0, "FLDZ"), (2, "FILD qword ptr [EAX]"), (4, "FST ST1"),
          (6, "FISTP qword ptr [EDX]"), (8, "FISTP qword ptr [EDX + 0x8]"), (11, "RET")],
         "d9ee df28 ddd1 df3a df7a08 c3", integer_copy_setup),
    Case("FILD FSTP ST copies exact integer metadata", 0x0D02B400,
         [(0, "FLDZ"), (2, "FILD qword ptr [EAX]"), (4, "FSTP ST1"),
          (6, "FISTP qword ptr [EDX]"), (8, "RET")],
         "d9ee df28 ddd9 df3a c3", integer_copy_setup),
    Case("FILD arithmetic clears exact integer metadata", 0x0D02B500,
         [(0, "FILD qword ptr [EAX]"), (2, "FADD ST0,ST0"),
          (4, "FISTP qword ptr [EDX]"), (6, "RET")],
         "df28 d8c0 df3a c3", lambda rng: integer_copy_setup(rng, 1234567)),
    Case("FILD floating register copy clears stale integer metadata", 0x0D02B600,
         [(0, "FILD qword ptr [EAX]"), (2, "FLD1"), (4, "FSTP ST1"),
          (6, "FISTP qword ptr [EDX]"), (8, "RET")],
         "df28 d9e8 ddd9 df3a c3", integer_copy_setup),
    Case("FILD reused stack slot clears exact integer metadata", 0x0D02B700,
         [(0, "FILD qword ptr [EAX]"), (2, "FISTP qword ptr [EDX]"),
          (4, "FLD1"), (6, "FISTP qword ptr [EDX + 0x8]"), (9, "RET")],
         "df28 df3a d9e8 df7a08 c3", integer_copy_setup),
]
CASES += [
    Case("FILD %s sign extends into qword" % size, 0x0D02C000 + i * 0x100,
         [(0, "FILD %s ptr [EAX]" % size), (2, "FISTP qword ptr [EDX]"), (4, "RET")],
         opcode + " df3a c3", integer_copy_setup)
    for i, (size, opcode) in enumerate([("word", "df00"), ("dword", "db00")])
]
CASES += [
    Case("FILD qword narrows to %s %s" % (size, name), 0x0D02C200 + i * 0x100,
         [(0, "FILD qword ptr [EAX]"), (2, "FIST %s ptr [EDX]" % size),
          (4, "FISTP %s ptr [EDX + 0x8]" % size), (7, "RET")],
         "df28 " + store + " " + pop + "08 c3",
         lambda rng, value=value: integer_copy_setup(rng, value & 0xffffffffffffffff))
    for i, (size, store, pop, name, value) in enumerate([
        ("dword", "db12", "db5a", "in range", -123456789),
        ("dword", "db12", "db5a", "overflow", 0x80000000),
        ("word", "df12", "df5a", "in range", -12345),
        ("word", "df12", "df5a", "overflow", 0x8000)])
]


CASES += [
    Case("%s short register form writes %s" % (name, dest),
         0x0D02D100 + i * 0x100,
         [(0, "FLD double ptr [EAX]"), (2, "FLD double ptr [EAX + 0x8]"),
          (5, name + " ST1"), (5 + len(bytes.fromhex(code)), "FSTP double ptr [EDX]"),
          (7 + len(bytes.fromhex(code)), "FSTP double ptr [EDX + 0x8]"),
          (10 + len(bytes.fromhex(code)), "RET")],
         "dd00 dd4008 " + code + " dd1a dd5a08 c3",
         lambda rng: {"regs": rand_regs(rng, EAX=SCRATCH, EDX=SCRATCH + 0x100),
                      "mem": [(SCRATCH, struct.pack("<2d", 8.0, 2.0))]})
    for i, (name, code, dest) in enumerate([
        ("FADD", "d8c1", "ST0"), ("FADD", "dcc1", "ST1"),
        ("FMUL", "d8c9", "ST0"), ("FMUL", "dcc9", "ST1"),
        ("FSUB", "d8e1", "ST0"), ("FSUB", "dce9", "ST1"),
        ("FSUBR", "d8e9", "ST0"), ("FSUBR", "dce1", "ST1"),
        ("FDIV", "d8f1", "ST0"), ("FDIV", "dcf9", "ST1"),
        ("FDIVR", "d8f9", "ST0"), ("FDIVR", "dcf1", "ST1"),
        ("FMUL", "66dcc9", "ST1 with prefix")])
]


def recovered_lines(addr, code):
    """The listing block recovery builds from capstone, for code Ghidra left
    unlisted: capstone's spellings, normalised by Image.to_insn."""
    import capstone
    image = object.__new__(T.Image)
    image.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    image.md.detail = True
    return [(ci.address - addr, image.to_insn(ci).raw.split("  ", 1)[1])
            for ci in image.md.disasm(bytes.fromhex(code.replace(" ", "")), addr)]


def float_key_compare_setup(rng):
    """Two qsort elements, each a pointer to an object with its float key at
    +0x18: less, equal or greater."""
    a = rng.uniform(-100.0, 100.0)
    b = rng.choice([a, a + rng.uniform(0.5, 10.0), a - rng.uniform(0.5, 10.0)])
    return {"regs": rand_regs(rng),
            "mem": [(SCRATCH, struct.pack("<2I", SCRATCH + 0x100, SCRATCH + 0x200)),
                    (SCRATCH + 0x118, struct.pack("<f", a)),
                    (SCRATCH + 0x218, struct.pack("<f", b))],
            "args": [SCRATCH, SCRATCH + 4]}


# Gothic 3's Engine.dll 103fd1c0, a qsort comparator on the float at +0x18
# (called from 10564e62). Ghidra left it unlisted, so it is translated from
# capstone, which spells FCOMIP "fcompi": the entry point was rejected as
# "unhandled mnemonic FCOMPI" and the game called an unknown target.
FLOAT_KEY_COMPARE = ("8b 44 24 04 8b 00 8b 4c 24 08 d9 40 18 8b 09 d9 5c 24 04 d9 41 18 "
                     "d9 5c 24 08 d9 44 24 08 d9 44 24 04 db f1 76 08 dd d8 83 c8 ff "
                     "dd d8 c3 d9 c9 df f1 dd d8 76 06 b8 01 00 00 00 c3 33 c0 c3")
CASES += [
    Case("Gothic 3 Engine 103fd1c0 float key compare (capstone FCOMPI)", 0x0D02FE00,
         recovered_lines(0x0D02FE00, FLOAT_KEY_COMPARE), FLOAT_KEY_COMPARE,
         float_key_compare_setup),
]


def rebased(lines, old, new, size):
    """Listing lines of a function at `old`, moved to `new`: branch targets
    inside the function follow it."""
    def move(m):
        v = int(m.group(0), 16)
        return "0x%x" % (v - old + new) if old <= v < old + size else m.group(0)
    return [(off, re.sub(r"0x[0-9a-f]{8}", move, text)) for off, text in lines]


def processing_range_setup(rng):
    """eCEntityAdmin's processing range box at ECX+0x64 (min) and +0x70 (max)
    and an entity box argument that overlaps it, touches it or misses it."""
    lo = [rng.uniform(-5000.0, 0.0) for _ in range(3)]
    hi = [x + rng.uniform(100.0, 8000.0) for x in lo]
    elo = [rng.uniform(a - 3000.0, b + 1000.0) for a, b in zip(lo, hi)]
    ehi = [x + rng.uniform(1.0, 2000.0) for x in elo]
    if rng.random() < 0.2:
        axis = rng.randrange(3)
        elo[axis] = hi[axis]
    return {"regs": dict(rand_regs(rng), ECX=SCRATCH + 0x300),
            "mem": [(SCRATCH + 0x364, struct.pack("<6f", *lo, *hi)),
                    (SCRATCH + 0x500, struct.pack("<6f", *elo, *ehi))],
            "args": [SCRATCH + 0x500]}


def frustum_box_setup(rng):
    """eCFrustumFilter: six planes (a, b, c, d) at ECX+0x60, per plane three
    corner indices at +0xc0 and +0xd2 (the corners the filter picks by the
    normal's signs); a box argument (min then max corner) inside, across or
    outside."""
    planes, near, far = [], [], []
    for _ in range(6):
        n = [rng.uniform(-1.0, 1.0) for _ in range(3)]
        planes += n + [rng.uniform(-3000.0, 3000.0)]
        near += [i + (0 if n[i] >= 0 else 3) for i in range(3)]
        far += [i + (3 if n[i] >= 0 else 0) for i in range(3)]
    lo = [rng.uniform(-3000.0, 3000.0) for _ in range(3)]
    hi = [x + rng.uniform(1.0, 2000.0) for x in lo]
    return {"regs": dict(rand_regs(rng), ECX=SCRATCH + 0x300),
            "mem": [(SCRATCH + 0x360, struct.pack("<24f", *planes)),
                    (SCRATCH + 0x3c0, bytes(far) + bytes(near)),
                    (SCRATCH + 0x500, struct.pack("<6f", *lo, *hi))],
            "args": [SCRATCH + 0x500]}


# Tests the world goes through before it is drawn (session 6: Gothic 3 draws
# the hero and the sky, never terrain, buildings or NPCs), as the game ships
# them and as the listing hands them to the translator.
ENGINE_PROCESSING_RANGE = [
    (0, 'MOV EDX,dword ptr [ESP + 0x4]'),
    (4, 'FLD float ptr [ECX + 0x70]'),
    (7, 'FLD float ptr [EDX]'),
    (9, 'FCOMIP ST0,ST1'),
    (11, 'FSTP ST0'),
    (13, 'JA 0x10491910'),
    (15, 'FLD float ptr [EDX + 0xc]'),
    (18, 'FLD float ptr [ECX + 0x64]'),
    (21, 'FCOMIP ST0,ST1'),
    (23, 'FSTP ST0'),
    (25, 'JA 0x10491910'),
    (27, 'FLD float ptr [ECX + 0x74]'),
    (30, 'FLD float ptr [EDX + 0x4]'),
    (33, 'FCOMIP ST0,ST1'),
    (35, 'FSTP ST0'),
    (37, 'JA 0x10491910'),
    (39, 'FLD float ptr [EDX + 0x10]'),
    (42, 'FLD float ptr [ECX + 0x68]'),
    (45, 'FCOMIP ST0,ST1'),
    (47, 'FSTP ST0'),
    (49, 'JA 0x10491910'),
    (51, 'FLD float ptr [ECX + 0x78]'),
    (54, 'FLD float ptr [EDX + 0x8]'),
    (57, 'FCOMIP ST0,ST1'),
    (59, 'FSTP ST0'),
    (61, 'JA 0x10491910'),
    (63, 'FLD float ptr [EDX + 0x14]'),
    (66, 'FLD float ptr [ECX + 0x6c]'),
    (69, 'FCOMIP ST0,ST1'),
    (71, 'FSTP ST0'),
    (73, 'JA 0x10491910'),
    (75, 'MOV AL,0x1'),
    (77, 'RET 0x4'),
    (80, 'XOR AL,AL'),
    (82, 'RET 0x4'),
]
ENGINE_CULL_FRUSTUM_BOX = [
    (0, 'SUB ESP,0x10'),
    (3, 'FLDZ'),
    (5, 'PUSH EBX'),
    (6, 'PUSH ESI'),
    (7, 'MOV ESI,dword ptr [ESP + 0x1c]'),
    (11, 'PUSH EDI'),
    (12, 'MOV byte ptr [ESP + 0xf],0x0'),
    (17, 'MOV BL,0x5'),
    (19, 'MOVSX EDX,BL'),
    (22, 'LEA EAX,[EDX + 0x6]'),
    (25, 'SHL EAX,0x4'),
    (28, 'ADD EAX,ECX'),
    (30, 'LEA EDI,[ECX + EDX*0x2]'),
    (33, 'ADD EDI,EDX'),
    (35, 'FLD float ptr [EAX + 0x4]'),
    (38, 'FSTP float ptr [ESP + 0x10]'),
    (42, 'FLD float ptr [EAX]'),
    (44, 'FSTP float ptr [ESP + 0x20]'),
    (48, 'FLD float ptr [EAX + 0x8]'),
    (51, 'FSTP float ptr [ESP + 0x14]'),
    (55, 'FLD float ptr [EAX + 0xc]'),
    (58, 'LEA EAX,[ECX + EDX*0x2 + 0x80]'),
    (65, 'MOVZX EAX,byte ptr [EDX + EAX*0x1 + 0x40]'),
    (70, 'FSTP float ptr [ESP + 0x18]'),
    (74, 'FLD float ptr [ESI + EAX*0x4]'),
    (77, 'MOVZX EAX,byte ptr [EDI + 0xc1]'),
    (84, 'FLD float ptr [ESP + 0x20]'),
    (88, 'FLD ST0'),
    (90, 'FMULP ST2'),
    (92, 'FLD float ptr [ESI + EAX*0x4]'),
    (95, 'MOVZX EAX,byte ptr [EDI + 0xc2]'),
    (102, 'FLD float ptr [ESP + 0x10]'),
    (106, 'FLD ST0'),
    (108, 'FMULP ST2'),
    (110, 'FXCH ST3'),
    (112, 'FADDP'),
    (114, 'FLD float ptr [ESI + EAX*0x4]'),
    (117, 'FLD float ptr [ESP + 0x14]'),
    (121, 'FLD ST0'),
    (123, 'FMULP ST2'),
    (125, 'FXCH ST2'),
    (127, 'FADDP'),
    (129, 'FLD float ptr [ESP + 0x18]'),
    (133, 'FLD ST0'),
    (135, 'FADDP ST2,ST0'),
    (137, 'FXCH ST5'),
    (139, 'FCOMI ST0,ST1'),
    (141, 'FSTP ST1'),
    (143, 'JA 0x1040cacb'),
    (145, 'CMP byte ptr [ESP + 0xf],0x0'),
    (150, 'JNZ 0x1040caa6'),
    (152, 'LEA EAX,[ECX + EDX*0x2 + 0x8c]'),
    (159, 'MOVZX EDX,byte ptr [EDX + EAX*0x1 + 0x46]'),
    (164, 'MOVZX EAX,byte ptr [EDI + 0xd3]'),
    (171, 'FLD float ptr [ESI + EDX*0x4]'),
    (174, 'MOVZX EDX,byte ptr [EDI + 0xd4]'),
    (181, 'FMULP ST3'),
    (183, 'FLD float ptr [ESI + EAX*0x4]'),
    (186, 'FMULP ST4'),
    (188, 'FXCH ST2'),
    (190, 'FADDP ST3,ST0'),
    (192, 'FMUL float ptr [ESI + EDX*0x4]'),
    (195, 'FADDP ST2,ST0'),
    (197, 'FXCH'),
    (199, 'FADDP ST2,ST0'),
    (201, 'FCOMI ST0,ST1'),
    (203, 'FSTP ST1'),
    (205, 'JBE 0x1040caae'),
    (207, 'MOV byte ptr [ESP + 0xf],0x1'),
    (212, 'JMP 0x1040caae'),
    (214, 'FSTP ST2'),
    (216, 'FSTP ST2'),
    (218, 'FSTP ST1'),
    (220, 'FSTP ST1'),
    (222, 'ADD BL,0xff'),
    (225, 'JNS 0x1040c9e3'),
    (231, 'XOR EAX,EAX'),
    (233, 'FSTP ST0'),
    (235, 'CMP byte ptr [ESP + 0xf],AL'),
    (239, 'POP EDI'),
    (240, 'POP ESI'),
    (241, 'SETNZ AL'),
    (244, 'POP EBX'),
    (245, 'ADD ESP,0x10'),
    (248, 'RET 0x4'),
    (251, 'FSTP ST2'),
    (253, 'POP EDI'),
    (254, 'FSTP ST2'),
    (256, 'POP ESI'),
    (257, 'FSTP ST1'),
    (259, 'MOV EAX,0xf'),
    (264, 'FSTP ST1'),
    (266, 'POP EBX'),
    (267, 'FSTP ST0'),
    (269, 'ADD ESP,0x10'),
    (272, 'RET 0x4'),
]
CASES += [
    Case("Gothic 3 Engine 104918c0 eCEntityAdmin::IntersectsProcessingRangeRegion", 0x0D02FD00,
         rebased(ENGINE_PROCESSING_RANGE, 0x104918c0, 0x0D02FD00, 85),
         "8b542404d94170d902dff1ddd87741d9420cd94164dff1ddd87735d94174d94204dff1ddd87729d94210d94168dff1ddd8771dd94178d94208dff1ddd87711d94214d9416cdff1ddd87705b001c2040032c0c20400", processing_range_setup),
    Case("Gothic 3 Engine 1040c9d0 eCFrustumFilter::CullFrustum(bCBox)", 0x0D02FA00,
         rebased(ENGINE_CULL_FRUSTUM_BOX, 0x1040c9d0, 0x0D02FA00, 275),
         "83ec10d9ee53568b74241c57c644240f00b3050fbed38d4206c1e00403c18d3c5103fad94004d95c2410d900d95c2420d94008d95c2414d9400c8d8451800000000fb6440240d95c2418d904860fb687c1000000d9442420d9c0decad904860fb687c2000000d9442410d9c0decad9cbdec1d90486d9442414d9c0decad9cadec1d9442418d9c0dec2d9cddbf1ddd9776a807c240f00753e8d84518c0000000fb65402460fb687d3000000d904960fb697d4000000decbd90486deccd9cadec3d80c96dec2d9c9dec2dbf1ddd9760fc644240f01eb08dddadddaddd9ddd980c3ff0f892cffffff33c0ddd83844240f5f5e0f95c05b83c410c20400ddda5fddda5eddd9b80f000000ddd95bddd883c410c20400", frustum_box_setup),
]


def sphere_pair_setup(rng):
    """Two bCSpheres (radius, then the centre): ECX's and the argument's, apart,
    overlapping or touching."""
    r1, r2 = rng.uniform(1.0, 500.0), rng.uniform(1.0, 500.0)
    c1 = [rng.uniform(-3000.0, 3000.0) for _ in range(3)]
    d = [rng.uniform(-1.0, 1.0) for _ in range(3)]
    n = max(sum(x * x for x in d) ** 0.5, 1e-3)
    dist = rng.choice([rng.uniform(0.0, r1 + r2), rng.uniform(r1 + r2, 3 * (r1 + r2)), r1 + r2])
    c2 = [a + b / n * dist for a, b in zip(c1, d)]
    return {"regs": dict(rand_regs(rng), ECX=SCRATCH + 0x300),
            "mem": [(SCRATCH + 0x300, struct.pack("<4f", r1, *c1)),
                    (SCRATCH + 0x500, struct.pack("<4f", r2, *c2))],
            "args": [SCRATCH + 0x500]}


# SharedBase 1001da30, bCSphere::Intersects(bCSphere), which
# eCFrustumFilter::CullCamera asks first.
SHAREDBASE_SPHERE_INTERSECTS = [
    (0, 'SUB ESP,0xc'),
    (3, 'MOV EAX,dword ptr [ESP + 0x10]'),
    (7, 'FLD float ptr [EAX + 0x4]'),
    (10, 'FSTP float ptr [ESP]'),
    (13, 'FLD float ptr [EAX + 0x8]'),
    (16, 'FSTP float ptr [ESP + 0x4]'),
    (20, 'FLD float ptr [EAX + 0xc]'),
    (23, 'FSTP float ptr [ESP + 0x8]'),
    (27, 'FLD float ptr [ESP]'),
    (30, 'FSUB float ptr [ECX + 0x4]'),
    (33, 'FSTP float ptr [ESP]'),
    (36, 'FLD float ptr [ESP + 0x4]'),
    (40, 'FSUB float ptr [ECX + 0x8]'),
    (43, 'FSTP float ptr [ESP + 0x4]'),
    (47, 'FLD float ptr [ESP + 0x8]'),
    (51, 'FSUB float ptr [ECX + 0xc]'),
    (54, 'FSTP float ptr [ESP + 0x8]'),
    (58, 'FLD float ptr [EAX]'),
    (60, 'FADD float ptr [ECX]'),
    (62, 'FSTP float ptr [ESP + 0x10]'),
    (66, 'FLD float ptr [ESP + 0x10]'),
    (70, 'FLD float ptr [ESP]'),
    (73, 'FLD float ptr [ESP + 0x4]'),
    (77, 'FLD float ptr [ESP + 0x8]'),
    (81, 'FLD ST3'),
    (83, 'FMULP ST4'),
    (85, 'FXCH ST3'),
    (87, 'FSTP float ptr [ESP + 0x10]'),
    (91, 'FLD float ptr [ESP + 0x10]'),
    (95, 'FLD ST1'),
    (97, 'FMULP ST2'),
    (99, 'FLD ST2'),
    (101, 'FMULP ST3'),
    (103, 'FXCH'),
    (105, 'FADDP ST2,ST0'),
    (107, 'FLD ST2'),
    (109, 'FMULP ST3'),
    (111, 'FXCH'),
    (113, 'FADDP ST2,ST0'),
    (115, 'FXCH'),
    (117, 'FSTP float ptr [ESP + 0x10]'),
    (121, 'FLD float ptr [ESP + 0x10]'),
    (125, 'FXCH'),
    (127, 'FCOMIP ST0,ST1'),
    (129, 'FSTP ST0'),
    (131, 'JBE 0x1001dabd'),
    (133, 'MOV AL,0x1'),
    (135, 'ADD ESP,0xc'),
    (138, 'RET 0x4'),
    (141, 'XOR AL,AL'),
    (143, 'ADD ESP,0xc'),
    (146, 'RET 0x4'),
]
CASES += [
    Case("Gothic 3 SharedBase 1001da30 bCSphere::Intersects(bCSphere)", 0x0D02FF00,
         rebased(SHAREDBASE_SPHERE_INTERSECTS, 0x1001da30, 0x0D02FF00, 149),
         "83ec0c8b442410d94004d91c24d94008d95c2404d9400cd95c2408d90424d86104d91c24d9442404d86108d95c2404d9442408d8610cd95c2408d900d801d95c2410d9442410d90424d9442404d9442408d9c3deccd9cbd95c2410d9442410d9c1decad9c2decbd9c9dec2d9c2decbd9c9dec2d9c9d95c2410d9442410d9c9dff1ddd87608b00183c40cc2040032c083c40cc20400", sphere_pair_setup),
]


def max_extend_setup(rng):
    """A bCBox (min then max corner) at ECX, its widest axis any of the three,
    sometimes thinner than the 1e-5 floor; the two constants it reads beside it."""
    lo = [rng.uniform(-3000.0, 3000.0) for _ in range(3)]
    size = [rng.choice([rng.uniform(0.0, 2e-5), rng.uniform(1.0, 4000.0)]) for _ in range(3)]
    return {"regs": dict(rand_regs(rng), ECX=SCRATCH + 0x300),
            "mem": [(SCRATCH + 0x300, struct.pack("<6f", *lo, *[a + s for a, s in zip(lo, size)])),
                    (SCRATCH + 0x800, struct.pack("<f", 1e-5)),
                    (SCRATCH + 0x808, struct.pack("<d", 0.5))]}


def moved_constants(lines, code, table):
    """A listing and its bytes with absolute data addresses moved into scratch."""
    for old, new in table:
        lines = [(off, text.replace("0x%x" % old, "0x%x" % new)) for off, text in lines]
        code = code.replace(struct.pack("<I", old).hex(), struct.pack("<I", new).hex())
    return lines, code


# SharedBase 1001b800 bCBox::GetMaxExtend: half the widest side, which sizes
# the eight children of an AO tree node (Engine 103595b0). SSE stores and x87
# compares on the same stack slot; its constants are 1e-5 (0x1008b4b0) and
# 0.5 (0x10090a70).
SHAREDBASE_MAX_EXTEND = moved_constants(rebased([
    (0, 'SUB ESP,0x8'),
    (3, 'MOVSS XMM0,dword ptr [0x1008b4b0]'),
    (11, 'MOVSS dword ptr [ESP],XMM0'),
    (16, 'MOV EDX,0x3'),
    (21, 'FLD float ptr [ECX + EDX*0x4 + 0x8]'),
    (25, 'SUB EDX,0x1'),
    (28, 'FSUB float ptr [ECX + EDX*0x4]'),
    (31, 'FSTP float ptr [ESP + 0x4]'),
    (35, 'FLD float ptr [ESP]'),
    (38, 'FLD float ptr [ESP + 0x4]'),
    (42, 'FCOMIP ST0,ST1'),
    (44, 'FSTP ST0'),
    (46, 'JBE 0x1001b83b'),
    (48, 'MOVSS XMM0,dword ptr [ESP + 0x4]'),
    (54, 'MOVSS dword ptr [ESP],XMM0'),
    (59, 'TEST EDX,EDX'),
    (61, 'JNZ 0x1001b815'),
    (63, 'FLD float ptr [ESP]'),
    (66, 'FMUL double ptr [0x10090a70]'),
    (72, 'FSTP float ptr [ESP + 0x4]'),
    (76, 'FLD float ptr [ESP + 0x4]'),
    (80, 'ADD ESP,0x8'),
    (83, 'RET'),
], 0x1001b800, 0x0D02FB80, 84), "83ec08f30f1005b0b40810f30f110424ba03000000d944910883ea01d82491d95c2404d90424d9442404dff1ddd8760bf30f10442404f30f11042485d275d6d90424dc0d700a0910d95c2404d944240483c408c3",
                                        [(0x1008b4b0, SCRATCH + 0x800), (0x10090a70, SCRATCH + 0x808)])
CASES += [
    Case("Gothic 3 SharedBase 1001b800 bCBox::GetMaxExtend", 0x0D02FB80,
         SHAREDBASE_MAX_EXTEND[0], SHAREDBASE_MAX_EXTEND[1], max_extend_setup),
]


def test_capstone_spellings_of_the_popping_compares():
    lines = dict(recovered_lines(0, "df f1 df e9 db f1 db e9"))
    assert lines == {0: "FCOMIP ST1", 2: "FUCOMIP ST1", 4: "FCOMI ST1", 6: "FUCOMI ST1"}


# -------------------------------------------------------------- translate --

class NoImage(object):
    """Complete synthetic listings need no recovery, but expose their bytes
    to disambiguate instruction forms that share the same listing text."""
    base = 0
    end = 0
    size = 0
    md = None

    def __init__(self, case=None):
        self.case = case

    def insn_end(self, addr, mnem):
        return None

    def rd32(self, va):
        return None

    def rd8(self, va):
        if self.case and self.case.addr <= va < self.case.addr + len(self.case.code):
            return self.case.code[va - self.case.addr]
        return None

    def is_exec(self, va):
        return False

    def relocated_pointers(self):
        return set()


class Opts(object):
    eager_flags = False


def translate_case(case):
    """The emitted C for one case, or the TranslateError it raised."""
    tr = T.Translator(NoImage(case), {a for c in CASES for a in (c.addr,) + c.entries}, Opts())
    insns = T.parse_listing_text(case.listing())
    fn = T.Function(case.addr, case.name, len(case.code), insns)
    fn.measure(NoImage())
    tr.prepare(fn)
    text = "\n".join(tr.translate(fn, case.entries))
    assert "FN(" not in text, "synthetic cases must not call anything"
    return text


# ------------------------------------------------------------------ build --

class Native(object):
    def __init__(self, lib_path):
        self.lib = C.CDLL(lib_path)
        self.lib.harness_mem.restype = C.POINTER(C.c_uint8)
        self.lib.harness_x86_size.restype = C.c_uint64
        self.lib.harness_run.argtypes = [C.c_uint32, C.POINTER(X86)]
        self.lib.harness_eflags.restype = C.c_uint32
        self.lib.harness_eflags.argtypes = [C.POINTER(X86)]
        assert self.lib.harness_x86_size() == C.sizeof(X86)
        self.mem = self.lib.harness_mem()

    def write(self, addr, data):
        C.memmove(C.addressof(self.mem.contents) + addr, data, len(data))

    def read(self, addr, n):
        return C.string_at(C.addressof(self.mem.contents) + addr, n)

    def zero(self, addr, n):
        C.memset(C.addressof(self.mem.contents) + addr, 0, n)


@pytest.fixture(scope="module")
def built():
    """Translate every case, compile the ones that translated into one shared
    library with the harness, and return (Native, {case name: error})."""
    clang = shutil.which("clang")
    if clang is None:
        pytest.skip("clang is not on PATH")
    if platform.system() == "Windows":
        # The harness is a POSIX shared library read through ctypes: clang's
        # MSVC target takes no -fPIC and exports nothing without dllexport.
        # The instruction semantics are checked on the other runners.
        pytest.skip("the instruction harness builds as a POSIX shared library")
    errors, parts, ok = {}, ['#include "x86.h"', T.CTX_MACROS], []
    for case in CASES:
        try:
            parts.append("/* %s */\n%s\n" % (case.name, translate_case(case)))
            ok.append(case)
        except T.TranslateError as e:
            errors[case.name] = str(e)
    work = tempfile.mkdtemp(prefix="translate-insns-")
    with open(os.path.join(work, "synth.c"), "w") as fh:
        fh.write("\n".join(parts))
    with open(os.path.join(work, "table.c"), "w") as fh:
        fh.write('#include "x86.h"\n')
        entries = [a for case in ok for a in (case.addr,) + case.entries]
        for addr in entries:
            fh.write("void fn_%08x(X86 *c);\n" % addr)
        fh.write("int recomp_is_call_return(uint32_t target) { return target == 0x%08xu; }\n" % MAGIC_RET)
        fh.write("int32_t recomp_index_of(uint32_t target) { switch (target) {\n")
        for index, addr in enumerate(entries):
            fh.write("case 0x%08xu: return %d;\n" % (addr, index))
        fh.write("default: return -1; } }\n")
        fh.write("void recomp_call(X86 *c, uint32_t target) {\n    switch (target) {\n")
        for addr in entries:
            fh.write("    case 0x%08xu: fn_%08x(c); return;\n" % (addr, addr))
        fh.write("    default: recomp_unknown_call(c, target); c->eip = rd32(c->r[R_ESP]); c->r[R_ESP] += 4;\n"
                 "    }\n}\n")
        # The harness calls each case from MAGIC_RET outside the code arena.
        # Like a non-entry CALL continuation, it returns without popping or
        # dispatching: the case's computed-return epilogue already cleaned ESP.
        fh.write("void recomp_jump(X86 *c, uint32_t target) {\n"
                 "    if (target == 0x%08xu) { c->eip = target; return; }\n"
                 "    recomp_call(c, target);\n}\n" % MAGIC_RET)
    lib = os.path.join(work, "libinsns" + (".dylib" if platform.system() == "Darwin" else ".so"))
    subprocess.check_call([clang, "-O1", "-g", "-std=c11", "-Wall", "-Wextra", "-Wno-unused",
                           "-fPIC", "-shared", "-I", os.path.join(ROOT, "runtime"),
                           os.path.join(HERE, "harness.c"), os.path.join(work, "synth.c"),
                           os.path.join(work, "table.c"), "-o", lib])
    return Native(lib), errors


# -------------------------------------------------------------------- run --

class Emu(object):
    def __init__(self):
        self.u = Uc(UC_ARCH_X86, UC_MODE_32)
        self.u.mem_map(CODE_BASE, CODE_SIZE)
        self.u.mem_map(STACK_BASE, STACK_SIZE)
        self.u.mem_map(SCRATCH, SCRATCH_SIZE)
        self.u.mem_map(CAVE, CAVE_SIZE)
        self.interrupts = []
        self.u.hook_add(UC_HOOK_INTR, lambda u, intno, user: self.interrupts.append(intno))

    def reset(self, case):
        self.u.mem_write(STACK_BASE, b"\0" * STACK_SIZE)
        self.u.mem_write(SCRATCH, b"\0" * SCRATCH_SIZE)
        self.u.mem_write(case.addr, case.code)
        self.u.reg_write(UC_X86_REG_FPCW, FPU_CW_INIT)
        self.u.reg_write(UC_X86_REG_MXCSR, 0x1f80)
        self.u.reg_write(UC_X86_REG_FPSW, 0)
        self.u.reg_write(UC_X86_REG_FPTAG, FPU_TAG_INIT)
        self.u.reg_write(UC_X86_REG_EFLAGS, EFLAGS_MISC_INIT)
        self.interrupts = []


def run_once(case, native, emu, rng):
    state = case.setup(rng)
    regs = dict(rand_regs(rng), **state.get("regs", {}))
    args = state.get("args", [])
    frame = struct.pack("<I", MAGIC_RET) + struct.pack("<%dI" % len(args), *args)

    emu.reset(case)
    native.zero(STACK_BASE, STACK_SIZE)
    native.zero(SCRATCH, SCRATCH_SIZE)
    for addr, data in state.get("mem", []):
        emu.u.mem_write(addr, data)
        native.write(addr, data)
    emu.u.mem_write(ESP_INIT, frame)
    native.write(ESP_INIT, frame)

    c = X86()
    for i, name in enumerate(REG_NAMES):
        value = ESP_INIT if name == "ESP" else regs[name]
        c.r[i] = value
        emu.u.reg_write(UC_REGS[i], value)
    c.eflags_misc = EFLAGS_MISC_INIT
    c.fpu_cw = FPU_CW_INIT
    c.fpu_tag = FPU_TAG_INIT

    emu.u.emu_start(case.addr, MAGIC_RET, timeout=2 * 1000 * 1000, count=100000)
    native.lib.harness_run(case.addr, C.byref(c))

    problems = []
    for i, name in enumerate(REG_NAMES):
        u = emu.u.reg_read(UC_REGS[i])
        if c.r[i] != u:
            problems.append("%s: native %08x unicorn %08x" % (name, c.r[i], u))
    nf = native.lib.harness_eflags(C.byref(c)) & EFLAGS_COMPARED
    uf = emu.u.reg_read(UC_X86_REG_EFLAGS) & EFLAGS_COMPARED
    if nf != uf:
        problems.append("EFLAGS: native %03x unicorn %03x" % (nf, uf))
    for base, size, what in ((STACK_BASE, STACK_SIZE, "stack"), (SCRATCH, SCRATCH_SIZE, "scratch")):
        n, u = bytearray(native.read(base, size)), bytearray(emu.u.mem_read(base, size))
        for lo, length in case.ignore:
            if base <= lo < base + size:
                n[lo - base:lo - base + length] = u[lo - base:lo - base + length] = b"\0" * length
        if n != u:
            first = next(k for k in range(size) if n[k] != u[k])
            problems.append("%s differs first at %08x: native %02x unicorn %02x"
                             % (what, base + first, n[first], u[first]))
    if case.expect_intr is not None:
        assert emu.interrupts == [case.expect_intr], emu.interrupts
    assert not problems, "\n".join(problems)
    if case.check:
        native_view = _MemView(native)
        emu_view = _MemView(emu)
        case.check(native_view, emu_view)


class _MemView(object):
    """Slice guest memory of either engine by absolute address."""

    def __init__(self, engine):
        self.engine = engine

    def __getitem__(self, s):
        assert isinstance(s, slice) and s.step is None
        n = s.stop - s.start
        if isinstance(self.engine, Native):
            return self.engine.read(s.start, n)
        return bytes(self.engine.u.mem_read(s.start, n))


@pytest.mark.parametrize("case", CASES, ids=[c.name for c in CASES])
def test_case(case, built):
    native, errors = built
    if case.name in errors:
        pytest.fail("translator: %s" % errors[case.name])
    emu = Emu()
    rng = random.Random(0x4D616A ^ case.addr)
    for _ in range(40):
        try:
            run_once(case, native, emu, rng)
        except UcError as e:
            pytest.fail("unicorn: %s" % e)


def test_inline_call_cache_gives_each_indirect_call_its_own_word():
    """[translate] inline_call_cache: an indirect CALL, through an import slot or a register,
    dispatches through recomp_call_cached with a word of its own; a direct call does not change,
    and with the option off the indirect call stays recomp_call."""
    listing = ("0d01d100  CALL dword ptr [0x0d01e000]\n"
               "0d01d106  CALL EAX\n"
               "0d01d108  RET\n")

    def emitted():
        tr = T.Translator(NoImage(), {0x0D01D100}, Opts())
        fn = T.Function(0x0D01D100, "calls", 0x9, T.parse_listing_text(listing))
        fn.measure(NoImage())
        tr.prepare(fn)
        return "\n".join(tr.translate(fn))

    saved = T.INLINE_CALL_CACHE
    try:
        T.INLINE_CALL_CACHE = True
        text = emitted()
        assert text.count("static uint64_t ic_ = 0;") == 2
        assert text.count("recomp_call_cached(c, t_, &ic_);") == 2
        assert "recomp_call(c, t_);" not in text
        T.INLINE_CALL_CACHE = False
        text = emitted()
        assert "recomp_call_cached" not in text and text.count("recomp_call(c, t_);") == 2
    finally:
        T.INLINE_CALL_CACHE = saved


def test_mmx_goes_to_the_mm_registers_and_xmm_forms_still_go_to_sse():
    """MMX is translated onto the MMn registers, because some codecs use it
    without asking CPUID, and the NFS Most Wanted race needs it.  MOVQ and MOVD
    are spelled the same for both register files, so the operands are what
    decide: an MMn operand takes the MMX path, an XMM one the SSE2 path, whose
    MOVQ also clears the upper half of the destination.  EMMS marks every x87
    register empty, which is all the x87 model has to do for it."""
    tr = T.Translator(NoImage(), {0x0D01D000}, Opts())
    insns = T.parse_listing_text(
        "0d01d000  MOVQ MM0,qword ptr [ESI]\n"
        "0d01d003  PXOR MM7,MM7\n"
        "0d01d006  PADDW MM0,MM7\n"
        "0d01d009  EMMS\n"
        "0d01d00b  MOVQ XMM0,qword ptr [EDX]\n"
        "0d01d00f  RET\n")
    fn = T.Function(0x0D01D000, "simd", 0x10, insns)
    fn.measure(NoImage())
    tr.prepare(fn)
    text = "\n".join(tr.translate(fn))
    assert "c->mm[0] = rd64(" in text
    assert "c->mm[7] = mmx_pxor(c->mm[7], c->mm[7]);" in text
    assert "c->mm[0] = mmx_padd(c->mm[0], c->mm[7], 16u);" in text
    assert "c->fpu_tag = 0xffffu;" in text
    # The XMM spelling of MOVQ keeps the SSE2 lanes, and zeroes the upper half.
    assert "c->xmm[0][0] = s0_;" in text and "c->xmm[0][2] = 0u;" in text
    # MMX must not touch the SSE2 register file, nor the reverse.
    assert "c->xmm[0][0] = c->mm" not in text


def test_segment_register_loads_do_not_reach_the_flat_model():
    """Ghidra decodes data as code now and then, and `MOV CS,[mem]` is what
    such bytes can spell.  A segment load has no meaning in the flat model
    the runtime provides: a load of CS is an invalid opcode on the CPU too,
    so it raises #UD; a load of a data segment is dropped."""
    tr = T.Translator(NoImage(), {0x0D01F000}, Opts())
    insns = T.parse_listing_text("0d01f000  MOV CS,word ptr [ECX]\n0d01f003  MOV DS,AX\n0d01f005  RET\n")
    fn = T.Function(0x0D01F000, "seg", 6, insns)
    fn.measure(NoImage())
    tr.prepare(fn)
    text = "\n".join(tr.translate(fn))
    assert "recomp_int(c, 6u);" in text
    assert "0x23u" not in text and "wr" not in text.split("MOV DS,AX")[1].split("\n")[0]
