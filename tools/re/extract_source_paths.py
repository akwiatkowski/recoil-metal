#!/usr/bin/env python3
"""Mine `__FILE__`-style source-path strings out of the retail FA executable and
attribute them to the functions that reference them.

WHY THIS EXISTS
---------------
The campaign's Tier 1 method is "the binary describes itself somewhere; scan for that
description across all of it at once". RTTI gave us class names, the export table gave us
method names, the registration scan gave us the Lua API. This script goes after a fourth
self-description: the source file paths that MSVC's `assert` and Gas Powered Games' own
SPEW/verify macros bake in via `__FILE__`.

A source path is a *fact* in a way a BSim name is not. If a function pushes the literal
`.\\sim\\Entity.cpp`, that function was compiled from `sim/Entity.cpp`. There is no
similarity threshold and no false-positive rate to argue about — the only question is
whether the reference really is a `__FILE__` and not, say, a filename used as data.

HOW IT WORKS
------------
1.  Walk `.rdata` and `.data` collecting NUL-terminated printable ASCII strings, recording
    each one's virtual address. We do this in Python rather than with `strings(1)` because
    we need the VA, and because `strings` applies a minimum length that would drop short
    paths like `.\\Fx.cpp`.

2.  Classify a string as a source path with a deliberately loose heuristic (any C/C++
    source or header extension, or a Windows/Perforce-looking path). Loose is correct here:
    the cost of a false candidate is one wasted scan, and the cost of a missed one is a
    silently incomplete oracle.

3.  Find the code that references each candidate. The image has its relocations stripped
    and is not ASLR-aware, so it is hard-bound to image base 0x00400000 — absolute VAs
    therefore appear *literally* in the instruction stream. We search `.text` for the
    4-byte little-endian encoding of each candidate VA. This finds `push imm32` (0x68),
    `mov reg, imm32` (0xB8+r), `mov [mem], imm32` (0xC7) and anything else that embeds the
    address, without having to disassemble 8.7 MB.

4.  Attribute each reference site to its containing function using the function starts and
    sizes already dumped to `callgraph.tsv`, and label the function from the annotated Moho
    method table where one is known.

5.  Recover the line number. The MSVC/GPG convention at an assertion call site is a run of
    pushes ending in the file and the line, e.g.

        6A 2B                push 0x2B          ; __LINE__
        68 xx xx xx xx       push offset "..."  ; __FILE__

    so we look a few bytes either side of the file push for a `push imm8` / `push imm32`
    that decodes to a plausible line number (1..99999).

WHAT THIS SCRIPT DOES NOT DO
----------------------------
It does not disassemble. A 4-byte value that happens to equal a string VA can occur inside
an unrelated instruction's displacement or immediate. The `ref_kind` column records the
opcode byte immediately before the match so a reader can judge; `push_imm32` matches are
the trustworthy ones and are counted separately in the summary.

Read-only. Never executes the target.

Usage:
    python3 tools/re/extract_source_paths.py \
        --exe ~/projects/llm/input/recoil-metal/retail-fa/bin/SupremeCommander.exe \
        --callgraph build/re-fa/exports/callgraph.tsv \
        --methods build/re-fa/exports/moho.methods.annotated.tsv \
        --out build/re-fa/exports/source_paths.tsv
"""

import argparse
import bisect
import os
import re
import struct
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_reader import PE32  # noqa: E402


# Sections that can hold read-only or writable initialised data. `.text` is excluded
# because a string living in the code section would be unusual for MSVC and every hit
# there would be a false positive from the reference scan itself.
DATA_SECTIONS = (".rdata", ".data")

# Minimum length for a harvested string. 5 is short enough for `a.cpp` and long enough to
# keep the candidate set from filling with two-byte noise.
MIN_STRING_LENGTH = 5

