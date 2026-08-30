#!/usr/bin/env python3
"""Recover per-class field lists from the engine's save/load serializers.

WHAT WAS READ (not inferred)
============================

Registration mechanism
----------------------
Every serialisable class ``T`` has a companion singleton ``Moho::TSerializer``, a
one-virtual-slot class whose RTTI base is ``gpg::SerSaveLoadHelper<T>``.  244 such
vtables exist.  Each singleton is filled by a static initialiser::

    mov  ecx, <inst>
    call 0x009533b0                                   ; atexit-style registration
    mov  dword ptr [inst+0x0C], <T::SerLoad  entry>
    mov  dword ptr [inst+0x10], <T::SerSave  entry>
    mov  dword ptr [inst+0x00], <vtable VA>           ; vptr written last

The single virtual slot publishes those two pointers onto the reflection ``RType``
for ``T`` (``type->mSerLoadFunc`` / ``type->mSerSaveFunc``; the assert strings at
0x00e4ec88 name them, from ``gpgcore/reflection/serialization.h`` lines 84 and 87).
This is the *member*-carrying path; the enum registrars at 0x006c1120 / 0x006c11c0
found by C-134 are a different, name-free path and are not used here.

Calling convention of the save entry point
------------------------------------------
``SerSaveFunc(gpg::WriteArchive& ar /*arg1*/, const T* obj /*arg2*/, ...)``, __cdecl.
The entry point is almost always a thin forwarder into the real ``MemberSave`` body,
and MSVC gave those bodies *custom* register conventions (they are static functions
with fully known call sites), so the register holding the object differs per class:
75 forwarders use ``esi=ar, edi=obj``, 40 use ``eax=ar, esi=obj``, others use
ecx/edx/ebx, and 20 have the body inlined into the entry point outright.  Assuming a
fixed register recovers 7 of 244 classes; this tool therefore *simulates* the
forwarder to find where arg2 lands, then propagates that taint through the body.

Member emission idioms inside ``MemberSave``
--------------------------------------------
Members are emitted in ascending declaration order by one of four idioms:

  * ``lea R,[obj+off]`` + ``call 0x00956e80`` -- ``WriteArchive::Write(RType*, const
    void*, RRef&)``.  The ``RType*`` comes from a lazily initialised global filled by
    ``push <TypeDescriptor>; call 0x0094ef10`` (``gpg::LookupRType``), so the member's
    *type name* is readable from the MSVC ``TypeDescriptor``.
  * ``mov R,[obj+off]`` + a per-pointee helper -- a tracked pointer; the helper
    carries the pointee's ``TypeDescriptor``.
  * a width-specific load (``fld dword``, ``movzx byte``, ``mov dword``) followed by
    an indirect call through a fixed ``gpg::WriteArchive`` vtable slot -- a primitive.
  * an inlined sub-object serializer, which appears as a plain run of offsets.

MEASURED NEGATIVE: THERE ARE NO MEMBER NAMES
============================================
``Write`` takes an ``RRef&`` that every call site constructs by zeroing two dwords
immediately before the ``lea``/``push``; no string is ever passed.  Corroborating
counts, all reported by ``--report-name-scan``:
  * ``.rdata`` string constants pushed anywhere inside all 244 MemberSave bodies;
  * the whole binary contains exactly ONE string matching ``^m[A-Z][A-Za-z0-9]+$``
    (``mInitFinished``), and that one is an assert *expression*, not a field name;
  * the reflection headers assert only on ``mSerLoadFunc``, ``mSerSaveFunc``,
    ``mSerConstructFunc``, ``mSerSaveConstructArgsFunc``, ``mDelete``, ``mVersion``
    and ``mInitFinished`` -- there is no ``AddMember``/``RegisterField`` API.
So ``member_name`` is always empty.  What the vein does pay is **order, offset and
type**, which is what the field list actually needs.

VALIDATION
==========
``tools/re/verify_serializer_members.py`` scores the output against 63 offsets
established by unrelated means (Lua accessors, vtable dispatch, hand disassembly):
58/63 = 92.1% land exactly on a recovered member boundary, 0 land on a *different*
field, 5 are absent because the engine does not save them (an RTTI base subobject it
rebuilds, a runtime backpointer, an intrusive list head, a derived counter).

KNOWN LIMITS (each measured, none silent)
=========================================
  * An array written by a loop over a moving pointer (``fld [esi]; add esi,4``) is
    seen once by a linear sweep, so it appears as its first element.  Such rows carry
    ``in_loop=1`` -- 11 of 1617.  ``Moho::VMatrix4`` is the clean example: 16 floats,
    one row.
  * 23 classes serialise as a single write at offset 0 -- either a base subobject
    (``Moho::Shield`` writes only its ``Entity``) or an opaque proxy (``Moho::COGrid``
    goes through a converter at 0x005b3950 and a different archive entry 0x009555d0),
    so no member list exists for them to give.
  * 7 classes have a bare ``ret`` for a MemberSave and save nothing at all.
  * A member's size is never read, only its offset; ``sizeof`` of a nested aggregate
    is therefore a lower bound derived from its own last member.

Usage:
    python3 tools/re/extract_serializer_members.py \
        --out build/re-fa/exports/serializer_members.tsv
    python3 tools/re/extract_serializer_members.py --report-name-scan
    python3 tools/re/extract_serializer_members.py --only Moho::Unit --dump
"""

from __future__ import annotations

