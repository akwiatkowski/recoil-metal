#!/usr/bin/env python3
"""Mine the retail Forged Alliance executable for log / spew / error strings and the
pointer tables that map engine enumerations onto their own names.

Why this exists
---------------
The campaign's Tier 1 method is "the binary describes itself somewhere; scan for that
description across all of it at once".  Log-format strings are such a description: they are
English sentences written by the engine's own authors, they name a concept, and they sit
inside the function that implements that concept.  Enum name tables are a stronger form of
the same thing -- a contiguous array of pointers to CamelCase identifiers is almost always a
compiler-emitted `enum -> const char *` mapping, which hands over both the names *and* their
integer values in one read.

What it produces (all under build/re-fa/exports/)
-------------------------------------------------
  strings.all.tsv        every extracted string: VA, section, class, length, text
  strings.xrefs.tsv      string VA -> each .text site that materialises it, plus the
                         containing function taken from callgraph.tsv
  strings.tables.tsv     runs of >= MIN_TABLE consecutive pointers-to-string found in the
                         data sections: the enum name table candidates

Method notes / caveats
----------------------
* The image has its relocations stripped and a fixed base of 0x00400000, so absolute VAs
  appear *literally* in .text (`push imm32` = 68 <va>, `mov reg,imm32` = B8+r <va>, and as
  displacements).  We therefore look for the raw little-endian dword anywhere in .text
  rather than trying to decode instructions.  That is deliberately over-inclusive: a
  coincidental dword can alias a string VA.  Callers must treat a single xref with no
  instruction context as a lead, not a fact.
* Attribution to a containing function uses callgraph.tsv's (start, size) pairs.  A site
  inside no known function is reported with function `-`.
* Strings are read as latin-1.  The retail corpus is ISO-8859-1, never UTF-8.

Read-only: this script opens the executable for reading and never writes to the corpus.
"""

from __future__ import annotations

import argparse
import bisect
import os
import re
import sys
from dataclasses import dataclass

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_reader import PE32  # noqa: E402

# --- tunables -------------------------------------------------------------------------

MIN_STRING = 2          # see MIN_XREF_STRING: short strings are kept, but not cross-referenced
MIN_XREF_STRING = 4     # below this a literal dword aliases far too much of .text to be evidence
MIN_TABLE = 4           # a "table" needs at least this many consecutive string pointers
STRING_SECTIONS = (".rdata", ".data", ".text", "PSFD00")

# printf-style conversion.  Deliberately narrow: `%` followed by the usual flag/width/
# precision soup and then a real conversion character.  `%%` and stray percents are excluded
# so that prose like "100% done" is not misfiled as a format string.
FORMAT_RE = re.compile(r"%[-+ #0]*[\d*]*(?:\.[\d*]+)?(?:h|hh|l|ll|I64|L)?[diouxXeEfgGcspn]")

# Diagnostic prose: the vocabulary engine authors use when something is wrong.
DIAGNOSTIC_RE = re.compile(
    r"\b(error|errors|warning|warn|fail|failed|failure|cannot|can't|unable|invalid|"
    r"illegal|unknown|unexpected|assert|assertion|expected|missing|bad|corrupt|"
    r"unsupported|not\s+supported|out\s+of\s+range|overflow|underflow|abort|fatal)\b",
    re.IGNORECASE,
)

# Enum-like bare identifier: CamelCase or ALLCAPS_WITH_UNDERSCORES, no whitespace, no
# punctuation beyond '_'.  These are what a to-string function returns.
IDENTIFIER_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
CAMEL_RE = re.compile(r"^[A-Z][a-z0-9]*(?:[A-Z][a-z0-9]*)+$")       # AttackMove
ALLCAPS_RE = re.compile(r"^[A-Z][A-Z0-9]*(?:_[A-Z0-9]+)+$")          # STATE_FOO_BAR


@dataclass(frozen=True)
class Str:
    va: int
    section: str
    text: str

    @property
    def kind(self) -> str:
        """Coarse classification.  One string gets exactly one label, most specific first."""
        if FORMAT_RE.search(self.text):
            return "format"
        if DIAGNOSTIC_RE.search(self.text):
            return "diagnostic"
        if IDENTIFIER_RE.match(self.text):
            if CAMEL_RE.match(self.text) or ALLCAPS_RE.match(self.text):
                return "enumlike"
            return "identifier"
        return "other"


