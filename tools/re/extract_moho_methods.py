#!/usr/bin/env python3
"""Recover the retail Moho Lua method table: class, signature, native address.

WHY THIS APPROACH. `WP-02` first tried to read the method tables out of read-only data,
on the assumption that the engine registers methods the way plain Lua does, with
`luaL_reg`-style `{name, function}` arrays. It does not (finding `F-008`): scanning for
that shape finds only LuaPlus's own standard libraries. The names are not in a table at
all.

They are in the CODE. Each Lua method has a tiny compiler-generated static-initialiser
function that fills a global descriptor field by field, and every one of those stores is a
`mov dword ptr [absolute], immediate` — opcode `C7 05`, ten bytes, with both operands
literal. One such block, at 0x006ca560, reads:

    mov dword ptr [0x1117684], 0x00e6ec0c   ; -> "Unit"             the class
    mov dword ptr [0x1117688], 0x00e7a814   ; -> "GetUnitId(self)"  the signature
    mov dword ptr [0x1117690], 0x006ca530   ;                       the native wrapper
    mov dword ptr [0x1117694], 0x00fee88c   ;                       the class method list

So the whole table can be recovered by scanning `.text` for runs of `C7 05`, grouping them
into blocks, and classifying each immediate by what it points at. No decompilation and no
disassembler are needed, which is what makes this cheap enough to re-run whenever the
question changes.

The signatures are the retail engine's own, including parameter names, and are more
informative than a bare method list: `GetUnitId(self)` states the receiver convention
outright.

Read-only, and offline apart from the preserved executable.
"""

import argparse
import os
import struct
import sys
from collections import defaultdict

from pe_reader import PE32

# `mov dword ptr [disp32], imm32`. Ten bytes: opcode, modrm, disp32, imm32.
STORE_OPCODE = b"\xc7\x05"
STORE_LENGTH = 10

# Two stores belong to the same initialiser when they are no further apart than this. The
# blocks observed interleave a five-byte `mov [abs], eax`, so the gap is small; 0x40 is
# comfortably above it and well below the padding that separates initialisers.
MAX_GAP = 0x40


def find_stores(pe: PE32):
    """Every `mov [disp32], imm32` in .text, as (site_va, dest_va, immediate)."""
    text = next(s for s in pe.sections if s.name == ".text")
    blob = pe.data[text.raw_offset:text.raw_offset + text.raw_size]
    out = []
    start = 0
    while True:
        index = blob.find(STORE_OPCODE, start)
        if index < 0:
            return out
        start = index + 1
        if index + STORE_LENGTH > len(blob):
            continue
        dest, imm = struct.unpack_from("<II", blob, index + 2)
        # A real store targets mapped memory. This is the filter that keeps the scan honest:
        # `c7 05` also occurs inside other instructions and inside constant pools, and those
        # essentially never produce a plausible destination.
        if pe.section_of(dest) is None:
            continue
        out.append((text.virtual_address + index, dest, imm))


def group(stores):
    """Runs of stores close enough together to be one initialiser."""
    blocks, current = [], []
    for store in stores:
        if current and store[0] - current[-1][0] > MAX_GAP:
            blocks.append(current)
            current = []
        current.append(store)
    if current:
        blocks.append(current)
    return blocks


# Field offsets within the descriptor a static initialiser fills, relative to its base.
# Recovered by reading three blocks by hand and confirmed against all 700-odd:
#
#   +0   const char *name        "GetUnitId"
#   +4   const char *scope       "Unit", or "<global>" for a free function
#   +8   const char *signature   "GetUnitId(self) - documentation"
#   +16  int (*wrapper)(...)     the native function Lua ends up calling
#   +20  void *method_list       the class's table; 0 for a free function
#
# The base is located from the wrapper store rather than from the lowest address written,
# because the initialiser also writes a type descriptor at -4 and anchoring on the minimum
# would shift every field by one word.
FIELD_NAME = 0
FIELD_SCOPE = 4
FIELD_SIGNATURE = 8
FIELD_WRAPPER = 16
FIELD_LIST = 20


# The registration wrapper for a `(self)` method is a 20-byte thunk, byte-identical in shape
# for every such method. Verified by disassembly at Unit::GetUnitId (0x006ca530) and
# Unit::GetHealth (0x006cb720):
#
#   8b 44 24 04   mov  eax, [esp+4]   ; the Lua `self` userdata, the only argument
#   50            push eax
#   e8 <rel32>    call <resolver>     ; userdata -> native object pointer, shared by all
#   83 c4 04      add  esp, 4
#   8b c8         mov  ecx, eax       ; receiver becomes `this` (__thiscall)
#   e9 <rel32>    jmp  <method>       ; tail call into the real Moho method
#
# Following it is worth the trouble: it turns an address for a generated Lua shim into the
# address of the actual engine method, which is what later subsystem work needs to read.
THUNK_HEAD = b"\x8b\x44\x24\x04\x50\xe8"
THUNK_MID = b"\x83\xc4\x04\x8b\xc8\xe9"
THUNK_LENGTH = 20


