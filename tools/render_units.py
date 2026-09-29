#!/usr/bin/env python3
"""Render every unit/building that has a wiki build-button icon, headless, and
compose side-by-side comparison sheets against the wiki images.

Why this exists: the engine reads the game's own `.scm` meshes and `.dds`
textures out of the mounted `.scd` archives, so a unit that LOOKS wrong is a
rendering bug (wrong mesh, missing texture, bad pose), not a modelling one. The
wiki mirror under `reference/supcom-wiki/` ships a `*_build_btn.png` icon per
unit page — the game's own build-button render — which is the closest available
ground truth for "does this unit look right".

The capture is `--screenshot` on a flat studio map with `--camera 45 45` and
`--no-hud`: the icon's approximate three-quarter view with no interface over it.

Usage:
    tools/render_units.py                 # render + sheets + report skeleton
    tools/render_units.py UEL0201 ...     # only those ids
    tools/render_units.py --sheets-only   # recompose sheets from existing pngs

Outputs (all gitignored scratch, under build/unit-icons/):
    build/unit-icons/renders/<ID>.png     the engine's frame
    build/unit-icons/sheets/<ID>.png      render | wiki icon side by side
    build/unit-icons/index.html           every pair, scrollable
"""

import json
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent

BINARY = ROOT / "build/recoil-metal"
GAMEDATA = "/Users/olek/projects/llm/input/recoil-metal/retail-fa/gamedata"
MAP = "/tmp/flat.smf"
IMAGES = ROOT / "reference/supcom-wiki/images"
OUT = ROOT / "build/unit-icons"
RENDERS = OUT / "renders"
SHEETS = OUT / "sheets"

# The capture recipe. `--camera 45 45` approximates the build-button view; yaw 45
# faces the unit the way the icons face it. `--focus` is in model radii: 2.2
# reads like the icon's crop without cutting tall structures' tops off in most
# cases; experimentals get 2.8 so UES0401's legs are not clipped.
PITCH = 45
YAW = 45
SHOT = 512

