#!/usr/bin/env python3
"""Disassemble a range of the retail executable and annotate the operands.

`objdump` alone prints bare hex immediates.  Almost every question in this campaign turns on
what those immediates *are* -- a string literal, a known Lua binding, a data table -- so this
wraps objdump and appends, for every immediate that resolves to something we can name:

  * `"..."`            the C string at that address, when it points into a data section
  * `<name>`           the recovered function name, from moho.methods.annotated.tsv
  * `-> "..."`         for `call`/`jmp` targets that are known functions

It takes no Ghidra project lock, so it is safe to run while other analysts hold the project.

Usage:
    python3 tools/re/disasm_annotated.py 0x006bf9b0 0x006bf9f0
    python3 tools/re/disasm_annotated.py 0x006bf9b0 +0x40
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_reader import PE32  # noqa: E402

DEFAULT_EXE = os.path.expanduser(
    "~/projects/llm/input/recoil-metal/retail-fa/bin/SupremeCommander.exe")
# Any 6-8 digit hex literal in the operand column is a candidate address.
HEX_RE = re.compile(r"0x([0-9a-f]{6,8})\b")


def load_names(path: str) -> dict[int, str]:
    """address -> best available name, from the campaign's recovered method table."""
    names: dict[int, str] = {}
    if not os.path.exists(path):
        return names
    with open(path, encoding="latin-1") as handle:
        header = handle.readline().rstrip("\n").split("\t")
        try:
            scope_i, name_i = header.index("scope"), header.index("name")
            wrapper_i = header.index("wrapper_va")
            method_i = header.index("method_va")
        except ValueError:
            return names
        for line in handle:
            parts = line.rstrip("\n").split("\t")
            if len(parts) <= max(scope_i, name_i, wrapper_i, method_i):
                continue
            label = f"{parts[scope_i]}::{parts[name_i]}"
            for index, prefix in ((wrapper_i, "luathunk_"), (method_i, "")):
                raw = parts[index]
                if raw and raw != "-" and raw != "00000000":
                    try:
                        names.setdefault(int(raw, 16), prefix + label)
                    except ValueError:
                        pass
    return names


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("start")
    parser.add_argument("stop", help="absolute VA, or +N for a length")
    parser.add_argument("--exe", default=DEFAULT_EXE)
    parser.add_argument("--methods",
                        default="build/re-fa/exports/moho.methods.annotated.tsv")
    args = parser.parse_args()

    start = int(args.start, 16)
    stop = start + int(args.stop[1:], 16) if args.stop.startswith("+") else int(args.stop, 16)

    pe = PE32(args.exe)
    names = load_names(args.methods)

    raw = subprocess.run(
        ["objdump", "-d", f"--start-address={start:#x}", f"--stop-address={stop:#x}", args.exe],
        capture_output=True, text=True, check=False).stdout

    for line in raw.splitlines():
        if ":\t" not in line and not re.match(r"^\s*[0-9a-f]{6,8}:", line):
            continue
        notes: list[str] = []
        seen: set[int] = set()
        # Skip the leading "  6bf9b8:" address column; only annotate operands.
        body = line.split(":", 1)[1] if ":" in line else line
        for match in HEX_RE.finditer(body):
            va = int(match.group(1), 16)
            if va in seen:
                continue
            seen.add(va)
            if va in names:
                notes.append(f"{va:#x}=<{names[va]}>")
                continue
            section = pe.section_of(va)
            if section is None:
                continue
            if section.name == ".text":
                continue  # a branch target with no known name adds nothing
            text = pe.identifier(va, 96)
            if text and len(text) >= 2:
                notes.append(f'{va:#x}="{text}"')
            else:
                # A pointer-to-pointer, e.g. a slot in an enum name table.
                inner = pe.u32(va)
                if inner is not None:
                    deref = pe.identifier(inner, 96)
                    if deref and len(deref) >= 2:
                        notes.append(f'{va:#x}=&"{deref}"')
        print(line + ("    ; " + "  ".join(notes) if notes else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