import argparse
import re
import struct
import subprocess
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pe_reader import PE32  # noqa: E402

EXE = "/Users/olek/projects/llm/input/recoil-metal/retail-fa/bin/SupremeCommander.exe"
VTABLE_CLASSES = "build/re-fa/exports/vtables.classes.tsv"
VTABLES = "build/re-fa/exports/vtables.tsv"
CALLGRAPH = "build/re-fa/exports/callgraph.tsv"

LOOKUP_RTYPE = 0x0094EF10          # gpg::LookupRType(const type_info&) -> RType*
WRITE_ARCHIVE_WRITE = 0x00956E80   # WriteArchive::Write(RType*, const void*, RRef&)
ARCHIVE_SER_POINTER = 0x00956500   # tracked-pointer write, reached via a per-type helper

# Calls that must never be mistaken for "the forwarder's one call into the body".
RUNTIME_CALLS = {LOOKUP_RTYPE, WRITE_ARCHIVE_WRITE, ARCHIVE_SER_POINTER, 0x009533B0}

SERIALIZER_INSTANCE_LOAD_OFF = 0x0C
SERIALIZER_INSTANCE_SAVE_OFF = 0x10

MAX_FUNC_BYTES = 0x3000
MAX_FORWARDER_DEPTH = 4

REGS = ("eax", "ecx", "edx", "ebx", "esi", "edi", "ebp", "esp")
GP_REGS = ("eax", "ecx", "edx", "ebx", "esi", "edi", "ebp")


# ======================================================================================
# byte-level scans (relocations are stripped and the image base is fixed, so raw
# absolute addresses in the instruction stream are usable directly)
# ======================================================================================


def scan_mov_mem_imm32(pe: PE32) -> dict[int, list[tuple[int, int]]]:
    """All ``C7 05 <dst32> <imm32>`` sites: dst VA -> [(site VA, imm)].

    ``mov dword ptr [abs32], imm32`` is the only encoding MSVC emits for a static
    initialiser storing a constant into a fixed global -- exactly how a serializer
    singleton is filled in.
    """
    out: dict[int, list[tuple[int, int]]] = defaultdict(list)
    text = next(s for s in pe.sections if s.name == ".text")
    blob = pe.read(text.virtual_address, min(text.virtual_size, text.raw_size))
    start = 0
    while True:
        i = blob.find(b"\xc7\x05", start)
        if i < 0 or i + 10 > len(blob):
            break
        dst, imm = struct.unpack_from("<II", blob, i + 2)
        out[dst].append((text.virtual_address + i, imm))
        start = i + 1
    return out


def scan_rtype_globals(pe: PE32) -> dict[int, int]:
    """Lazily initialised ``RType*`` cache globals -> their ``TypeDescriptor`` VA.

    The idiom is ``push <td>; call LookupRType; add esp,4; mov [glob], eax``.  The
    whole 19-byte run is matched and the call target checked, so false positives are
    structurally impossible.
    """
    out: dict[int, int] = {}
    text = next(s for s in pe.sections if s.name == ".text")
    base = text.virtual_address
    blob = pe.read(base, min(text.virtual_size, text.raw_size))
    start = 0
    while True:
        i = blob.find(b"\x68", start)
        if i < 0 or i + 19 > len(blob):
            break
        start = i + 1
        if blob[i + 5] != 0xE8:
            continue
        if blob[i + 10:i + 13] != b"\x83\xc4\x04" or blob[i + 13] != 0xA3:
            continue
        td = struct.unpack_from("<I", blob, i + 1)[0]
        rel = struct.unpack_from("<i", blob, i + 6)[0]
        if base + i + 10 + rel != LOOKUP_RTYPE:
            continue
        out[struct.unpack_from("<I", blob, i + 14)[0]] = td
    return out


def type_name(pe: PE32, type_descriptor_va: int) -> str:
    """MSVC ``TypeDescriptor`` -> its mangled name, which starts 8 bytes in."""
    return pe.identifier(type_descriptor_va + 8, 512) or f"<td:{type_descriptor_va:#010x}>"


def read_function_starts() -> set[int]:
    """Every direct call target in the binary, from the existing call graph export.

    Used only as a tail-call oracle: an unconditional ``jmp`` whose target is a known
    function start ends the current function.
    """
    starts: set[int] = set()
    with open(CALLGRAPH, encoding="utf-8") as handle:
        for line in handle:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 3:
                continue
            starts.add(int(parts[0], 16))
            for callee in parts[2].split(","):
                # Import thunks are recorded as "EXTERNAL:<ordinal>"; skip those.
                if callee and not callee.startswith("EXTERNAL:"):
                    starts.add(int(callee, 16))
    return starts


# ======================================================================================
# serializer discovery
# ======================================================================================


@dataclass
class Serializer:
    serializer_class: str          # e.g. "Moho::UnitSerializer"
    subject_class: str             # e.g. "Moho::Unit"
    vtable_va: int
    instance_va: int | None = None
    load_fn: int | None = None
    save_fn: int | None = None
    body_va: int | None = None     # after following the forwarder chain
    status: str = "unresolved"


HELPER_RE = re.compile(r"^\.\?AU\?\$SerSaveLoadHelper@(.+)@gpg@@$")