def follow_thunk(pe: PE32, wrapper: int):
    """(resolver, method) for a `(self)` wrapper, or (None, None) for any other shape."""
    body = pe.read(wrapper, THUNK_LENGTH)
    if body is None or len(body) < THUNK_LENGTH:
        return None, None
    if body[0:6] != THUNK_HEAD or body[10:16] != THUNK_MID:
        return None, None
    call_rel = struct.unpack_from("<i", body, 6)[0]
    jmp_rel = struct.unpack_from("<i", body, 16)[0]
    # x86 relative displacements are measured from the end of the instruction.
    return wrapper + 10 + call_rel, wrapper + 20 + jmp_rel


def classify(pe: PE32, blocks):
    """Turn initialiser blocks into one row per registered Lua callable."""
    rows = []
    for block in blocks:
        fields = {dest: imm for _, dest, imm in block}

        # The wrapper store is the anchor: an immediate pointing into .text. Usually there is
        # exactly one, but a handful of blocks carry a second code pointer, so each candidate
        # is tried and the one whose implied base actually yields a name and a scope wins.
        # Rejecting multi-anchor blocks outright would silently drop those registrations.
        anchors = [dest for dest, imm in fields.items() if pe.is_code(imm)]
        name = scope = signature = None
        anchor = None
        for candidate in sorted(anchors):
            base = candidate - FIELD_WRAPPER
            maybe_name = pe.identifier(fields.get(base + FIELD_NAME, 0) or 0)
            maybe_scope = pe.identifier(fields.get(base + FIELD_SCOPE, 0) or 0)
            if maybe_name and maybe_scope:
                anchor = candidate
                name, scope = maybe_name, maybe_scope
                signature = pe.identifier(fields.get(base + FIELD_SIGNATURE, 0) or 0)
                break
        if anchor is None:
            continue
        base = anchor - FIELD_WRAPPER
        # A registration always names something callable and says where it lives. Blocks
        # that pass the anchor test but fill neither are some other static initialiser.
        if not signature:
            signature = name

        # The signature string carries the documentation after " - ", where there is any.
        doc = ""
        if " - " in signature:
            signature, doc = signature.split(" - ", 1)

        resolver, method = follow_thunk(pe, fields[anchor])
        rows.append({
            "resolver": resolver,
            "method": method,
            "scope": scope,
            "name": name,
            "signature": signature.strip(),
            "doc": doc.strip(),
            "wrapper": fields[anchor],
            "list": fields.get(base + FIELD_LIST, 0),
            "site": block[0][0],
        })
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--exe",
        default=os.path.expanduser(
            "~/projects/llm/input/recoil-metal/retail-fa/bin/SupremeCommander.exe"))
    parser.add_argument("--tsv", help="write the table here instead of stdout")
    args = parser.parse_args()

    pe = PE32(args.exe)
    rows = classify(pe, group(find_stores(pe)))
    rows.sort(key=lambda r: (r["scope"], r["name"]))

    by_scope = defaultdict(int)
    for row in rows:
        by_scope[row["scope"]] += 1

    out = open(args.tsv, "w") if args.tsv else sys.stdout
    try:
        print("scope\tname\tsignature\tmethod_va\twrapper_va\tmethod_list_va"
              "\tinit_site_va\tdoc", file=out)
        for row in rows:
            method = f"{row['method']:08x}" if row["method"] else "-"
            print(f"{row['scope']}\t{row['name']}\t{row['signature']}"
                  f"\t{method}\t{row['wrapper']:08x}\t{row['list']:08x}"
                  f"\t{row['site']:08x}\t{row['doc']}", file=out)
    finally:
        if args.tsv:
            out.close()

    resolvers = defaultdict(int)
    for row in rows:
        if row["resolver"]:
            resolvers[row["resolver"]] += 1
    followed = sum(1 for r in rows if r["method"])
    print(f"# callables recovered: {len(rows)}", file=sys.stderr)
    print(f"# thunks followed to the real method: {followed}", file=sys.stderr)
    for va, n in sorted(resolvers.items(), key=lambda kv: -kv[1]):
        print(f"#   receiver resolver {va:08x} used by {n}", file=sys.stderr)
    print(f"# distinct scopes: {len(by_scope)}", file=sys.stderr)
    for name, count in sorted(by_scope.items(), key=lambda kv: -kv[1]):
        print(f"#   {name:28s} {count}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
