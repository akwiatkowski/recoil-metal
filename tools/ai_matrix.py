#!/usr/bin/env python3
"""Offline AI personality x faction matrix: slow headless 1v1s, one JSON each.

NOT part of the test suite — a manual overnight-style harness. For every run it
plays a full 1v1 on the default map at headless speed (victory or the sim cap),
streams progress lines, then prints the outcome table and writes one JSON doc
(config + snapshots + final outcome) for later processing.

Each run's binary invocation mirrors the `ai-sanity` make target: --play is the
sim TIME CAP (a victory ends the run early via --matrix-out), --timeout is the
wall failsafe, and --screenshot is what lets the pre-run exit without opening
a window — never drop it.
"""
import argparse
import datetime
import itertools
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BINARY = ROOT / "build/recoil-metal"
JOBS = ROOT / "build/ai-matrix"
PERSONALITIES = ("easy", "medium", "tech", "rushland", "rushair", "rushnaval",
                 "rushbalanced", "turtle", "adaptive", "random")
FACTIONS = ("uef", "aeon", "cybran", "seraphim")
DEFAULT_PERSONALITIES = ("easy", "turtle", "tech")


def job_name(value):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,79}", value):
        raise argparse.ArgumentTypeError("use 1-80 letters, digits, underscores or hyphens")
    return value


def personality_list(value):
    names = [name.strip() for name in value.split(",") if name.strip()]
    unknown = [name for name in names if name not in PERSONALITIES]
    if not names or unknown:
        raise argparse.ArgumentTypeError(f"choose from {', '.join(PERSONALITIES)}")
    return names


def faction_pair(value):
    names = [name.strip() for name in value.split(",") if name.strip()]
    if len(names) != 2 or any(name not in FACTIONS for name in names):
        raise argparse.ArgumentTypeError("use two factions, e.g. uef,cybran")
    return names


def positive(value):
    result = int(value)
    if result <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return result


def commit_hash():
    # The binary cannot know its own revision; the driver stamps every JSON with it
    # so results stay comparable only by hash, explicitly.
    result = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                            capture_output=True, text=True)
    result.check_returncode()
    return result.stdout.strip()


def build_binary():
    # Always the latest compiled binary: a matrix result means nothing if the binary
    # predates the commit stamped on it.
    subprocess.run(["mise", "exec", "--", "cmake", "--build", "build",
                    "--target", "recoil-metal"], cwd=ROOT, check=True)


def run_pairs(personalities):
    # Mirrors plus one game per unordered cross pair: p0-vs-p1 covers the matchup
    # (seat order is symmetric on SCMP_009's fixed spawns), the reverse fixture
    # doubles wall time for no new information.
    seen = []
    for name in personalities:
        if (name, name) not in seen:
            seen.append((name, name))
    for first, second in itertools.combinations(personalities, 2):
        seen.append((first, second))
    return seen


def command(args, commit, run_dir, personalities, factions):
    return [str(BINARY), str(args.fa_root / "maps" / args.map / f"{args.map}.scmap"),
            "--gamedata", str(args.fa_root / "gamedata"), "--skirmish", "--observer",
            "--armies", "2", "--factions", ",".join(factions),
            "--ai-personalities", ",".join(personalities),
            "--mute", "--play", str(args.seconds),
            "--matrix-out", str(run_dir / "result.json"),
            "--matrix-commit", commit,
            "--report-interval", str(args.interval),
            "--screenshot", str(run_dir / "final.png"), "320", "180"]