def ensure_flat_map() -> None:
    """Write the studio map if absent: a legal Recoil SMF, 128x128 squares of
    dead-flat ground — enough terrain to sit a unit on, no relief to hide it."""
    import struct
    if Path(MAP).exists():
        return
    mapx = mapy = 128  # multiples of 128 are required by the header checks
    header = bytearray(80)
    header[0:16] = b"spring map file\x00"
    struct.pack_into("<i", header, 16, 1)          # version
    struct.pack_into("<i", header, 24, mapx)
    struct.pack_into("<i", header, 28, mapy)
    struct.pack_into("<i", header, 32, 8)          # squareSize
    struct.pack_into("<i", header, 36, 8)          # texelPerSquare
    struct.pack_into("<i", header, 40, 32)         # tileSize
    struct.pack_into("<f", header, 44, 0.0)        # minHeight
    struct.pack_into("<f", header, 48, 100.0)      # maxHeight
    hptr = 80
    samples = (mapx + 1) * (mapy + 1)
    tptr = hptr + samples * 2
    struct.pack_into("<i", header, 52, hptr)       # heightmapPtr
    struct.pack_into("<i", header, 60, tptr)       # tilesPtr
    tiles = struct.pack("<ii", 0, 0) + b"\x00" * ((mapx // 4) * (mapy // 4) * 4)
    Path(MAP).write_bytes(bytes(header) + b"\x00" * (samples * 2) + tiles)


def icon_ids() -> list[str]:
    return sorted(p.name[: -len("_build_btn.png")]
                  for p in IMAGES.glob("*_build_btn.png"))


def blueprint_path(unit_id: str) -> str:
    return f"/units/{unit_id}/{unit_id}_unit.bp"


def focus_for(unit_id: str) -> float:
    # The second tier digit of the id distinguishes experimentals (4xxx) and
    # ACUs/SCUs (00xx-ish); experimentals are tall and clip at 2.2.
    experimental = unit_id[3] == "4" or unit_id in {"UEL0001", "URL0001",
                                                  "UAL0001", "XSL0001"}
    return 2.8 if experimental else 2.2


def render(unit_id: str) -> tuple[str, bool, str]:
    out = RENDERS / f"{unit_id}.png"
    cmd = [
        str(BINARY), MAP, "--gamedata", GAMEDATA,
        "--units", blueprint_path(unit_id), "1",
        "--focus", str(focus_for(unit_id)),
        "--camera", str(PITCH), str(YAW),
        "--no-hud", "--no-interpolate", "--log-level", "error",
        "--screenshot", str(out), str(SHOT), str(SHOT),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    ok = proc.returncode == 0 and out.exists() and out.stat().st_size > 0
    return unit_id, ok, (proc.stdout + proc.stderr)[-4000:]


def sheet(unit_id: str) -> None:
    render_png = RENDERS / f"{unit_id}.png"
    icon_png = IMAGES / f"{unit_id}_build_btn.png"
    canvas = Image.new("RGB", (SHOT + 280, SHOT + 28), (24, 24, 28))
    draw = ImageDraw.Draw(canvas)
    if render_png.exists():
        canvas.paste(Image.open(render_png).convert("RGB"), (0, 28))
    if icon_png.exists():
        icon = Image.open(icon_png).convert("RGBA")
        # Checkerboard behind the icon so its transparent edge is visible.
        bg = Image.new("RGB", icon.size, (110, 60, 110))
        checker = Image.new("RGB", icon.size)
        d = ImageDraw.Draw(checker)
        c = 0
        for y in range(0, icon.size[1], 8):
            for x in range(0, icon.size[0], 8):
                if (x // 8 + y // 8) % 2 == 0:
                    d.rectangle([x, y, x + 7, y + 7], fill=(150, 150, 150))
        bg.paste(checker, (0, 0))
        icon = Image.alpha_composite(bg.convert("RGBA"), icon).convert("RGB")
        icon = icon.resize((256, 256), Image.NEAREST)
        canvas.paste(icon, (SHOT + 12, 28 + 60))
    draw.text((6, 6), f"{unit_id} — engine render", fill=(235, 235, 235))
    draw.text((SHOT + 12, 6), "wiki build_btn", fill=(235, 235, 235))
    canvas.save(SHEETS / f"{unit_id}.png")


def index_html(ids: list[str]) -> None:
    rows = "\n".join(
        f'<tr><td>{u}</td><td><img src="sheets/{u}.png" width="600"></td></tr>'
        for u in ids)
    (OUT / "index.html").write_text(
        "<html><body style='background:#18181c'><table>" + rows + "</table></body></html>")


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    sheets_only = "--sheets-only" in sys.argv
    ids = [a.upper() for a in args] if args else icon_ids()
    RENDERS.mkdir(parents=True, exist_ok=True)
    ensure_flat_map()
    SHEETS.mkdir(parents=True, exist_ok=True)

    failures: dict[str, str] = {}
    if not sheets_only:
        done = 0
        with ThreadPoolExecutor(max_workers=8) as pool:
            for unit_id, ok, log in pool.map(render, ids):
                done += 1
                tag = "ok" if ok else "FAIL"
                print(f"[{done:3}/{len(ids)}] {unit_id} {tag}", flush=True)
                if not ok:
                    failures[unit_id] = log
        if failures:
            print(f"\n{len(failures)} failed:")
            for unit_id, log in failures.items():
                print(f"== {unit_id}\n{log}\n")

    for unit_id in ids:
        try:
            sheet(unit_id)
        except Exception as exc:  # noqa: BLE001 — report, keep going
            print(f"sheet {unit_id}: {exc}")
    index_html(ids)
    print(f"wrote {OUT}/index.html ({len(ids)} units)")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