# A source path, loosely. Two families are expected in this binary:
#   `.\sim\Entity.cpp`                              — relative, MSVC compiled with a
#                                                     relative include path
#   `C:\work\rts\main\code\src\core/ColMain.h`      — absolute, from the Perforce tree
SOURCE_EXTENSION = re.compile(r"\.(cpp|cxx|cc|c|hpp|hxx|hh|h|inl)$", re.IGNORECASE)

# Plausible __LINE__ values. Real GPG source files run to a few thousand lines; the upper
# bound is generous so that a genuine large file is not silently dropped.
LINE_MIN, LINE_MAX = 1, 99999


@dataclass(frozen=True)
class Func:
    va: int
    size: int


def harvest_strings(pe: PE32) -> list[tuple[int, str]]:
    """Every NUL-terminated printable ASCII string in the data sections, as (va, text).

    Printable is defined as 0x20..0x7E plus tab — deliberately excluding CR/LF so that a
    multi-line log format string does not swallow a following path.
    """
    out: list[tuple[int, str]] = []
    printable = set(range(0x20, 0x7F)) | {0x09}
    for section in pe.sections:
        if section.name not in DATA_SECTIONS:
            continue
        blob = pe.data[section.raw_offset:section.raw_offset + section.raw_size]
        start = None
        for index, byte in enumerate(blob):
            if byte in printable:
                if start is None:
                    start = index
                continue
            # Terminator (or any non-printable) closes the run. Only NUL-terminated runs
            # are real C strings; anything else is data that merely looked like text.
            if start is not None:
                if byte == 0 and index - start >= MIN_STRING_LENGTH:
                    text = blob[start:index].decode("latin-1")
                    out.append((section.virtual_address + start, text))
                start = None
    return out


def is_source_path(text: str) -> bool:
    """True when the string looks like something a `__FILE__` would expand to."""
    if not SOURCE_EXTENSION.search(text):
        return False
    # A bare word with a source extension ("string.h" in an error message) is a weak
    # candidate; require some evidence of a path, which is what `__FILE__` always carries
    # unless the compiler was invoked on a bare filename in the source's own directory.
    return ("\\" in text or "/" in text) and len(text) < 260


def load_functions(path: str) -> list[Func]:
    """Function starts and sizes from the Ghidra call-graph dump: va \\t size \\t callees."""
    funcs: list[Func] = []
    with open(path, "r", encoding="latin-1") as handle:
        for line in handle:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 2:
                continue
            try:
                funcs.append(Func(int(parts[0], 16), int(parts[1])))
            except ValueError:
                continue
    funcs.sort(key=lambda f: f.va)
    return funcs


def load_known_names(path: str) -> dict[int, str]:
    """Address -> best known name, from the annotated Moho method table.

    Both the resolved native method and its generated 20-byte Lua wrapper are recorded,
    because a reference site can land in either and both are informative.
    """
    names: dict[int, str] = {}
    if not os.path.exists(path):
        return names
    with open(path, "r", encoding="latin-1") as handle:
        header = handle.readline().rstrip("\n").split("\t")
        col = {name: i for i, name in enumerate(header)}
        for line in handle:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < len(header):
                continue

            def field(key: str) -> str:
                index = col.get(key, -1)
                return parts[index] if 0 <= index < len(parts) else ""

            subsystem = field("subsystem")
            scope = field("scope")
            name = field("name")
            label = f"{subsystem}:{scope}.{name}" if scope and scope != "<global>" \
                else f"{subsystem}:{name}"
            for key in ("method_va", "wrapper_va"):
                raw = field(key)
                if raw and raw not in ("-", "00000000"):
                    try:
                        names.setdefault(int(raw, 16), label)
                    except ValueError:
                        pass
    return names


def containing(funcs: list[Func], starts: list[int], va: int) -> Func | None:
    """The function whose [start, start+size) range covers `va`, or None."""
    index = bisect.bisect_right(starts, va) - 1
    if index < 0:
        return None
    func = funcs[index]
    return func if va < func.va + func.size else None