def subject_from_helper(mangled_base: str) -> str | None:
    """``.?AU?$SerSaveLoadHelper@VUnit@Moho@@@gpg@@`` -> ``Moho::Unit``.

    The template argument is a mangled type name with its leading tag letter (V class,
    U struct, W4 enum) attached; strip that and reverse the ``@``-separated scope.
    """
    match = HELPER_RE.match(mangled_base)
    if not match:
        return None
    arg = match.group(1)
    for tag in ("W4", "V", "U", "M", "H"):
        if arg.startswith(tag):
            arg = arg[len(tag):]
            break
    if "?$" in arg:            # a template instantiation; keep it verbatim
        return arg
    parts = [p for p in arg.split("@") if p]
    return "::".join(reversed(parts)) if parts else None


def read_serializer_vtables() -> list[Serializer]:
    rows: list[Serializer] = []
    with open(VTABLE_CLASSES, encoding="utf-8") as handle:
        header = handle.readline().rstrip("\n").split("\t")
        i_name, i_vt, i_base = (header.index(c)
                                for c in ("class_name", "vtable_va", "base_at_offset"))
        for line in handle:
            parts = line.rstrip("\n").split("\t")
            if not parts[i_name].endswith("Serializer"):
                continue
            subject = subject_from_helper(parts[i_base])
            if subject is None:
                continue
            rows.append(Serializer(serializer_class=parts[i_name],
                                   subject_class=subject,
                                   vtable_va=int(parts[i_vt], 16)))
    return rows


def resolve_instances(pe: PE32, serializers: list[Serializer]) -> None:
    stores = scan_mov_mem_imm32(pe)
    by_imm: dict[int, list[int]] = defaultdict(list)
    for dst, sites in stores.items():
        for _site, imm in sites:
            by_imm[imm].append(dst)

    for ser in serializers:
        candidates = sorted(set(by_imm.get(ser.vtable_va, [])))
        if len(candidates) != 1:
            continue
        inst = candidates[0]
        ser.instance_va = inst
        for off, attr in ((SERIALIZER_INSTANCE_LOAD_OFF, "load_fn"),
                          (SERIALIZER_INSTANCE_SAVE_OFF, "save_fn")):
            sites = stores.get(inst + off)
            if sites and len({imm for _s, imm in sites}) == 1:
                setattr(ser, attr, sites[0][1])


# ======================================================================================
# disassembly
# ======================================================================================

ADDR_LINE_RE = re.compile(r"^\s*[0-9a-f]+:")
BYTES_RE = re.compile(r"^\s*([0-9a-f]+):\s((?:[0-9a-f]{2} ?)+)\s*$")


@dataclass
class Insn:
    va: int
    size: int
    mnemonic: str
    operands: str

    def __str__(self) -> str:
        return f"{self.va:#010x}  {self.mnemonic} {self.operands}".rstrip()