def play(args, commit, run_dir, personalities, factions):
    """Play one run in the foreground, streaming the binary's output to both the
    console and stdout.log. Returns True when result.json was produced."""
    argv = command(args, commit, run_dir, personalities, factions)
    if args.dry_run:
        print(" ".join(argv))
        return True
    if not BINARY.is_file():
        raise RuntimeError("build/recoil-metal is missing; drop --no-build only after building")
    run_dir.mkdir(parents=True, exist_ok=False)
    (run_dir / "config.json").write_text(json.dumps({
        "personalities": list(personalities), "factions": list(factions),
        "map": args.map, "seconds": args.seconds, "commit": commit,
        "command": argv}, indent=2) + "\n")
    label = f"{personalities[0]}-vs-{personalities[1]} ({'/'.join(factions)})"
    print(f"### {label}\n  {' '.join(argv)}", flush=True)
    timed_out = False
    with (run_dir / "stdout.log").open("w") as stdout, \
         (run_dir / "stderr.log").open("w") as stderr:
        process = subprocess.Popen(argv, cwd=ROOT, stdout=subprocess.PIPE,
                                   stderr=stderr, text=True, start_new_session=True)
        try:
            for line in process.stdout:
                stdout.write(line)
                print(line, end="", flush=True)
            code = process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            process.terminate()
            try:
                code = process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                code = process.wait()
            for line in process.stdout:
                stdout.write(line)
                print(line, end="", flush=True)
    if timed_out:
        # No result.json: SIGTERM kills the binary before it reports. The timeout
        # record keeps the config so the gap is visible, not silently missing.
        (run_dir / "timeout.json").write_text(json.dumps({
            "personalities": list(personalities), "factions": list(factions),
            "map": args.map, "seconds": args.seconds, "commit": commit,
            "timeout_seconds": args.timeout}, indent=2) + "\n")
        print(f"  TIMEOUT after {args.timeout}s wall — no result.json", flush=True)
        return False
    if code != 0 or not (run_dir / "result.json").is_file():
        print(f"  FAILED (exit {code}) — no result.json", flush=True)
        return False
    return True


def summarize(run_dir):
    doc = json.loads((run_dir / "result.json").read_text())
    outcome = "cap-reached"
    if doc["finished"]:
        outcome = f"team {doc['winner']} wins" if doc["winner"] is not None else "draw"
    parts = [f"{run_dir.name}: {outcome} at tick {doc['ticksPlayed']}"]
    for army in doc["armies"]:
        parts.append(f"  army {army['army']} ({army['faction']}/{army['personality']}): "
                     f"{army['alive']} alive, +{army['generatedMass']:.0f}/"
                     f"+{army['generatedEnergy']:.0f} generated, {army['kills']} kills "
                     f"(worth {army['killMassWorth']:.0f}/{army['killEnergyWorth']:.0f})")
    return "\n".join(parts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", type=job_name, default=None,
                        help="job dir under build/ai-matrix (default: matrix-<timestamp>)")
    parser.add_argument("--personalities", type=personality_list,
                        default=list(DEFAULT_PERSONALITIES),
                        help="comma list; mirrors + one game per cross pair")
    parser.add_argument("--factions", type=faction_pair, default=["uef", "uef"],
                        help="one pair for every run, e.g. uef,cybran")
    parser.add_argument("--seconds", type=positive, default=3600,
                        help="sim time cap per run (a victory ends it early)")
    parser.add_argument("--timeout", type=positive, default=3600,
                        help="wall-clock failsafe per run, in seconds")
    parser.add_argument("--interval", type=positive, default=15,
                        help="progress line cadence, in wall seconds")
    parser.add_argument("--map", type=job_name, default="SCMP_009")
    parser.add_argument("--fa-root", type=Path,
                        default=Path("/Volumes/Samsung_T5/faf/Supreme Commander Forged Alliance"))
    parser.add_argument("--no-build", action="store_true", help="skip the rebuild")
    parser.add_argument("--dry-run", action="store_true", help="print commands, run nothing")
    args = parser.parse_args()

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    job = JOBS / (args.name or f"matrix-{stamp}")
    try:
        commit = commit_hash()
        if not args.no_build and not args.dry_run:
            build_binary()
        pairs = run_pairs(args.personalities)
        print(f"matrix: {len(pairs)} run(s) -> {job} (commit {commit[:12]})", flush=True)
        results = []
        for first, second in pairs:
            run_dir = job / f"{first}-vs-{second}_{'-'.join(args.factions)}"
            ok = play(args, commit, run_dir, (first, second), args.factions)
            results.append((run_dir, ok))
        print("\n=== MATRIX SUMMARY " + "=" * 60)
        if args.dry_run:
            print(f"matrix: dry run, {len(results)} command(s) printed, nothing executed")
            return 0
        failures = 0
        for run_dir, ok in results:
            if ok:
                print(summarize(run_dir))
            else:
                print(f"{run_dir.name}: NO RESULT")
                failures += 1
        print(f"matrix: {len(results) - failures}/{len(results)} run(s) produced JSON -> {job}")
        return 1 if failures else 0
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        print(f"ai-matrix: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