def find_references(pe: PE32, targets: dict[int, str]) -> list[tuple[int, int, str, int | None]]:
    """Scan `.text` for literal little-endian occurrences of each target VA.

    Returns (ref_site_va, string_va, ref_kind, line_number_or_None). `ref_site_va` points
    at the *instruction byte*, i.e. one before the immediate, when the preceding byte
    identifies a known instruction form; otherwise it points at the immediate itself.
    """
    text = next(s for s in pe.sections if s.name == ".text")
    blob = pe.data[text.raw_offset:text.raw_offset + text.raw_size]
    base = text.virtual_address

    results = []
    for string_va, _ in targets.items():
        needle = struct.pack("<I", string_va)
        pos = blob.find(needle)
        while pos != -1:
            prev = blob[pos - 1] if pos > 0 else None
            if prev == 0x68:
                kind, site = "push_imm32", pos - 1
            elif prev is not None and 0xB8 <= prev <= 0xBF:
                kind, site = "mov_reg_imm32", pos - 1
            elif prev == 0x05:            # add eax, imm32 / part of a longer form
                kind, site = "arith_imm32", pos - 1
            else:
                kind, site = "raw_imm32", pos
            results.append((base + site, string_va, kind,
                            recover_line(blob, pos, prev),
                            recover_message(pe, blob, pos, prev)))
            pos = blob.find(needle, pos + 1)
    return results


def recover_line(blob: bytes, imm_pos: int, prev: int | None) -> int | None:
    """Look for a `__LINE__` push adjacent to a `__FILE__` push.

    MSVC's `_assert(msg, file, line)` is cdecl, so arguments are pushed right-to-left:
    line first, then file, then message. The line push therefore sits *before* the file
    push in the instruction stream. GPG's own macros vary, so both sides are checked and
    the nearer plausible value wins.
    """
    if prev != 0x68:
        return None
    file_push = imm_pos - 1

    def decode_push_before(end: int) -> int | None:
        # `push imm8` is 6A xx (two bytes); `push imm32` is 68 xx xx xx xx (five).
        if end >= 2 and blob[end - 2] == 0x6A:
            return blob[end - 1]
        if end >= 5 and blob[end - 5] == 0x68:
            return struct.unpack_from("<I", blob, end - 4)[0]
        return None

    def decode_push_at(start: int) -> int | None:
        if start + 2 <= len(blob) and blob[start] == 0x6A:
            return blob[start + 1]
        if start + 5 <= len(blob) and blob[start] == 0x68:
            return struct.unpack_from("<I", blob, start + 1)[0]
        return None

    for value in (decode_push_before(file_push), decode_push_at(imm_pos + 4)):
        if value is not None and LINE_MIN <= value <= LINE_MAX:
            return value
    return None


