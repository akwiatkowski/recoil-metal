#!/usr/bin/env python3
"""Extract the shipped script/blueprint corpus from retail Forged Alliance `.scd` archives.

Background
----------
Retail FA ships its game data in `.scd` files, which are plain ZIP archives (claim `C-025`).
The shipped Lua is NOT confined to `lua.scd`: `mohodata.scd`, `schook.scd`, `units.scd`,
`projectiles.scd`, `props.scd`, `effects.scd` and `mods.scd` all carry `.lua` and/or `.bp`
files.  Searching only `lua.scd` produces clean false negatives (evidence trap 4, `C-193`).

This tool therefore does two things:

1. Writes a *full inventory* (every archive, every entry, grouped by extension) so the
   denominator of any "N files contain no X" claim can be reproduced.
2. Extracts every text-ish entry (`.lua`, `.bp`, `.lst`, `.sca`, `.txt`, `.cfg`, `.nfo`)
   into `<out>/<archive-stem>/<path-inside-archive>` so tooling can grep a real filesystem.

Encoding: shipped files are ISO-8859-1 (latin-1) with CRLF endings, and `lua/sim/Unit.lua`
contains a literal NUL byte.  Bytes are written through verbatim; consumers must decode as
latin-1, never utf-8, and must use `grep -a` (see evidence traps 1 and 3 in the plan).

The source corpus is READ-ONLY: this script never writes inside it.

Usage:
    python3 extract_scd_corpus.py <gamedata-dir> <out-dir>
"""

from __future__ import annotations

import json
import sys
import zipfile
from collections import Counter
from pathlib import Path

# Entries worth putting on disk. Everything else (textures, meshes, sounds) is skipped:
# it is gigabytes of binary that no textual search will ever match.
TEXT_SUFFIXES = {".lua", ".bp", ".lst", ".sca", ".txt", ".cfg", ".nfo", ".scd_manifest"}


def inventory_archive(path: Path) -> dict:
    """Return per-extension entry counts plus the raw name list for one archive."""
    with zipfile.ZipFile(path) as zf:
        names = [i.filename for i in zf.infolist() if not i.is_dir()]
    ext_counts = Counter(Path(n).suffix.lower() or "<none>" for n in names)
    return {"entries": len(names), "by_ext": dict(ext_counts), "names": names}


def extract_archive(path: Path, out_root: Path) -> int:
    """Extract the text-ish entries of one archive into `out_root/<stem>/...`.

    Returns the number of files written.  Paths inside the archive use backslashes in
    some SCDs, so they are normalised to `/` before being joined.
    """
    dest_root = out_root / path.stem
    written = 0
    with zipfile.ZipFile(path) as zf:
        for info in zf.infolist():
            if info.is_dir():
                continue
            name = info.filename.replace("\\", "/")
            if Path(name).suffix.lower() not in TEXT_SUFFIXES:
                continue
            # Defend against path traversal in a hostile/odd archive.
            rel = Path(*[p for p in name.split("/") if p not in ("", ".", "..")])
            dest = dest_root / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(zf.read(info))
            written += 1
    return written


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__)
        return 2
    src = Path(argv[1]).expanduser()
    out = Path(argv[2]).expanduser()
    out.mkdir(parents=True, exist_ok=True)

    report: dict[str, dict] = {}
    for scd in sorted(src.glob("*.scd")):
        if not zipfile.is_zipfile(scd):
            report[scd.name] = {"error": "not a zip archive"}
            print(f"{scd.name:24s} NOT A ZIP")
            continue
        inv = inventory_archive(scd)
        n = extract_archive(scd, out)
        inv["extracted"] = n
        report[scd.name] = inv
        lua = inv["by_ext"].get(".lua", 0)
        bp = inv["by_ext"].get(".bp", 0)
        print(f"{scd.name:24s} entries={inv['entries']:7d}  lua={lua:5d}  bp={bp:5d}  extracted={n}")

    # `names` is huge for units/env/textures; keep it out of the summary file.
    summary = {k: {kk: vv for kk, vv in v.items() if kk != "names"} for k, v in report.items()}
    (out / "inventory.json").write_text(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