# --- string extraction ----------------------------------------------------------------

def extract_strings(pe: PE32) -> list[Str]:
    """Every printable NUL-terminated run of >= MIN_STRING bytes in the data sections.

    Uses numpy to find the printable runs because the sections total ~9 MB and a per-byte
    Python loop over that is minutes rather than seconds.
    """
    out: list[Str] = []
    for section in pe.sections:
        if section.name not in STRING_SECTIONS:
            continue
        raw = pe.data[section.raw_offset:section.raw_offset + section.raw_size]
        buf = np.frombuffer(raw, dtype=np.uint8)
        # Printable ASCII plus tab/newline/carriage-return, which log formats use freely.
        printable = ((buf >= 0x20) & (buf <= 0x7E)) | (buf == 0x09) | (buf == 0x0A) | (buf == 0x0D)
        # Run boundaries: where the printable predicate flips.
        flips = np.flatnonzero(np.diff(printable.astype(np.int8)))
        starts = (flips[printable[flips + 1] == 1] + 1).tolist()
        if printable.size and printable[0]:
            starts.insert(0, 0)
        ends = (flips[printable[flips] == 1] + 1).tolist()
        if printable.size and printable[-1]:
            ends.append(int(printable.size))
        # Short strings are worth keeping in the data sections, because enum name tables
        # legitimately contain entries like "Up" and "Top" and dropping them silently
        # splits a table in two.  In .text a 2-byte printable run is pure noise.
        floor = MIN_STRING if section.name in (".rdata", ".data") else MIN_XREF_STRING
        for start, end in zip(starts, ends):
            if end - start < floor:
                continue
            # Require the NUL terminator: a run that just abuts binary data is not a C string.
            if end >= len(raw) or raw[end] != 0:
                continue
            text = raw[start:end].decode("latin-1")
            out.append(Str(section.virtual_address + start, section.name, text))
    return out


# --- cross references -----------------------------------------------------------------

def find_dword_sites(pe: PE32, section_name: str, wanted: np.ndarray) -> dict[int, list[int]]:
    """Every offset in `section_name` holding a little-endian dword that is in `wanted`.

    Unaligned included -- x86 immediates are not aligned.  Returns {value: [site_va, ...]}.
    """
    section = next(s for s in pe.sections if s.name == section_name)
    raw = pe.data[section.raw_offset:section.raw_offset + section.raw_size]
    buf = np.frombuffer(raw, dtype=np.uint8).astype(np.uint32)
    if buf.size < 4:
        return {}
    # Build the dword at every byte offset in one vectorised shot.
    dwords = (buf[:-3] | (buf[1:-2] << 8) | (buf[2:-1] << 16) | (buf[3:] << 24))
    hits = np.flatnonzero(np.isin(dwords, wanted))
    result: dict[int, list[int]] = {}
    for offset in hits.tolist():
        result.setdefault(int(dwords[offset]), []).append(section.virtual_address + offset)
    return result


# --- function attribution -------------------------------------------------------------

class FunctionMap:
    """(start, size) pairs from callgraph.tsv, queried by binary search."""

    def __init__(self, path: str):
        self.starts: list[int] = []
        self.ends: list[int] = []
        with open(path, encoding="latin-1") as handle:
            for line in handle:
                parts = line.rstrip("\n").split("\t")
                if len(parts) < 2:
                    continue
                start = int(parts[0], 16)
                self.starts.append(start)
                self.ends.append(start + int(parts[1]))
        order = sorted(range(len(self.starts)), key=lambda i: self.starts[i])
        self.starts = [self.starts[i] for i in order]
        self.ends = [self.ends[i] for i in order]

    def containing(self, va: int) -> int | None:
        index = bisect.bisect_right(self.starts, va) - 1
        if index < 0:
            return None
        return self.starts[index] if va < self.ends[index] else None


# --- pointer tables -------------------------------------------------------------------

