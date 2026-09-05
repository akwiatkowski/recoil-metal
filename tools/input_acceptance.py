#!/usr/bin/env python3
"""Run real AppKit mouse input through all 23 T1 land products and three HUD profiles."""

import argparse
from pathlib import Path
import struct
import subprocess


PROFILES = {
    "compact": (1280, 720, "1"),
    "standard": (1600, 900, "0.8"),
    "wide": (2240, 1260, "0.5"),
}
FACTORIES = {"UEB0101": "uef", "UAB0101": "aeon", "URB0101": "cybran", "XSB0101": "seraphim"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build/recoil-metal"))
    parser.add_argument("--map", type=Path, required=True)
    content = parser.add_mutually_exclusive_group(required=True)
    content.add_argument("--gamedata", type=Path)
    content.add_argument("--data-dir", type=Path, help="an extracted VFS tree; logs retain missing-asset warnings")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--profiles", nargs="+", choices=PROFILES, default=list(PROFILES))
    parser.add_argument("--factories", nargs="+", choices=FACTORIES, default=list(FACTORIES))
    parser.add_argument("--backings", nargs="+", choices=(1, 2), type=int, default=[1, 2])
    args = parser.parse_args()
    content_path = args.gamedata or args.data_dir
    content_flag = "--gamedata" if args.gamedata else "--data-dir"
    for path in (args.binary, args.map, content_path):
        if not path.exists():
            parser.error(f"required input is unavailable: {path}")
    args.output.mkdir(parents=True, exist_ok=True)
    runs = 0
    for profile in args.profiles:
        width, height, ui_scale = PROFILES[profile]
        for backing in args.backings:
            for factory in args.factories:
                stem = args.output / f"{profile}-{backing}x-{factory}"
                image = stem.with_suffix(".png")
                log = stem.with_suffix(".log")
                command = [str(args.binary.resolve()), str(args.map.resolve()),
                    content_flag, str(content_path.resolve()), "--skirmish", "--armies", "2",
                    "--factions", FACTORIES[factory], "--units", f"/units/{factory}/{factory}_unit.bp", "1",
                    "--window", str(width), str(height), "--ui-scale", ui_scale,
                    "--backing", str(backing), "--input-acceptance", str(image.resolve()), "--mute"]
                print(f"{profile} simulated {backing}x {factory}", flush=True)
                with log.open("w") as output:
                    try:
                        result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT, timeout=240)
                    except subprocess.TimeoutExpired:
                        raise SystemExit(f"FAIL timeout; see {log}") from None
                text = log.read_text()
                expected = 5 if factory == "XSB0101" else 6
                if result.returncode or f"input acceptance: PASS {expected} products," not in text:
                    raise SystemExit(f"FAIL input acceptance; see {log}")
                if text.count("production, selection, Move, Shift queue, Stop, input swallowing PASS") != expected:
                    raise SystemExit(f"FAIL incomplete per-product evidence; see {log}")
                if "production pagination, pending/active cancellation, Clear Queue PASS" not in text:
                    raise SystemExit(f"FAIL missing production-control evidence; see {log}")
                if text.count("native Guard target and Stop PASS") != expected:
                    raise SystemExit(f"FAIL incomplete Guard evidence; see {log}")
                png = image.read_bytes()
                if png[:8] != b"\x89PNG\r\n\x1a\n" or struct.unpack(">II", png[16:24]) != (width * backing, height * backing):
                    raise SystemExit(f"FAIL capture dimensions: {image}")
                runs += 1
    print(f"PASS {runs} native-input runs; logs and captures: {args.output}")


if __name__ == "__main__":
    main()
