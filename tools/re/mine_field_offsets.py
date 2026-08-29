#!/usr/bin/env python3
"""Bulk-mine C++ object field offsets out of the retail Forged Alliance executable.

WHY THIS WORKS AT ALL
---------------------
Every Lua-visible engine method is a small generated binding function.  Its shape is
always the same:

    <arity check>                       ; complain if the script passed the wrong count
    call  <receiver resolver>           ; turn the Lua `_c_object` userdata into a C++ this
    mov   %eax, %esi                    ; park the object pointer in a callee-saved register
    ...
    flds  0x90(%esi)                    ; <-- THE FIELD.  one instruction, one offset.
    ...
    call  <lua push helper>             ; hand the value back to the script

So a getter like `Entity:GetHealth` reveals `Entity+0x90` mechanically.  There are 797
such bindings with a known native address in `moho.methods.annotated.tsv`, and each one
carries its own *name*, *Lua signature* and often the engine's own *documentation string*.
That is a named, typed field offset per function, at scale — which is what this script
harvests.

THE BASE POINTER IS READ, NOT GUESSED
-------------------------------------
The campaign has already been burned once by recording an offset without saying what it
was relative to (`C-038` -> `C-053`: `Unit+0x98` and `Entity+0x90` are the same field,
because the `Entity` subobject sits at `Unit+0x08`).  An offset without a base is not a
fact.

Fortunately the binary states the base outright.  Each receiver resolver is a separate
template instantiation per C++ type, and it contains a literal pointer to the MSVC
`TypeDescriptor` for that type:

    push  $0xfcbc48                     ; -> TypeDescriptor
    call  0x94ef10                      ; the "look up / cache the type" helper

and at `0xfcbc48 + 8` the file literally contains the mangled name `.?AVEntity@Moho@@`.
So the class name behind every offset in this table was *read out of the executable*, not
inferred from the Lua scope name.  Where a Lua class inherits (a `Unit` method whose
receiver resolves as `Moho::Entity`), that shows up here as a disagreement between the Lua
scope and the resolved base class — and the resolved base class wins.

WHAT IS READ VS WHAT IS INFERRED
--------------------------------
  read     : the offset, the access width, the load/store direction, the base class name
  inferred : what the field *means* (taken from the method's own name and doc string)
  inferred : `confidence`, from how many distinct offsets the function touches

Read-only.  Never executes the artifact.  Takes no Ghidra project lock.

Usage:
    python3 tools/re/mine_field_offsets.py \
        --exe ~/projects/llm/input/recoil-metal/retail-fa/bin/SupremeCommander.exe \
        --methods build/re-fa/exports/moho.methods.annotated.tsv \
        --out build/re-fa/exports/field_offsets.tsv
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from typing import Iterable, Optional

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_reader import PE32  # noqa: E402

# --------------------------------------------------------------------------------------
# Constants derived from this specific binary.  Each one is justified, per project rules.
# --------------------------------------------------------------------------------------

# The MSVC "get_or_cache_type_info" helper.  Every per-type receiver resolver calls it with
# a pointer to that type's TypeDescriptor.  Found by disassembling the two resolvers used by
# Entity:GetHealth (0x5aeaf0) and Unit:GetHealth (0x59a190): both push a distinct constant
# and call this same address, and those constants dereference to ".?AVEntity@Moho@@" and
# ".?AVUnit@Moho@@" respectively.
TYPE_INFO_HELPER_VA = 0x0094EF10

# MSVC TypeDescriptor layout: void* pVFTable; void* spare; char name[].
TYPE_DESCRIPTOR_NAME_OFFSET = 8

# How far to disassemble looking for the end of a function before giving up and widening.
# Lua bindings are small; 0x400 covers the overwhelming majority in one objdump call.
WINDOW_STEPS = (0x400, 0x1200, 0x4000)

# A resolver is a short helper; searching this far into it is enough to find its type push.
RESOLVER_SCAN_BYTES = 0x200

# Registers the MSVC calling conventions used here (cdecl / thiscall) preserve across a
# call.  A `this` pointer parked in one of these survives intervening calls; one parked in
# eax/ecx/edx does not, so tracking is dropped at the next call in that case.
CALLEE_SAVED = {"%ebx", "%esi", "%edi", "%ebp"}

# Instructions that read a memory operand without writing it, even though the memory
# operand is syntactically last in AT&T order.
READ_ONLY_MNEMONICS = {"cmp", "cmpb", "cmpw", "cmpl", "test", "testb", "testw", "testl"}

# Access width / C type implied purely by the instruction mnemonic.  This is machine
# evidence about the field's size; the *semantic* type (bool vs byte, ptr vs int) is
# refined later from the method name and from whether the loaded value is dereferenced.
TYPE_BY_MNEMONIC = {
    "flds": "float", "fsts": "float", "fstps": "float",
    "fldl": "double", "fstl": "double", "fstpl": "double",
    "fildl": "i32", "fistpl": "i32",
    "fildll": "i64", "fistpll": "i64",
    "movss": "float", "movsd": "double",
    "movl": "u32", "movb": "u8", "movw": "u16",
    "movzbl": "u8", "movsbl": "i8", "movzwl": "u16", "movswl": "i16",
    "cmpl": "u32", "cmpb": "u8", "cmpw": "u16",
    "testl": "u32", "testb": "u8", "testw": "u16",
    "addl": "u32", "subl": "u32", "orl": "u32", "andl": "u32", "xorl": "u32",
    "incl": "u32", "decl": "u32", "notl": "u32", "negl": "u32",
    "pushl": "u32", "leal": "addr", "btl": "u32", "btsl": "u32", "btrl": "u32",
    "imull": "u32", "shll": "u32", "shrl": "u32", "sarl": "u32",
    "faddp": "float", "fmuls": "float", "fadds": "float", "fsubs": "float",
    "fcomps": "float", "fcoms": "float", "fdivs": "float",
    "movaps": "float4", "movups": "float4", "movlps": "float2",
    # SSE scalar arithmetic reading the field directly: the operand is a 32-bit float.
    "mulss": "float", "addss": "float", "subss": "float", "divss": "float",
    "ucomiss": "float", "comiss": "float", "maxss": "float", "minss": "float",
    # `cvtsi2ss D(%this),%xmm` converts a 32-bit *integer* field to float, so the field is an
    # int; `cvttss2si` reads a float.  Getting these the wrong way round would mislabel every
    # enum-valued field the engine converts for Lua.
    "cvtsi2ssl": "int32", "cvtsi2sdl": "int32",
    "cvttss2si": "float", "cvtss2sd": "float",
}

# Accessor name prefixes.  Their presence is what makes a binding a *simple* accessor and
# therefore a high-confidence single-field read.
GETTER_PREFIXES = ("Get", "Is", "Has", "Can", "Are", "Was", "Should", "Test")
SETTER_PREFIXES = ("Set", "Toggle", "Change", "Enable", "Disable", "Add", "Remove",
                   "Reset", "Clear")

# --------------------------------------------------------------------------------------
# Disassembly plumbing
# --------------------------------------------------------------------------------------

# llvm-objdump AT&T line:  "  693e48: d9 86 90 00 00 00   \tflds\t0x90(%esi)"
#
# Parsed by splitting on TAB rather than by matching the byte column, and that detail is
# load-bearing.  llvm-objdump pads the byte column to a fixed width, but an instruction long
# enough to fill it (10 bytes — exactly the encoding of `orl $imm32, disp32(%reg)`) gets no
# padding space before the tab.  A regex that required "pairs each followed by a space"
# silently dropped every such line, which is to say it dropped precisely the 64-bit
# unit-state bit-mask writes this sweep most wants to see.
LINE_RE = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2}[ \t])+[0-9a-f]{2}|[0-9a-f]{2})\s*$")

# A memory operand.  Group 1 = displacement (may be absent -> 0), group 2 = base register,
# group 3 = the index part if this is a scaled/indexed access (an array, not a plain field).
MEM_RE = re.compile(r"(-?0x[0-9a-f]+)?\((%e[a-z]{2})((?:,\s*%e[a-z]{2}(?:,\s*\d)?)?)\)")

REG_RE = re.compile(r"%e[a-z]{2}")


@dataclass
class Insn:
    va: int
    raw: str
    mnemonic: str
    operands: str

    @property
    def text(self) -> str:
        return f"{self.mnemonic} {self.operands}".strip()


class Disassembler:
    """Thin, cached wrapper around `objdump -d` over an address range.

    objdump is used rather than Ghidra deliberately: it needs no project lock, so this can
    run while other analysis sessions hold the Ghidra database, and Ghidra's decompiler
    returns empty bodies for functions this small anyway.
    """

    def __init__(self, exe_path: str):
        self.exe_path = exe_path
        self._cache: dict[tuple[int, int], list[Insn]] = {}

    def range(self, start: int, length: int) -> list[Insn]:
        key = (start, length)
        if key in self._cache:
            return self._cache[key]
        result = subprocess.run(
            ["objdump", "-d",
             f"--start-address=0x{start:x}", f"--stop-address=0x{start + length:x}",
             self.exe_path],
            capture_output=True, text=True, check=False)
        insns: list[Insn] = []
        for line in result.stdout.splitlines():
            fields = line.split("\t")
            head = LINE_RE.match(fields[0])
            if not head or len(fields) < 2:
                continue
            va = int(head.group(1), 16)
            if va < start:
                continue
            operands = fields[2].strip() if len(fields) > 2 else ""
            # Drop llvm-objdump's trailing "# imm = 0x400" annotation.
            operands = operands.split(" # ")[0].strip()
            insns.append(Insn(va, head.group(2).strip(), fields[1].strip(), operands))
        self._cache[key] = insns
        return insns

    def function(self, start: int) -> list[Insn]:
        """Instructions from `start` to the end of its function.

        End detection: a `ret` ends the function unless some earlier conditional or
        unconditional jump targets an address *beyond* it — that would mean the ret is a
        mid-function early exit and there is more body to come.  Widen the window and retry
        if no end is found, so that a long binding is not silently truncated.
        """
        for window in WINDOW_STEPS:
            insns = self.range(start, window)
            body = self._truncate_at_end(insns)
            if body is not None:
                return body
        # Nothing looked like a clean end; return the widest read so far and let the caller
        # downgrade confidence rather than pretending the body was understood.
        return self.range(start, WINDOW_STEPS[-1])

    @staticmethod
    def _truncate_at_end(insns: list[Insn]) -> Optional[list[Insn]]:
        furthest_target = 0
        for index, insn in enumerate(insns):
            if insn.mnemonic.startswith("j"):
                target = re.match(r"^(0x[0-9a-f]+)", insn.operands)
                if target:
                    furthest_target = max(furthest_target, int(target.group(1), 16))
            if insn.mnemonic in ("retl", "ret", "retw") or insn.mnemonic.startswith("retl"):
                if furthest_target <= insn.va:
                    return insns[:index + 1]
        return None


# --------------------------------------------------------------------------------------
# Receiver-resolver identification: turning a call target into a C++ class name
# --------------------------------------------------------------------------------------

def demangle_type_descriptor(raw: str) -> str:
    """`.?AVEntity@Moho@@` -> `Moho::Entity`.

    Only the tiny subset of MSVC name mangling that appears in type descriptors is handled:
    a leading `.?AV` (class) or `.?AU` (struct), then namespace-qualified components in
    *reverse* order, terminated by `@@`.
    """
    name = raw
    for prefix in (".?AV", ".?AU", ".?AW", ".?AT"):
        if name.startswith(prefix):
            name = name[len(prefix):]
            break
    # `.?AW4EUnitState@Moho@@` — MSVC prefixes enum names with the digit 4.
    if name[:1].isdigit():
        name = name[1:]
    name = name.rstrip("@")
    parts = [part for part in name.split("@") if part]
    return "::".join(reversed(parts)) if parts else raw


class ResolverMap:
    """Maps `call <addr>` targets to the C++ class the callee returns.

    A receiver resolver is recognised structurally, not by a hardcoded list: disassemble the
    call target and look for `push $IMM` followed shortly by a call to the type-info helper,
    where IMM points at something that reads as an MSVC type descriptor.  That makes the map
    self-extending — any new per-type resolver in the binary is picked up for free.
    """

    def __init__(self, pe: PE32, disasm: Disassembler):
        self.pe = pe
        self.disasm = disasm
        self._cache: dict[int, Optional[str]] = {}

    def class_of(self, target_va: int) -> Optional[str]:
        if target_va in self._cache:
            return self._cache[target_va]
        self._cache[target_va] = self._probe(target_va)
        return self._cache[target_va]

    def _probe(self, target_va: int) -> Optional[str]:
        if not self.pe.is_code(target_va):
            return None
        insns = self.disasm.range(target_va, RESOLVER_SCAN_BYTES)
        pending_immediate: Optional[int] = None
        for insn in insns:
            if insn.mnemonic == "pushl" and insn.operands.startswith("$"):
                literal = re.match(r"^\$(0x[0-9a-f]+|-?\d+)", insn.operands)
                if literal:
                    pending_immediate = int(literal.group(1), 0)
            elif insn.mnemonic.startswith("call") and pending_immediate is not None:
                callee = re.match(r"^(0x[0-9a-f]+)", insn.operands)
                if callee and int(callee.group(1), 16) == TYPE_INFO_HELPER_VA:
                    name = self.pe.identifier(
                        pending_immediate + TYPE_DESCRIPTOR_NAME_OFFSET, 160)
                    if name and name.startswith(".?A"):
                        # `.?AW4Foo@Moho@@` is an *enum*.  Its "resolver" converts a Lua
                        # string or number into a scalar, so anything read through the
                        # returned pointer is a value, not a field.  Recording those would
                        # invent object layouts for types that have none.
                        if name.startswith(".?AW"):
                            return None
                        return demangle_type_descriptor(name)
                pending_immediate = None
        return None


# --------------------------------------------------------------------------------------
# Field-access extraction
# --------------------------------------------------------------------------------------

@dataclass
class Access:
    """One `[base + offset]` touch attributed to a known C++ class."""
    base_class: str
    offset: int
    chain: tuple[int, ...]      # () for a direct field; (0x14c,) means "through +0x14c"
    kind: str                   # "load" | "store" | "read" (compare/test) | "addr" (lea)
    ctype: str                  # width implied by the instruction mnemonic
    mnemonic: str
    va: int
    indexed: bool               # True when scaled/indexed => an array, not a plain field
    immediate: Optional[int]    # the immediate of a test/cmp/and/or, for bit masks
    vcall: bool = False         # the loaded value is the target of an indirect call


@dataclass
class MethodResult:
    accesses: list[Access] = field(default_factory=list)
    resolver_classes: list[str] = field(default_factory=list)
    body_length: int = 0
    truncated: bool = False
    # (callee_va, class) for every `mov <this>,%ecx ; call callee` — i.e. the real native
    # method the generated binding forwards to.  Recovering these is worthwhile on its own:
    # it names engine methods that no export table or RTTI entry points at.
    thiscall_targets: list[tuple[int, str]] = field(default_factory=list)


def _memory_operands(insn: Insn) -> list[tuple[Optional[int], str, bool, bool]]:
    """(displacement, base_register, is_indexed, is_last_operand) for each memory operand."""
    found = []
    for match in MEM_RE.finditer(insn.operands):
        disp = int(match.group(1), 16) if match.group(1) else 0
        found.append((disp, match.group(2), bool(match.group(3)),
                      match.end() >= len(insn.operands.rstrip())))
    return found


def _written_register(insn: Insn) -> Optional[str]:
    """The GPR this instruction writes, if the last operand is a bare register.

    AT&T puts the destination last.  Compares and tests are excluded because they write only
    flags, and pushes because they write memory.
    """
    if insn.mnemonic in READ_ONLY_MNEMONICS or insn.mnemonic.startswith("push"):
        return None
    operands = [part.strip() for part in insn.operands.split(",")]
    if not operands:
        return None
    last = operands[-1]
    return last if REG_RE.fullmatch(last) else None


def _immediate_of(insn: Insn) -> Optional[int]:
    match = re.search(r"\$(0x[0-9a-f]+|-?\d+)", insn.operands)
    return int(match.group(1), 0) if match else None


def _esp_change(insn: Insn) -> Optional[int]:
    """How this instruction moves `%esp`, or None if it does not touch it.

    Needed because the generated bindings routinely park the resolved receiver in a stack
    slot and reload it much later, so following the object requires knowing which slot is
    which.  `and $-8,%esp` realigns to an amount not knowable statically; the convention here
    is to treat that as the frame origin, which is correct because it only ever appears in
    the prologue, before any spill.
    """
    if insn.mnemonic in ("pushl", "pushfl"):
        return -4
    if insn.mnemonic in ("popl", "popfl"):
        return 4
    if not insn.operands.endswith("%esp"):
        return None
    literal = re.match(r"^\$(-?0x[0-9a-f]+|-?\d+),", insn.operands)
    if insn.mnemonic == "subl" and literal:
        return -int(literal.group(1), 0)
    if insn.mnemonic == "addl" and literal:
        return int(literal.group(1), 0)
    return "reset"      # andl/movl into %esp: frame realignment or teardown


def analyse_method(method_va: int, disasm: Disassembler, resolvers: ResolverMap,
                   seed: Optional[tuple[str, str]] = None) -> MethodResult:
    """Walk one Lua binding and record every field touch on a resolved receiver.

    The tracking model is deliberately small and conservative:

      * `tracked[reg] = (class, chain)` means that register currently holds a pointer
        obtained from a resolver of `class`, reached by following `chain` offsets.
      * A load `mov D(%tracked), %r` records an access AND (because it is pointer-width)
        propagates tracking to `%r` one level deeper, which is what recovers two-level facts
        like "the army index lives at +0x08 of the pointer at Entity+0x14c".
      * Any other write to a tracked register drops it.  A call drops tracked *volatile*
        registers, since eax/ecx/edx are not preserved.

    Anything the model cannot follow simply produces no row, which is the correct failure
    mode here: a missing offset costs nothing, a mislabelled one poisons the table.
    """
    result = MethodResult()
    insns = disasm.function(method_va)
    result.body_length = len(insns)
    result.truncated = bool(insns) and not insns[-1].mnemonic.startswith("ret")

    # `seed` lets the caller say "on entry, register R already holds a C" — used to mine the
    # real native method a binding forwards to, where `this` arrives in ECX.
    tracked: dict[str, tuple[str, tuple[int, ...]]] = {}
    if seed is not None:
        tracked[seed[0]] = (seed[1], ())
    # Stack slots holding a tracked object, keyed by offset from the realigned frame base.
    spilled: dict[int, tuple[str, tuple[int, ...]]] = {}
    esp_delta = 0
    pending_class: Optional[str] = None      # class returned by the call we just passed
    pending_countdown = 0                    # how many more instructions eax may still hold it
    # Registers holding a value freshly loaded through a tracked pointer, so that a later
    # `call *%reg` can be attributed back to the slot it came from.  That is what turns
    # `mov (%esi),%edx; mov 0x40(%edx),%eax; call *%eax` into "vtable slot +0x40".
    loaded_into: dict[str, Access] = {}

    for index, insn in enumerate(insns):
        # --- an indirect call through a previously loaded slot -------------------------
        if insn.mnemonic.startswith("call") and insn.operands.startswith("*%"):
            callee_reg = insn.operands[1:].strip()
            if callee_reg in loaded_into:
                loaded_into[callee_reg].vcall = True

        # --- record accesses through any currently tracked pointer ---------------------
        for disp, base_reg, indexed, is_last in _memory_operands(insn):
            if base_reg not in tracked:
                continue
            owner_class, chain = tracked[base_reg]
            if insn.mnemonic.startswith("call"):
                kind = "vcall"
            elif insn.mnemonic in READ_ONLY_MNEMONICS:
                kind = "read"
            elif insn.mnemonic == "leal":
                kind = "addr"
            elif insn.mnemonic.startswith("fst") or insn.mnemonic.startswith("fist"):
                kind = "store"
            elif insn.mnemonic.startswith("fld") or insn.mnemonic.startswith("fild"):
                kind = "load"
            elif is_last and not insn.mnemonic.startswith("push"):
                kind = "store"
            else:
                kind = "load"
            access = Access(
                base_class=owner_class,
                offset=disp,
                chain=chain,
                kind=kind,
                ctype=TYPE_BY_MNEMONIC.get(insn.mnemonic, insn.mnemonic),
                mnemonic=insn.mnemonic,
                va=insn.va,
                indexed=indexed,
                immediate=_immediate_of(insn) if insn.mnemonic in READ_ONLY_MNEMONICS
                or insn.mnemonic in ("andl", "orl", "xorl", "btl") else None,
            )
            result.accesses.append(access)
            # Remember where the loaded value went, so an indirect call on it can be
            # attributed back to this slot.
            if kind == "load":
                destination = insn.operands.split(",")[-1].strip()
                if REG_RE.fullmatch(destination):
                    loaded_into[destination] = access

        # --- spills to the stack frame -------------------------------------------------
        # (the matching reload is handled below, with the other tracking propagation)
        reloaded: Optional[tuple[str, tuple[int, ...]]] = None
        if insn.mnemonic == "movl":
            source, _, destination = insn.operands.partition(",")
            source, destination = source.strip(), destination.strip()
            spill = MEM_RE.fullmatch(destination)
            if spill and spill.group(2) == "%esp" and source in tracked:
                slot = (int(spill.group(1), 16) if spill.group(1) else 0) + esp_delta
                spilled[slot] = tracked[source]
            reload_match = MEM_RE.fullmatch(source)
            if reload_match and reload_match.group(2) == "%esp":
                slot = ((int(reload_match.group(1), 16) if reload_match.group(1) else 0)
                        + esp_delta)
                reloaded = spilled.get(slot)

        # --- the real native method this binding forwards to ---------------------------
        # `mov <this>,%ecx ; call TARGET` is MSVC thiscall.  Recording it names TARGET.
        if (insn.mnemonic.startswith("call") and "%ecx" in tracked
                and index > 0 and insns[index - 1].operands.endswith("%ecx")):
            callee = re.match(r"^(0x[0-9a-f]+)", insn.operands)
            if callee:
                result.thiscall_targets.append(
                    (int(callee.group(1), 16), tracked["%ecx"][0]))

        # --- keep the frame pointer honest ---------------------------------------------
        change = _esp_change(insn)
        if change == "reset":
            esp_delta = 0
            spilled.clear()
        elif isinstance(change, int):
            esp_delta += change

        # --- update the tracking set ---------------------------------------------------
        if insn.mnemonic.startswith("call"):
            # A call clobbers the volatile registers.
            for reg in list(tracked):
                if reg not in CALLEE_SAVED:
                    del tracked[reg]
            for reg in list(loaded_into):
                if reg not in CALLEE_SAVED:
                    del loaded_into[reg]
            target = re.match(r"^(0x[0-9a-f]+)", insn.operands)
            resolved = resolvers.class_of(int(target.group(1), 16)) if target else None
            if resolved:
                result.resolver_classes.append(resolved)
                pending_class = resolved
                # eax holds the fresh object pointer; give it a few instructions to be
                # parked somewhere durable before assuming it was discarded.
                pending_countdown = 6
                tracked["%eax"] = (resolved, ())
            else:
                pending_class = None
                pending_countdown = 0
            continue

        written = _written_register(insn)
        if written is None:
            continue
        # Any write other than the load we just recorded invalidates the vcall attribution.
        if written in loaded_into and loaded_into[written].va != insn.va:
            del loaded_into[written]

        # `mov D(%esp), %reg` — the receiver coming back out of its stack slot.
        propagated = reloaded
        if propagated is None and insn.mnemonic == "movl":
            for disp, base_reg, indexed, is_last in _memory_operands(insn):
                if base_reg in tracked and not is_last and not indexed:
                    owner_class, chain = tracked[base_reg]
                    if len(chain) < 1:  # keep the chain shallow; deeper is not attributable
                        propagated = (owner_class, chain + (disp,))
                    break

        # `mov %tracked, %reg` — a straight copy carries the tracking over.
        if propagated is None and insn.mnemonic in ("movl", "movl "):
            source = insn.operands.split(",")[0].strip()
            if source in tracked:
                propagated = tracked[source]

        # `mov %eax, %esi` right after a resolver call is the canonical receiver park.
        if propagated is None and pending_class and pending_countdown > 0:
            source = insn.operands.split(",")[0].strip()
            if source == "%eax" and insn.mnemonic == "movl":
                propagated = (pending_class, ())

        if propagated is not None:
            tracked[written] = propagated
        else:
            tracked.pop(written, None)

        if pending_countdown:
            pending_countdown -= 1

    return result


# --------------------------------------------------------------------------------------
# Turning raw accesses into a reportable table
# --------------------------------------------------------------------------------------

def split_camel(name: str) -> str:
    return re.sub(r"(?<!^)(?=[A-Z])", " ", name).lower()


def field_meaning(method_name: str, scope: str) -> str:
    """A human-readable guess at what the field is, from the method's own name.

    INFERRED, not read.  The offset and the class are facts; this column is the method name
    with its accessor prefix removed, which is nearly always right for a simple accessor and
    is the whole reason the getter sweep is worth running.
    """
    # A free function takes the object as an argument, so its name describes an operation on
    # the object rather than the field being touched.  Saying so is more honest than dressing
    # `CreateRotator` up as a field name.
    if scope in ("", "<global>"):
        return f"(touched by global {method_name})"
    stem = method_name
    for prefix in GETTER_PREFIXES + SETTER_PREFIXES:
        if stem.startswith(prefix) and len(stem) > len(prefix):
            stem = stem[len(prefix):]
            break
    meaning = split_camel(stem) if stem else split_camel(method_name)
    return meaning.strip() or method_name


def semantic_type(ctype: str, method_name: str, is_pointer: bool) -> str:
    """Refine the machine-read width into something a C++ header would say."""
    if ctype == "u32":
        if is_pointer:
            return "ptr"
        if method_name.startswith(("Is", "Has", "Can", "Are", "Was", "Should")):
            return "bool32"
        return "int32"
    if ctype in ("u8", "i8") and method_name.startswith(("Is", "Has", "Can", "Are", "Was")):
        return "bool"
    return ctype


def classify(method_name: str, distinct_offsets: int, base_known: bool,
             truncated: bool) -> str:
    """Confidence in "this offset is the field this method is named after".

    high   - the base class was read from a type descriptor, the name is a plain accessor,
             and the function touches exactly one field of that object.  Nothing else it
             could be.
    medium - base class read, but the function touches a handful of fields, so the offset is
             certainly a field of that class but not certainly *this* method's field.
    low    - base class unknown, the body was too long to bound, or the function sprays over
             many offsets.
    """
    if not base_known or truncated:
        return "low"
    accessor = method_name.startswith(GETTER_PREFIXES + SETTER_PREFIXES)
    if distinct_offsets == 1 and accessor:
        return "high"
    if distinct_offsets <= 2 and accessor:
        return "high" if distinct_offsets == 1 else "medium"
    if distinct_offsets <= 5:
        return "medium"
    return "low"


HEADER = [
    "base_class", "offset", "field_meaning", "type",
    "source_method_va", "source_method_name", "confidence", "ambiguous_base_flag",
    "access", "insn", "lua_scope", "base_is_subobject", "chain",
    "n_offsets_in_fn", "subsystem", "signature", "doc", "derivation",
]


def emit(base_class: str, offset_text: str, meaning: str, ctype: str, method_va: int,
         label: str, confidence: str, ambiguous: str, access_kind: str, insn_text: str,
         scope: str, subobject: str, chain_text: str, distinct: str, row: dict,
         derivation: str) -> list[str]:
    return [base_class, offset_text, meaning, ctype, f"{method_va:08x}", label, confidence,
            ambiguous, access_kind, insn_text, scope, subobject, chain_text, distinct,
            row.get("subsystem", ""), row.get("signature", ""), row.get("doc", ""),
            derivation]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--methods", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--chains-out", default=None,
                        help="optional TSV for two-level accesses (pointer field -> field)")
    parser.add_argument("--vtables-out", default=None,
                        help="optional TSV mapping vtable slot offsets to the Lua method "
                             "that dispatches through them")
    parser.add_argument("--layout-out", default=None,
                        help="optional TSV with one row per (class, offset) — the readable "
                             "object layout, with a witness count per field")
    parser.add_argument("--natives-out", default=None,
                        help="optional TSV of native methods the bindings forward to")
    parser.add_argument("--follow", action="store_true",
                        help="also mine the native method each binding forwards to "
                             "(more offsets, one evidence step further away)")
    parser.add_argument("--limit", type=int, default=0, help="debug: only N methods")
    args = parser.parse_args()

    pe = PE32(args.exe)
    disasm = Disassembler(args.exe)
    resolvers = ResolverMap(pe, disasm)

    with open(args.methods, encoding="latin-1") as handle:
        header = handle.readline().rstrip("\n").split("\t")
        rows = [dict(zip(header, line.rstrip("\n").split("\t")))
                for line in handle if line.strip()]

    direct_rows: list[list[str]] = []
    chain_rows: list[list[str]] = []
    vtable_rows: list[list[str]] = []
    native_targets: list[tuple[str, str, int, str]] = []
    stats: dict[str, int] = defaultdict(int)

    considered = 0
    for row in rows:
        method_va_text = row.get("method_va", "-")
        if not method_va_text or method_va_text == "-":
            stats["skipped_unresolved"] += 1
            continue
        method_va = int(method_va_text, 16)
        if args.limit and considered >= args.limit:
            break
        considered += 1

        name = row.get("name", "")
        scope = row.get("scope", "")
        followed_offsets: set[tuple[str, int]] = set()
        analysis = analyse_method(method_va, disasm, resolvers)

        # ---- follow the real native method, when the binding is only a forwarder -------
        # Many two-argument bindings do no field work themselves: they resolve the receiver
        # and immediately `call Moho::Unit::SetConsumptionActive(this=ecx, value)`.  Mining
        # that callee recovers the field the binding is named after.  Everything found this
        # way is a step further from the evidence, so it is capped at medium confidence and
        # tagged `via-thiscall` in the `derivation` column.
        if args.follow:
            for callee_va, owner in analysis.thiscall_targets:
                if resolvers.class_of(callee_va) is not None:
                    continue        # that is another receiver resolver, not a method
                followed = analyse_method(callee_va, disasm, resolvers,
                                          seed=("%ecx", owner))
                native_targets.append((owner, name, callee_va, scope))
                for access in followed.accesses:
                    if access.chain or access.indexed or access.base_class != owner:
                        continue
                    analysis.accesses.append(Access(
                        base_class=access.base_class, offset=access.offset, chain=(),
                        kind=access.kind, ctype=access.ctype, mnemonic=access.mnemonic,
                        va=callee_va, indexed=False, immediate=access.immediate,
                        vcall=access.vcall))
                    followed_offsets.add((access.base_class, access.offset))

        if not analysis.accesses:
            stats["no_accesses"] += 1
            continue

        # Pointer-ness: an offset is a pointer if some *other* access chains through it.
        chained_through = {access.chain[0] for access in analysis.accesses if access.chain}
        # A slot reached through offset 0 and then called indirectly is a vtable slot, and
        # offset 0 is therefore the vptr rather than a data member.
        vptr_offsets = {access.chain[0] for access in analysis.accesses
                        if access.chain and access.vcall}

        direct = [a for a in analysis.accesses if not a.chain and not a.indexed]
        per_class_offsets: dict[str, set[int]] = defaultdict(set)
        for access in direct:
            per_class_offsets[access.base_class].add(access.offset)

        seen: set[tuple[str, int, str]] = set()
        for access in direct:
            key = (access.base_class, access.offset, access.kind)
            if key in seen:
                continue
            seen.add(key)
            distinct = len(per_class_offsets[access.base_class])
            confidence = classify(name, distinct, True, analysis.truncated)
            base_is_subobject = ""
            if scope and scope != "<global>":
                short = access.base_class.split("::")[-1]
                if short != scope:
                    base_is_subobject = f"lua {scope} -> native {access.base_class}"
            # A vptr at offset 0 is the object's own.  A vptr at a non-zero offset is the
            # start of an embedded base subobject — for `Moho::Unit` that is `+0x08`, the
            # `Entity` subobject (`C-053`).  Calling both "vtable pointer" would erase
            # exactly the base-pointer distinction this table exists to preserve.
            is_vptr = access.offset in vptr_offsets
            via_callee = (access.base_class, access.offset) in followed_offsets
            if via_callee:
                # One evidence step further out.  For a free function it is two: the object
                # is an argument, so which of its several objects ended up in ECX is a guess.
                confidence = "low" if scope in ("", "<global>") else "medium"
            direct_rows.append(emit(
                access.base_class,
                f"0x{access.offset:X}",
                ("vtable pointer" if access.offset == 0 else
                 "embedded base subobject (its vptr)") if is_vptr
                else field_meaning(name, scope),
                ("vptr" if access.offset == 0 else "subobject") if is_vptr
                else semantic_type(access.ctype, name, access.offset in chained_through),
                method_va,
                f"{scope}:{name}" if scope and scope != "<global>" else name,
                confidence,
                "false",
                access.kind,
                access.mnemonic + (f" $0x{access.immediate:x}"
                                   if access.immediate is not None else ""),
                scope, base_is_subobject, "", str(distinct), row,
                "via-thiscall" if via_callee else "lua-binding",
            ))
            stats[f"conf_{confidence}"] += 1

        for access in analysis.accesses:
            # A scaled/indexed access is an *array element*, not a field: `mov 0x4(%eax,%ecx)`
            # after `imul $0x34,%eax` is "record[army].something".  Those must not land in the
            # field table, but dropping them silently is how a sweep loses the most
            # interesting facts, so they are recorded here with the flag set.
            if access.indexed:
                chain_rows.append(emit(
                    access.base_class,
                    ("->".join(f"0x{step:X}" for step in access.chain) + "->" if access.chain
                     else "") + f"[idx]+0x{access.offset:X}",
                    field_meaning(name, scope), access.ctype, method_va,
                    f"{scope}:{name}" if scope and scope != "<global>" else name,
                    "medium", "true", access.kind, access.mnemonic, scope, "",
                    "array element", "", row, "lua-binding"))
                continue
            if not access.chain:
                continue
            target = emit(
                access.base_class,
                "->".join(f"0x{step:X}" for step in access.chain) + f"->0x{access.offset:X}",
                field_meaning(name, scope), access.ctype, method_va,
                f"{scope}:{name}" if scope and scope != "<global>" else name,
                "medium", "true", access.kind, access.mnemonic, scope, "", "chained", "",
                row, "lua-binding")
            # `mov (%this),%r ; mov 0xNN(%r),%f ; call *%f` is a virtual dispatch, not a
            # field read.  Those rows are the class's vtable layout, which is a different
            # (and separately useful) artifact from its data layout.
            if access.vcall and access.chain == (0,):
                vtable_rows.append(emit(
                    access.base_class, f"0x{access.offset:X}",
                    f"virtual slot called by {name}", "vtable slot", method_va,
                    f"{scope}:{name}" if scope and scope != "<global>" else name,
                    "high", "false", "vcall", access.mnemonic, scope, "", "vtable", "",
                    row, "lua-binding"))
            else:
                chain_rows.append(target)

    # De-duplicate: the same (class, offset) is often proven by several methods.  Keep every
    # witness row — corroboration is the point — but sort so the table reads as a layout.
    def sort_key(entry: list[str]) -> tuple:
        return (entry[0], int(entry[1], 16), entry[5])

    direct_rows.sort(key=sort_key)
    chain_rows.sort(key=lambda entry: (entry[0], entry[1]))

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as handle:
        handle.write("\t".join(HEADER) + "\n")
        for entry in direct_rows:
            handle.write("\t".join(entry) + "\n")

    if args.chains_out:
        with open(args.chains_out, "w", encoding="utf-8") as handle:
            handle.write("\t".join(HEADER) + "\n")
            for entry in chain_rows:
                handle.write("\t".join(entry) + "\n")

    if args.vtables_out:
        vtable_rows.sort(key=lambda entry: (entry[0], int(entry[1], 16)))
        with open(args.vtables_out, "w", encoding="utf-8") as handle:
            handle.write("\t".join(HEADER) + "\n")
            for entry in vtable_rows:
                handle.write("\t".join(entry) + "\n")

    if args.layout_out:
        # One row per (class, offset) instead of one per witness: the readable form, and the
        # one to diff against the plan's table.  `witnesses` is the corroboration count —
        # an offset proven by four independent methods is much harder to doubt than one.
        rank = {"high": 0, "medium": 1, "low": 2}
        grouped: dict[tuple[str, int], list[list[str]]] = defaultdict(list)
        for entry in direct_rows:
            grouped[(entry[0], int(entry[1], 16))].append(entry)
        with open(args.layout_out, "w", encoding="utf-8") as handle:
            handle.write("base_class\toffset\tfield_meaning\ttype\tconfidence\taccess\t"
                         "witnesses\twitness_methods\tinstructions\n")
            for (cls, offset) in sorted(grouped):
                group = sorted(grouped[(cls, offset)], key=lambda e: rank[e[6]])
                # Prefer a scoped accessor's wording over a free function's when naming.
                named = next((e for e in group if e[10] not in ("", "<global>")), group[0])
                methods = sorted({e[5] for e in group})
                handle.write("\t".join([
                    cls, f"0x{offset:X}", named[2], named[3], group[0][6],
                    ",".join(sorted({e[8] for e in group})),
                    str(len(methods)), "; ".join(methods),
                    "; ".join(sorted({e[9] for e in group})),
                ]) + "\n")

    if args.natives_out:
        with open(args.natives_out, "w", encoding="utf-8") as handle:
            handle.write("owner_class\tmethod_name\tnative_va\tlua_scope\n")
            for owner, method_name, callee_va, scope in sorted(set(native_targets)):
                handle.write(f"{owner}\t{method_name}\t{callee_va:08x}\t{scope}\n")

    print(f"native thiscall targets: {len(set(native_targets))}", file=sys.stderr)
    print(f"methods analysed      : {considered}", file=sys.stderr)
    print(f"direct offset rows    : {len(direct_rows)}", file=sys.stderr)
    print(f"chained offset rows   : {len(chain_rows)}", file=sys.stderr)
    print(f"vtable slot rows      : {len(vtable_rows)}", file=sys.stderr)
    for key in sorted(stats):
        print(f"  {key:22s}: {stats[key]}", file=sys.stderr)
    classes = sorted({entry[0] for entry in direct_rows})
    print(f"classes seen ({len(classes)}): {', '.join(classes)}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
