#!/usr/bin/env python3
"""Latin-1 safe regex search over the extracted retail FA corpus.

Why not `grep`: on this machine `grep` is `ugrep` invoked with `-I` (skip binary).  The
shipped Lua is ISO-8859-1 and `lua/sim/Unit.lua` contains a literal NUL byte, so `grep`
silently drops matches (evidence trap 1).  `LC_ALL=C grep -a` fixes it, but any negative
result that matters deserves a second, independent implementation.  This is that one.

It reads bytes and matches bytes, so no decoding step can ever drop a file, and it prints
a per-archive breakdown so a "zero hits" answer always comes with its denominator.

Usage:
    python3 grep_corpus.py <corpus-root> <regex> [--ext .lua,.bp] [--count] [--files]
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import Counter
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("root", type=Path)
    ap.add_argument("pattern")
    ap.add_argument("--ext", default=".lua", help="comma-separated suffixes, or 'all'")
    ap.add_argument("--count", action="store_true", help="only per-archive totals")
    ap.add_argument("--files", action="store_true", help="only matching file paths")
    ap.add_argument("-i", "--ignorecase", action="store_true")
    args = ap.parse_args()

    flags = re.IGNORECASE if args.ignorecase else 0
    rx = re.compile(args.pattern.encode("latin-1"), flags)
    exts = None if args.ext == "all" else {e.strip().lower() for e in args.ext.split(",")}

    scanned: Counter[str] = Counter()
    hit_files: Counter[str] = Counter()
    hit_lines: Counter[str] = Counter()
    out: list[str] = []

    for path in sorted(args.root.rglob("*")):
        if not path.is_file():
            continue
        if exts is not None and path.suffix.lower() not in exts:
            continue
        # First path component under the root is the archive stem.
        archive = path.relative_to(args.root).parts[0]
        scanned[archive] += 1
        data = path.read_bytes()
        if not rx.search(data):
            continue
        hit_files[archive] += 1
        # splitlines() on bytes also splits on \r, which is right for CRLF files.
        for n, line in enumerate(data.split(b"\n"), 1):
            if rx.search(line):
                hit_lines[archive] += 1
                if not args.count and not args.files:
                    out.append(f"{path.relative_to(args.root)}:{n}: {line.decode('latin-1').strip()}")
        if args.files:
            out.append(str(path.relative_to(args.root)))

    for line in out:
        print(line)
    print("--- per-archive: matched_files/scanned_files (matching lines) ---", file=sys.stderr)
    for archive in sorted(scanned):
        print(f"{archive:14s} {hit_files[archive]:5d}/{scanned[archive]:5d}  ({hit_lines[archive]} lines)", file=sys.stderr)
    print(f"TOTAL          {sum(hit_files.values())}/{sum(scanned.values())} files, {sum(hit_lines.values())} lines", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