def recover_message(pe: PE32, blob: bytes, imm_pos: int, prev: int | None) -> str:
    """The assertion text pushed after the line, for the canonical GPG panic shape.

    Verified against 0x0067f584 (`.\\sim\\Entity.cpp`, line 354):

        68 c0 6d e7 00   push offset ".\\sim\\Entity.cpp"   ; __FILE__
        68 62 01 00 00   push 0x162                        ; __LINE__
        68 8c fe e4 00   push offset "Reached the ..."     ; message
        e8 ..            call 0x009728a0                   ; the panic reporter
        83 c4 0c         add  esp, 0xc                     ; cdecl cleanup, 3 args
        cc               int3                              ; noreturn

    So the callee signature is `panic(const char* msg, int line, const char* file)`.
    """
    if prev != 0x68:
        return ""
    # Skip the file immediate (4 bytes), then the line push (2 or 5 bytes).
    after_file = imm_pos + 4
    if after_file < len(blob) and blob[after_file] == 0x6A:
        after_line = after_file + 2
    elif after_file < len(blob) and blob[after_file] == 0x68:
        after_line = after_file + 5
    else:
        return ""
    if after_line + 5 > len(blob) or blob[after_line] != 0x68:
        return ""
    message_va = struct.unpack_from("<I", blob, after_line + 1)[0]
    return (pe.identifier(message_va, 300) or "").replace("\t", " ")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--callgraph", required=True)
    parser.add_argument("--methods", default="")
    parser.add_argument("--out", required=True)
    parser.add_argument("--strings-out", default="",
                        help="optional: dump every harvested data string as va\\ttext")
    args = parser.parse_args()

    pe = PE32(args.exe)
    strings = harvest_strings(pe)
    candidates = {va: text for va, text in strings if is_source_path(text)}
    print(f"[strings] {len(strings):,} data strings, "
          f"{len(candidates):,} look like source paths", file=sys.stderr)

    if args.strings_out:
        with open(args.strings_out, "w", encoding="latin-1") as handle:
            for va, text in strings:
                handle.write(f"{va:08x}\t{text}\n")

    funcs = load_functions(args.callgraph)
    starts = [f.va for f in funcs]
    names = load_known_names(args.methods) if args.methods else {}
    print(f"[inputs] {len(funcs):,} functions, {len(names):,} known names", file=sys.stderr)

    refs = find_references(pe, candidates)
    print(f"[refs] {len(refs):,} literal references in .text", file=sys.stderr)

    rows = []
    for site_va, string_va, kind, line, message in sorted(refs):
        func = containing(funcs, starts, site_va)
        rows.append((
            f"{string_va:08x}",
            candidates[string_va],
            f"{site_va:08x}",
            f"{func.va:08x}" if func else "",
            names.get(func.va, "") if func else "",
            str(line) if line is not None else "",
            kind,
            message,
        ))

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="latin-1") as handle:
        handle.write("string_va\tsource_path\tref_site_va\tcontaining_func_va\t"
                     "containing_func_name\tline_number\tref_kind\tassert_message\n")
        for row in rows:
            handle.write("\t".join(row) + "\n")

    # Rollup: per source file, how many distinct functions it owns and the address /
    # line span it covers. The span is the useful part — because the linker emits
    # object files contiguously, it brackets the translation unit in `.text`.
    per_file: dict[str, set[str]] = defaultdict(set)
    per_file_lines: dict[str, list[int]] = defaultdict(list)
    for row in rows:
        if row[3]:
            per_file[row[1]].add(row[3])
        if row[5]:
            per_file_lines[row[1]].append(int(row[5]))

    rollup_path = args.out.replace(".tsv", ".rollup.tsv")
    with open(rollup_path, "w", encoding="latin-1") as handle:
        handle.write("source_path\tref_sites\tfunctions\tfirst_func_va\tlast_func_va\t"
                     "min_line\tmax_line\n")
        counts = Counter(row[1] for row in rows)
        for path in sorted(candidates.values()):
            owners = sorted(per_file.get(path, ()))
            lines = per_file_lines.get(path, [])
            handle.write("\t".join([
                path, str(counts.get(path, 0)), str(len(owners)),
                owners[0] if owners else "", owners[-1] if owners else "",
                str(min(lines)) if lines else "", str(max(lines)) if lines else "",
            ]) + "\n")

    print(f"[out] {args.out}: {len(rows):,} reference rows, "
          f"{len(per_file):,} files with at least one attributed function", file=sys.stderr)
    print(f"[out] {rollup_path}", file=sys.stderr)
    kinds = Counter(row[6] for row in rows)
    print(f"[kinds] {dict(kinds)}", file=sys.stderr)
    lines_ok = sum(1 for row in rows if row[5])
    msgs_ok = sum(1 for row in rows if row[7])
    print(f"[lines] {lines_ok:,} of {len(rows):,} rows recovered a line number",
          file=sys.stderr)
    print(f"[msgs]  {msgs_ok:,} of {len(rows):,} rows recovered an assertion message",
          file=sys.stderr)
    for path, owners in sorted(per_file.items(), key=lambda kv: -len(kv[1]))[:40]:
        print(f"  {len(owners):5d}  {path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