class Disassembler:
    """objdump front end with a per-range cache.

    Parsing splits on TAB, never on whitespace: llvm-objdump pads the byte column to a
    fixed width and a 10-byte instruction fills it exactly, so a space-based split
    silently drops those instructions.  Any address-prefixed line inside the requested
    window that does not parse raises instead of being skipped -- a parser that
    silently drops what it cannot classify reports a clean, false negative.
    """

    def __init__(self, exe: str = EXE) -> None:
        self.exe = exe
        self._cache: dict[int, list[Insn]] = {}

    def fetch(self, starts: list[int], size: int = MAX_FUNC_BYTES) -> None:
        wanted = sorted(s for s in set(starts) if s not in self._cache)
        if not wanted:
            return
        for start in wanted:
            self._cache[start] = []
        lo, hi = min(wanted), max(wanted) + size
        proc = subprocess.Popen(
            ["objdump", "-d", "-M", "intel",
             f"--start-address={lo:#x}", f"--stop-address={hi:#x}", self.exe],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        assert proc.stdout is not None
        for line in proc.stdout:
            if not ADDR_LINE_RE.match(line):
                continue                       # banner, section header, symbol, "..."
            fields = line.rstrip("\n").split("\t")
            head = BYTES_RE.match(fields[0])
            if head is None or len(fields) < 2:
                raise ValueError(f"unparsed objdump line: {line!r}")
            va = int(head.group(1), 16)
            insn = Insn(va, len(head.group(2).split()), fields[1].strip(),
                        fields[2].strip() if len(fields) > 2 else "")
            for start in wanted:
                if start <= va < start + size:
                    self._cache[start].append(insn)
        proc.wait()

    def function(self, start: int, func_starts: set[int]) -> list[Insn]:
        self.fetch([start])
        return trim_to_function(self._cache[start], func_starts)


BRANCH_RE = re.compile(r"^0x([0-9a-f]+)")


def branch_target(insn: Insn) -> int | None:
    if not (insn.mnemonic.startswith("j") or insn.mnemonic == "call"):
        return None
    match = BRANCH_RE.match(insn.operands)
    return int(match.group(1), 16) if match else None


def trim_to_function(insns: list[Insn], func_starts: set[int]) -> list[Insn]:
    """Linear sweep to the function's last instruction.

    A ``ret``/tail-``jmp``/``int3`` ends the function once no seen branch can still
    land after it.  A ``jmp`` is a tail call (and therefore an end) when its target is
    a known function start; otherwise it is an intra-function branch and extends the
    reach.  Conditional jumps always extend the reach.
    """
    reach = 0
    body: list[Insn] = []
    for insn in insns:
        target = branch_target(insn)
        tail_call = (insn.mnemonic == "jmp"
                     and (target is None or target in func_starts))
        if insn.mnemonic != "call" and target is not None and not tail_call:
            reach = max(reach, target)
        body.append(insn)
        if insn.va < reach:
            continue
        if insn.mnemonic.startswith("ret") or tail_call:
            break
        if insn.mnemonic == "int3":
            body.pop()
            break
    return body


# ======================================================================================
# taint simulation
#
# One abstract value matters: OBJ (arg2 of the save entry point) and pointers derived
# from it by address arithmetic.  ARCHIVE (arg1) is tracked purely as a cross-check.
# ======================================================================================

MEM_RE = re.compile(
    r"(?:(?P<width>byte|word|dword|qword|tbyte|xmmword) ptr )?"
    r"\[(?P<base>e[a-z][a-z])"
    r"(?:\s*\+\s*(?P<index>e[a-z][a-z])(?:\s*\*\s*(?P<scale>\d+))?)?"
    r"(?:\s*(?P<sign>[+-])\s*(?P<disp>0x[0-9a-f]+|\d+))?\]")

# First operand is written by these; everything else with a memory first operand
# (fld, push, cmp, test, fstp is special-cased) only reads it.
WRITE_MNEMONICS = {
    "mov", "movzx", "movsx", "movsd", "movss", "movaps", "movdqa", "movdqu", "lea",
    "add", "sub", "and", "or", "xor", "adc", "sbb", "inc", "dec", "neg", "not",
    "shl", "shr", "sar", "rol", "ror", "imul", "pop", "setne", "sete", "setl",
    "setg", "setle", "setge", "seta", "setb", "setae", "setbe", "cmovne", "cmove",
    "cmovl", "cmovg", "cmovle", "cmovge", "xchg", "fstp", "fst", "fistp", "fisttp",
    "cdq", "cwde", "movd", "movq", "sbb", "bt", "bts", "btr", "shld", "shrd",
}
# Instructions that clobber a register without an explicit destination operand.
IMPLICIT_CLOBBER = {"cdq": ("edx",), "cwde": ("eax",), "div": ("eax", "edx"),
                    "idiv": ("eax", "edx"), "mul": ("eax", "edx")}
CALL_CLOBBER = ("eax", "ecx", "edx")


@dataclass
class Access:
    """One reference to a member, in emission order."""
    offset: int
    insn_index: int
    access: str        # mnemonic
    width: str
    indexed: bool
    in_loop: bool = False   # written inside a backward branch -> an ARRAY, not a scalar
    via: str = "?"
    type_name: str = ""


@dataclass
class Trace:
    accesses: list[Access] = field(default_factory=list)
    calls: list[tuple[int, int, bool]] = field(default_factory=list)
    # (insn index, target, obj_is_live_at_call)
    string_pushes: list[int] = field(default_factory=list)
    archive_ecx_calls: int = 0     # Write() calls entered with ecx == ARCHIVE
    archive_other_calls: int = 0   # ... with ecx something else -> convention wrong
    unhandled: Counter = field(default_factory=Counter)
    vtable_slots: dict[int, set[str]] = field(default_factory=dict)


class State:
    """Register / stack-slot taint.

    Stack slots are keyed ``(epoch, offset)``: ``offset`` counts bytes below the
    function's entry ESP, and ``epoch`` bumps on ``and esp, imm`` (which makes the
    absolute value unknown but keeps all *subsequent* offsets mutually consistent).
    """

    def __init__(self) -> None:
        self.regs: dict[str, tuple[str, int] | None] = {r: None for r in GP_REGS}
        self.stack: dict[tuple[int, int], tuple[str, int] | None] = {}
        self.esp = 0
        self.epoch = 0
        self.ebp_frame: tuple[int, int] | None = None   # (epoch, esp) at "mov ebp,esp"

    def clone(self) -> "State":
        new = State()
        new.regs = dict(self.regs)
        new.stack = dict(self.stack)
        new.esp, new.epoch, new.ebp_frame = self.esp, self.epoch, self.ebp_frame
        return new

    def slot(self, base: str, disp: int) -> tuple[int, int] | None:
        if base == "esp":
            return (self.epoch, self.esp + disp)
        if base == "ebp" and self.ebp_frame is not None and self.regs["ebp"] is None:
            return (self.ebp_frame[0], self.ebp_frame[1] + disp)
        return None


def entry_state() -> State:
    """At the save entry point: [esp]=ret, [esp+4]=arg1 (archive), [esp+8]=arg2 (obj)."""
    st = State()
    st.stack[(0, 4)] = ("ARCHIVE", 0)
    st.stack[(0, 8)] = ("OBJ", 0)
    return st


def parse_operands(operands: str) -> list[str]:
    """Split the operand list on top-level commas (memory operands contain none)."""
    return [p.strip() for p in operands.split(",")] if operands else []


def simulate(insns: list[Insn], state: State, rtype_at: dict[int, str],
             pe: PE32) -> Trace:
    """Walk one function body linearly, recording every OBJ-relative reference."""
    trace = Trace()
    slot_at: dict[int, tuple[str, int]] = {}   # insn index -> (dst reg, vtable slot)

    for idx, insn in enumerate(insns):
        mnem, ops = insn.mnemonic, insn.operands
        parts = parse_operands(ops)
        mem = MEM_RE.search(ops)

        # ---- record OBJ-relative memory references -----------------------------
        if mem:
            base_t = state.regs.get(mem.group("base"))
            idx_t = state.regs.get(mem.group("index")) if mem.group("index") else None
            tainted = base_t if (base_t and base_t[0] == "OBJ") else None
            if tainted is None and idx_t and idx_t[0] == "OBJ":
                tainted = idx_t
            if tainted is not None:
                disp = 0
                if mem.group("disp"):
                    d = mem.group("disp")
                    disp = int(d, 16) if d.startswith("0x") else int(d)
                    if mem.group("sign") == "-":
                        disp = -disp
                trace.accesses.append(Access(
                    offset=tainted[1] + disp, insn_index=idx, access=mnem,
                    width=mem.group("width") or "",
                    indexed=bool(mem.group("index"))))

        # ---- a bare OBJ register used as a value is an access at its current disp
        if mnem == "push" and ops in state.regs and (state.regs[ops] or ("",))[0] == "OBJ":
            trace.accesses.append(Access(offset=state.regs[ops][1], insn_index=idx,
                                         access="push", width="", indexed=False))
        if mnem == "push":
            m = re.match(r"^0x([0-9a-f]+)$", ops)
            if m:
                va = int(m.group(1), 16)
                section = pe.section_of(va)
                if section and section.name == ".rdata" and pe.identifier(va, 64):
                    trace.string_pushes.append(va)

        # ---- vtable-slot bookkeeping (for "call R" through a WriteArchive slot) --
        if mnem == "mov" and len(parts) == 2 and parts[0] in GP_REGS:
            m = re.match(r"^(?:dword ptr )?\[(e[a-z][a-z])(?: \+ (0x[0-9a-f]+))?\]$",
                         parts[1])
            if m and (state.regs.get(m.group(1)) or ("",))[0] != "OBJ":
                slot_at[idx] = (parts[0],
                                int(m.group(2), 16) if m.group(2) else 0)

        # ---- calls --------------------------------------------------------------
        if mnem == "call":
            target = branch_target(insn)
            obj_live = (any(v and v[0] == "OBJ" for v in state.regs.values())
                        or any(v and v[0] == "OBJ" for v in state.stack.values()))
            trace.calls.append((idx, target if target is not None else -1, obj_live))
            if target == WRITE_ARCHIVE_WRITE:
                ecx = state.regs.get("ecx")
                if ecx and ecx[0] == "ARCHIVE":
                    trace.archive_ecx_calls += 1
                else:
                    trace.archive_other_calls += 1
            for reg in CALL_CLOBBER:
                state.regs[reg] = None
            continue

        # ---- state transfer -----------------------------------------------------
        transfer(state, insn, mnem, parts, mem, trace)

    attribute(insns, trace, rtype_at, slot_at)
    return trace


def transfer(state: State, insn: Insn, mnem: str, parts: list[str],
             mem: re.Match | None, trace: Trace) -> None:
    """Apply one instruction's effect on taint and on the stack pointer."""
    # Stack pointer arithmetic.
    if mnem == "push":
        state.esp -= 4
        src = parts[0] if parts else ""
        state.stack[(state.epoch, state.esp)] = state.regs.get(src) if src in GP_REGS else None
        return
    if mnem == "pop":
        dst = parts[0] if parts else ""
        if dst in GP_REGS:
            state.regs[dst] = state.stack.get((state.epoch, state.esp))
        state.esp += 4
        return
    if mnem in ("sub", "add") and parts and parts[0] == "esp":
        m = re.match(r"^(?:0x([0-9a-f]+)|(\d+))$", parts[1]) if len(parts) > 1 else None
        if m:
            amount = int(m.group(1), 16) if m.group(1) else int(m.group(2))
            state.esp += -amount if mnem == "sub" else amount
        else:
            trace.unhandled[f"esp {mnem} {parts[1]}"] += 1
        return
    if mnem == "and" and parts and parts[0] == "esp":
        state.epoch += 1
        state.esp = 0
        return
    if mnem == "mov" and len(parts) == 2 and parts[0] == "ebp" and parts[1] == "esp":
        state.ebp_frame = (state.epoch, state.esp)
        state.regs["ebp"] = None
        return
    if mnem == "mov" and len(parts) == 2 and parts[0] == "esp" and parts[1] == "ebp":
        if state.ebp_frame is not None:
            state.epoch, state.esp = state.ebp_frame
        return

    if mnem in IMPLICIT_CLOBBER:
        for reg in IMPLICIT_CLOBBER[mnem]:
            state.regs[reg] = None
        return

    if not parts:
        return
    dst = parts[0]

    # lea: address arithmetic -- taint survives and the displacement accumulates.
    if mnem == "lea" and len(parts) == 2 and dst in GP_REGS:
        m = MEM_RE.search(parts[1])
        base_t = state.regs.get(m.group("base")) if m else None
        if m and base_t and base_t[0] == "OBJ" and not m.group("index"):
            disp = 0
            if m.group("disp"):
                d = m.group("disp")
                disp = int(d, 16) if d.startswith("0x") else int(d)
                if m.group("sign") == "-":
                    disp = -disp
            state.regs[dst] = (base_t[0], base_t[1] + disp)
        else:
            state.regs[dst] = None
        return

    if mnem == "mov" and len(parts) == 2:
        src = parts[1]
        if dst in GP_REGS:
            if src in GP_REGS:
                state.regs[dst] = state.regs[src]
            elif "[" in src:
                m = MEM_RE.search(src)
                slot = state.slot(m.group("base"), _disp(m)) if m and not m.group("index") else None
                state.regs[dst] = state.stack.get(slot) if slot else None
            else:
                state.regs[dst] = None       # immediate
            return
        if "[" in dst:
            m = MEM_RE.search(dst)
            slot = state.slot(m.group("base"), _disp(m)) if m and not m.group("index") else None
            if slot is not None:
                state.stack[slot] = state.regs.get(src) if src in GP_REGS else None
            return
        return

    # add/sub of a constant to an OBJ pointer keeps it a member pointer.
    if mnem in ("add", "sub") and len(parts) == 2 and dst in GP_REGS:
        base_t = state.regs.get(dst)
        m = re.match(r"^(?:0x([0-9a-f]+)|(\d+))$", parts[1])
        if base_t and base_t[0] == "OBJ" and m:
            amount = int(m.group(1), 16) if m.group(1) else int(m.group(2))
            state.regs[dst] = (base_t[0], base_t[1] + (amount if mnem == "add" else -amount))
        else:
            state.regs[dst] = None
        return

    if mnem in ("cmp", "test", "fld", "fcom", "fcomp", "fucom", "fucomp", "nop",
                "jmp") or mnem.startswith("j"):
        return

    if mnem in WRITE_MNEMONICS and dst in GP_REGS:
        state.regs[dst] = None
        return
    if mnem in WRITE_MNEMONICS or mnem.startswith(("f", "set", "rep", "movs", "stos")):
        return
    if dst in GP_REGS:
        # Unknown mnemonic with a register destination: be loud, then be safe.
        trace.unhandled[mnem] += 1
        state.regs[dst] = None


def _disp(m: re.Match) -> int:
    if not m.group("disp"):
        return 0
    d = m.group("disp")
    value = int(d, 16) if d.startswith("0x") else int(d)
    return -value if m.group("sign") == "-" else value


RTYPE_LOOKBACK = 26
CALL_LOOKAHEAD = 30


def loop_ranges(insns: list[Insn]) -> list[tuple[int, int]]:
    """Backward-branch spans.

    A member written inside one is an ARRAY walked by a moving pointer (``add esi,4``);
    a linear sweep sees it once, so the row must say so rather than pass the array off
    as a single scalar.
    """
    out: list[tuple[int, int]] = []
    for insn in insns:
        target = branch_target(insn)
        if insn.mnemonic != "call" and target is not None and target <= insn.va:
            out.append((target, insn.va))
    return out


def attribute(insns: list[Insn], trace: Trace, rtype_at: dict[int, str],
              slot_at: dict[int, tuple[str, int]]) -> None:
    """Attach the consuming archive entry point (and hence a type) to each access.

    Types are attributed at the CALL, not at the load: MSVC hoists the lazy ``RType*``
    fetch to either side of the ``lea``, so only "nearest RType load before the
    consuming call" is stable.
    """
    loops = loop_ranges(insns)
    for acc in trace.accesses:
        va = insns[acc.insn_index].va
        acc.in_loop = any(lo <= va <= hi for lo, hi in loops)
        live_slots: dict[str, int] = {}
        for k in range(acc.insn_index + 1,
                       min(acc.insn_index + CALL_LOOKAHEAD, len(insns))):
            if k in slot_at:
                reg, slot = slot_at[k]
                live_slots[reg] = slot
            ahead = insns[k]
            if ahead.mnemonic != "call":
                continue
            target = branch_target(ahead)
            if target == LOOKUP_RTYPE:
                continue                       # lazy type-cache fill, not a consumer
            tname = nearest_rtype(rtype_at, k)
            if target == WRITE_ARCHIVE_WRITE:
                acc.via, acc.type_name = "Write(RType*,void*,RRef)", tname
            elif target == ARCHIVE_SER_POINTER:
                acc.via, acc.type_name = "SerPointer", tname
            elif target is not None:
                acc.via, acc.type_name = f"helper:{target:#010x}", tname
            else:
                reg = ahead.operands.strip()
                if reg in live_slots:
                    slot = live_slots[reg]
                    trace.vtable_slots.setdefault(slot, set()).add(acc.access)
                    acc.via = f"WriteArchive+{slot:#04x}"
                else:
                    acc.via = "call <indirect>"
            break


def nearest_rtype(rtype_at: dict[int, str], before: int) -> str:
    candidates = [k for k in rtype_at if before - RTYPE_LOOKBACK <= k < before]
    return rtype_at[max(candidates)] if candidates else ""


def index_rtype_loads(insns: list[Insn], rtype_globals: dict[int, int],
                      pe: PE32) -> dict[int, str]:
    out: dict[int, str] = {}
    for pos, insn in enumerate(insns):
        if insn.mnemonic != "mov":
            continue
        m = re.match(r"^e[a-z][a-z], (?:dword ptr )?\[(0x[0-9a-f]+)\]$", insn.operands)
        if m and int(m.group(1), 16) in rtype_globals:
            out[pos] = type_name(pe, rtype_globals[int(m.group(1), 16)])
    return out


# ======================================================================================
# forwarder chasing
# ======================================================================================


def analyse(dis: Disassembler, va: int, state: State, rtype_globals: dict[int, int],
            pe: PE32, func_starts: set[int], depth: int = 0
            ) -> tuple[int, list[Insn], Trace, str]:
    """Return (body VA, body listing, trace, status) for one save entry point.

    A function that touches no OBJ-relative memory is a forwarder; follow its single
    OBJ-carrying non-runtime call.  Anything else is the body.  A function with no
    accesses and no unique candidate call is reported as such rather than silently
    yielding an empty field list.
    """
    insns = dis.function(va, func_starts)
    rtype_at = index_rtype_loads(insns, rtype_globals, pe)
    trace = simulate(insns, state.clone(), rtype_at, pe)

    if trace.accesses:
        return va, insns, trace, "body"

    if depth >= MAX_FORWARDER_DEPTH:
        return va, insns, trace, "forwarder-depth-exhausted"

    # Tail jmp counts as a call for forwarding purposes.
    targets = [t for _i, t, live in trace.calls
               if t > 0 and live and t not in RUNTIME_CALLS]
    last = insns[-1] if insns else None
    if last is not None and last.mnemonic == "jmp":
        tgt = branch_target(last)
        if tgt and tgt not in RUNTIME_CALLS:
            targets.append(tgt)

    if not targets:
        return va, insns, trace, "no-members-no-forward"
    if len(set(targets)) > 1:
        return va, insns, trace, f"ambiguous-forward({len(set(targets))})"

    # Re-simulate up to the forwarding call to get the state handed to the callee.
    target = targets[0]
    handoff = state.clone()
    sub_trace = Trace()
    slot_at: dict[int, tuple[str, int]] = {}
    for insn in insns:
        if insn.mnemonic in ("call", "jmp") and branch_target(insn) == target:
            break
        if insn.mnemonic == "call":
            for reg in CALL_CLOBBER:
                handoff.regs[reg] = None
            continue
        transfer(handoff, insn, insn.mnemonic,
                 parse_operands(insn.operands), MEM_RE.search(insn.operands), sub_trace)
    # Callee's entry ESP sits one dword below ours for a `call`, same for a tail `jmp`.
    callee = handoff.clone()
    if not (insns and insns[-1].mnemonic == "jmp" and branch_target(insns[-1]) == target):
        callee.esp -= 4
    shift = callee.esp
    callee.stack = {(e, o - shift): v for (e, o), v in handoff.stack.items()
                    if e == handoff.epoch}
    callee.esp = 0
    callee.epoch = handoff.epoch
    callee.ebp_frame = None
    return analyse(dis, target, callee, rtype_globals, pe, func_starts, depth + 1)


# ======================================================================================
# output
# ======================================================================================

WIDTH_TO_TYPE = {
    ("fld", "dword"): "float", ("fld", "qword"): "double",
    ("movzx", "byte"): "u8/bool", ("movsx", "byte"): "i8",
    ("movzx", "word"): "u16", ("movsx", "word"): "i16",
    ("mov", "dword"): "u32/i32", ("mov", "byte"): "u8", ("mov", "word"): "u16",
    ("mov", "qword"): "u64", ("push", ""): "ptr",
}


def resolve_type(acc: Access, helper_types: dict[int, str]) -> str:
    """Type of one member.

    A tracked-pointer helper carries the pointee's own ``TypeDescriptor`` and must win
    over ``nearest_rtype``: the lazy ``RType*`` fetch nearest a helper call frequently
    belongs to the *previous* member (MSVC hoists it), so trusting it there silently
    mislabels pointers.  ``Write(RType*, ...)`` is the opposite case -- the RType is
    the argument, so the nearest fetch is authoritative.
    """
    if acc.via.startswith("helper:"):
        helper = int(acc.via.split(":")[1], 16)
        if helper in helper_types:
            return helper_types[helper] + "*"
    if acc.type_name:
        return acc.type_name
    if acc.access == "lea":
        return ""
    return WIDTH_TO_TYPE.get((acc.access, acc.width), "")


def dedup(accesses: list[Access]) -> list[tuple[Access, int]]:
    """One row per distinct offset, in first-emission order, with an occurrence count.

    A member referenced twice (a spill/reload, or an array walked in a loop) must not
    become two members; a member's *first* reference is its position in the record.
    """
    first: dict[int, Access] = {}
    count: Counter = Counter()
    order: list[int] = []
    for acc in accesses:
        if acc.offset not in first:
            first[acc.offset] = acc
            order.append(acc.offset)
        elif not first[acc.offset].type_name and acc.type_name:
            preserved = first[acc.offset].insn_index
            first[acc.offset] = acc
            first[acc.offset].insn_index = preserved
        count[acc.offset] += 1
    return [(first[o], count[o]) for o in order]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="build/re-fa/exports/serializer_members.tsv")
    ap.add_argument("--only", help="comma-separated subject classes to restrict to")
    ap.add_argument("--dump", action="store_true", help="print the body listing")
    ap.add_argument("--report-name-scan", action="store_true",
                    help="quantify the member-name negative and exit")
    args = ap.parse_args()

    pe = PE32(EXE)
    serializers = read_serializer_vtables()
    resolve_instances(pe, serializers)
    rtype_globals = scan_rtype_globals(pe)
    func_starts = read_function_starts()
    dis = Disassembler()

    resolved = [s for s in serializers if s.save_fn]
    if args.only:
        keep = set(args.only.split(","))
        resolved = [s for s in resolved if s.subject_class in keep]

    dis.fetch([s.save_fn for s in resolved])
    traces: dict[str, Trace] = {}
    listings: dict[str, list[Insn]] = {}
    for ser in resolved:
        body, insns, trace, status = analyse(dis, ser.save_fn, entry_state(),
                                             rtype_globals, pe, func_starts)
        ser.body_va, ser.status = body, status
        traces[ser.subject_class] = trace
        listings[ser.subject_class] = insns

    # Pointer helpers carry the pointee's TypeDescriptor; one more pass names them.
    helpers = sorted({int(a.via.split(":")[1], 16)
                      for t in traces.values() for a in t.accesses
                      if a.via.startswith("helper:")})
    dis.fetch(helpers, size=0x200)
    helper_types: dict[int, str] = {}
    for helper in helpers:
        for insn in dis.function(helper, func_starts)[:60]:
            m = re.match(r"^e[a-z][a-z], (?:dword ptr )?\[(0x[0-9a-f]+)\]$",
                         insn.operands)
            if insn.mnemonic == "mov" and m and int(m.group(1), 16) in rtype_globals:
                helper_types[helper] = type_name(pe, rtype_globals[int(m.group(1), 16)])
                break

    if args.dump:
        for ser in resolved:
            print(f"### {ser.subject_class}  entry={ser.save_fn:#010x} "
                  f"body={ser.body_va:#010x} status={ser.status}")
            for insn in listings[ser.subject_class]:
                print("   ", insn)
        return 0

    if args.report_name_scan:
        return report_name_scan(pe, traces, listings)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", encoding="utf-8") as handle:
        handle.write("class\tmember_index\tmember_name\toffset\ttype\taccess\tvia\t"
                     "occurrences\tindexed\tin_loop\tbody_va\tstatus\n")
        for ser in sorted(resolved, key=lambda s: s.subject_class):
            rows = dedup(traces[ser.subject_class].accesses)
            for index, (acc, count) in enumerate(rows):
                handle.write("\t".join([
                    ser.subject_class, str(index),
                    "",                       # no name exists on this path
                    f"{acc.offset:#06x}",
                    resolve_type(acc, helper_types),
                    f"{acc.access} {acc.width}".strip(),
                    acc.via, str(count), "1" if acc.indexed else "0",
                    "1" if acc.in_loop else "0",
                    f"{ser.body_va:#010x}", ser.status,
                ]) + "\n")

    parsed = [s for s in resolved if traces[s.subject_class].accesses]
    total = sum(len(dedup(t.accesses)) for t in traces.values())
    typed = sum(1 for t in traces.values() for acc, _ in dedup(t.accesses)
                if resolve_type(acc, helper_types))
    print(f"serializer vtables (SerSaveLoadHelper<T>) : {len(serializers)}")
    print(f"  singleton resolved                      : "
          f"{sum(1 for s in serializers if s.instance_va)}")
    print(f"  SerSave entry resolved                  : "
          f"{sum(1 for s in serializers if s.save_fn)}")
    print(f"  body located, >=1 member                : {len(parsed)}")
    print(f"distinct members emitted                  : {total}")
    print(f"  with a recovered type                   : {typed} "
          f"({100.0 * typed / max(total, 1):.1f}%)")
    print(f"  with a recovered NAME                   : 0 (see --report-name-scan)")
    status = Counter(s.status for s in resolved)
    print("status:", ", ".join(f"{k}={v}" for k, v in status.most_common()))
    ecx_ok = sum(t.archive_ecx_calls for t in traces.values())
    ecx_bad = sum(t.archive_other_calls for t in traces.values())
    print(f"convention cross-check: Write() calls with ecx==arg1 : {ecx_ok} ok, "
          f"{ecx_bad} mismatched")
    unhandled: Counter = Counter()
    for t in traces.values():
        unhandled += t.unhandled
    print("unhandled instruction forms:",
          ", ".join(f"{k}x{v}" for k, v in unhandled.most_common(12)) or "none")
    slots: dict[int, set[str]] = defaultdict(set)
    for t in traces.values():
        for slot, acc in t.vtable_slots.items():
            slots[slot] |= acc
    print("WriteArchive vtable slots observed:",
          ", ".join(f"{s:#04x}={'/'.join(sorted(a))}" for s, a in sorted(slots.items())))
    print(f"wrote {out}")
    return 0


def report_name_scan(pe: PE32, traces: dict[str, Trace],
                     listings: dict[str, list[Insn]]) -> int:
    """Quantify the 'serializers carry no member names' negative."""
    pushes = sum(len(t.string_pushes) for t in traces.values())
    distinct = {va for t in traces.values() for va in t.string_pushes}
    print(f"MemberSave bodies scanned                       : {len(traces)}")
    print(f"total instructions in those bodies              : "
          f"{sum(len(v) for v in listings.values())}")
    print(f".rdata string constants pushed anywhere in them : {pushes} "
          f"({len(distinct)} distinct)")
    for va in sorted(distinct):
        print(f"    {va:#010x}  {pe.identifier(va, 96)!r}")
    print("\nControl: a member-name registry would leave field-name strings behind.")
    hits = 0
    with open("build/re-fa/exports/strings.all.tsv", encoding="utf-8") as handle:
        handle.readline()
        for line in handle:
            text = line.rstrip("\n").split("\t")[-1]
            if re.match(r"^m[A-Z][A-Za-z0-9]{2,}$", text):
                hits += 1
                print(f"    m-prefixed identifier string: {text!r}")
    print(f"  strings matching ^m[A-Z][A-Za-z0-9]+$ in the whole binary: {hits}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
