#!/usr/bin/env python3
"""Recover the engine's own enum-to-name tables from its enum-registration functions.

The mechanism
-------------
Retail Forged Alliance registers each of its named enumerations at startup through a pair of
helpers.  Read from `0x005626a0`, the shape of one entry is exactly 22 bytes:

    6a 01                push  1                    ; the enum's integer value
    68 8c 7d e6 00       push  offset "UNITSTATE_Immobile"
    8b ce                mov   ecx, esi              ; the registry object
    e8 ...               call  0x00947000            ; intern the name -> eax
    50                   push  eax
    8b ce                mov   ecx, esi
    e8 ...               call  0x0094def0            ; bind (name, value) into the registry

and the enclosing function first does `mov [esi+0x64], offset "UNITSTATE_"`, recording the
common prefix that every name in that enum carries.

That means **the integer value is a literal immediate sitting next to its own name**.  It is
not inferred from array position, so an off-by-one is not possible: `push 1` next to
`"UNITSTATE_Immobile"` says Immobile is 1 and nothing else.

52 functions in the image call both helpers, so this one scan recovers every enumeration the
engine chose to name.

Output
------
    build/re-fa/exports/enum_registrations.tsv
        function_va, prefix, value, name (prefix stripped), raw_name, string_va

Caveats
-------
* This is a byte-pattern decoder, not a disassembler.  It looks for `push imm` immediately
  followed by `push <string VA>` inside the registrant function's range.  A `push imm/push
  imm` pair that is *not* a registration would be picked up too; the guard is that the second
  operand must resolve to a printable C string.  Spot-check any table that looks odd.
* Values above 0x7F are encoded `68 <imm32>` rather than `6a <imm8>`; both are handled.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_reader import PE32  # noqa: E402

DEFAULT_EXE = os.path.expanduser(
    "~/projects/llm/input/recoil-metal/retail-fa/bin/SupremeCommander.exe")

# The two helpers every registration function calls.  Identified by reading 0x005626a0.
INTERN_NAME = 0x00947000     # __thiscall (const char *) -> interned name handle
BIND_VALUE = 0x0094def0      # __thiscall (handle)       -> binds it at the pushed value


def load_functions(path: str) -> dict[int, tuple[int, list[int]]]:
    """callgraph.tsv -> {start: (size, [callees])}."""
    out: dict[int, tuple[int, list[int]]] = {}
    with open(path, encoding="latin-1") as handle:
        for line in handle:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 2:
                continue
            # Callees are hex VAs, except imports, which Ghidra emits as "EXTERNAL:xxxxxxxx".
            callees: list[int] = []
            if len(parts) > 2 and parts[2]:
                for token in parts[2].split(","):
                    if token.startswith("EXTERNAL:"):
                        continue
                    try:
                        callees.append(int(token, 16))
                    except ValueError:
                        pass
            out[int(parts[0], 16)] = (int(parts[1]), callees)
    return out


def decode(pe: PE32, start: int, size: int) -> tuple[str | None, list[tuple[int, str, int]]]:
    """Return (prefix, [(value, raw_name, string_va), ...]) for one registration function."""
    body = pe.read(start, size)
    if not body:
        return None, []

    prefix: str | None = None
    entries: list[tuple[int, str, int]] = []

    index = 0
    while index < len(body) - 6:
        byte = body[index]

        # mov dword [reg+0x64], imm32   ->  the enum's common name prefix.
        # C7 /0 with an 8-bit displacement of 0x64: `c7 4? 64 <imm32>`.
        if byte == 0xC7 and 0x40 <= body[index + 1] <= 0x47 and body[index + 2] == 0x64:
            candidate = struct.unpack_from("<I", body, index + 3)[0]
            text = pe.identifier(candidate, 64)
            if text:
                prefix = text
            index += 7
            continue

        value: int | None = None
        after = 0
        if byte == 0x6A:                                   # push imm8
            value, after = body[index + 1], index + 2
        elif byte == 0x68:                                 # push imm32
            value, after = struct.unpack_from("<I", body, index + 1)[0], index + 5

        # The pair must be `push <value>` immediately followed by `push <string>`.
        if value is not None and after < len(body) - 4 and body[after] == 0x68:
            target = struct.unpack_from("<I", body, after + 1)[0]
            text = pe.identifier(target, 96)
            # Bitmask enums such as RULEUCC_ run up to 1 << 24, so the value cannot be
            # capped low.  The discriminator instead is that a *value* must not itself be a
            # pointer to a string -- that shape is two consecutive string pushes, not a
            # (value, name) registration.
            if text and len(text) >= 2 and not pe.identifier(value, 8):
                entries.append((value, text, target))
                index = after + 5
                continue

        index += 1

    return prefix, entries


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", default=DEFAULT_EXE)
    parser.add_argument("--callgraph", default="build/re-fa/exports/callgraph.tsv")
    parser.add_argument("--out", default="build/re-fa/exports/enum_registrations.tsv")
    args = parser.parse_args()

    pe = PE32(args.exe)
    functions = load_functions(args.callgraph)

    registrants = [va for va, (_, callees) in functions.items()
                   if INTERN_NAME in callees and BIND_VALUE in callees]
    registrants.sort()
    print(f"registration functions: {len(registrants)}", file=sys.stderr)

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    total = 0
    with open(args.out, "w", encoding="latin-1") as out:
        out.write("function_va\tprefix\tvalue\tname\traw_name\tstring_va\n")
        for va in registrants:
            size, _ = functions[va]
            prefix, entries = decode(pe, va, size)
            for value, raw, string_va in entries:
                short = raw[len(prefix):] if prefix and raw.startswith(prefix) else raw
                out.write(f"{va:08x}\t{prefix or '-'}\t{value}\t{short}\t{raw}\t{string_va:08x}\n")
                total += 1
    print(f"entries: {total}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