def find_tables(pe: PE32, string_vas: set[int], by_va: dict[int, Str]) -> list[list[int]]:
    """Runs of >= MIN_TABLE consecutive aligned dwords, each pointing at a string.

    Enum-to-name tables are compiler-emitted arrays of `const char *`, so they are dword
    aligned and contiguous.  Requiring alignment and contiguity is what keeps the false
    positive rate low; scattered pointers into string space are far too common to be
    interesting.
    """
    tables: list[list[int]] = []
    wanted = np.fromiter(string_vas, dtype=np.uint32, count=len(string_vas))
    wanted.sort()
    for section in pe.sections:
        if section.name not in (".rdata", ".data"):
            continue
        raw = pe.data[section.raw_offset:section.raw_offset + section.raw_size]
        count = len(raw) // 4
        dwords = np.frombuffer(raw[:count * 4], dtype="<u4")
        is_ptr = np.isin(dwords, wanted)
        # Walk the boolean run-length encoding for stretches of True.
        index = 0
        flat = is_ptr.tolist()
        while index < count:
            if not flat[index]:
                index += 1
                continue
            run = index
            while run < count and flat[run]:
                run += 1
            if run - index >= MIN_TABLE:
                base = section.virtual_address + index * 4
                tables.append([base] + [int(dwords[i]) for i in range(index, run)])
            index = run
    return tables


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", default=os.path.expanduser(
        "~/projects/llm/input/recoil-metal/retail-fa/bin/SupremeCommander.exe"))
    parser.add_argument("--callgraph", default="build/re-fa/exports/callgraph.tsv")
    parser.add_argument("--outdir", default="build/re-fa/exports")
    args = parser.parse_args()

    pe = PE32(args.exe)
    strings = extract_strings(pe)
    by_va = {s.va: s for s in strings}
    print(f"strings: {len(strings)}", file=sys.stderr)

    os.makedirs(args.outdir, exist_ok=True)
    with open(os.path.join(args.outdir, "strings.all.tsv"), "w", encoding="latin-1") as out:
        out.write("va\tsection\tkind\tlength\ttext\n")
        for s in sorted(strings, key=lambda s: s.va):
            out.write(f"{s.va:08x}\t{s.section}\t{s.kind}\t{len(s.text)}\t"
                      f"{s.text.replace(chr(9), '\\t').replace(chr(10), '\\n').replace(chr(13), '\\r')}\n")

    # Only cross-reference the strings that carry meaning; xrefing all 200k+ aliases badly.
    interesting = [s for s in strings
                   if s.kind in ("format", "diagnostic", "enumlike")
                   and len(s.text) >= MIN_XREF_STRING]
    wanted = np.fromiter({s.va for s in interesting}, dtype=np.uint32)
    wanted.sort()
    sites = find_dword_sites(pe, ".text", wanted)
    print(f"interesting: {len(interesting)}  with .text sites: {len(sites)}", file=sys.stderr)

    functions = FunctionMap(args.callgraph)
    with open(os.path.join(args.outdir, "strings.xrefs.tsv"), "w", encoding="latin-1") as out:
        out.write("string_va\tkind\tsite_va\tfunction_va\ttext\n")
        for s in sorted(interesting, key=lambda s: s.va):
            text = s.text[:200].replace("\t", " ").replace("\n", "\\n").replace("\r", "\\r")
            for site in sites.get(s.va, []):
                func = functions.containing(site)
                func_field = f"{func:08x}" if func is not None else "-"
                out.write(f"{s.va:08x}\t{s.kind}\t{site:08x}\t{func_field}\t{text}\n")

    tables = find_tables(pe, set(by_va), by_va)
    print(f"pointer-run tables: {len(tables)}", file=sys.stderr)
    with open(os.path.join(args.outdir, "strings.tables.tsv"), "w", encoding="latin-1") as out:
        out.write("table_va\tcount\tindex\tstring_va\ttext\n")
        for table in tables:
            base, pointers = table[0], table[1:]
            for index, pointer in enumerate(pointers):
                text = by_va[pointer].text.replace("\t", " ")
                out.write(f"{base:08x}\t{len(pointers)}\t{index}\t{pointer:08x}\t{text}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
