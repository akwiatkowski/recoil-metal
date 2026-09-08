#!/usr/bin/env python3
"""Run and stop tracked offscreen AI matches with one narrowly scoped approval rule."""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
BINARY = ROOT / "build/recoil-metal"
JOBS = ROOT / "build/ai-matches"
PERSONALITIES = ("easy", "medium", "tech", "rushland", "rushair", "rushnaval",
                 "rushbalanced", "turtle", "adaptive", "random")


def job_name(value):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,79}", value):
        raise argparse.ArgumentTypeError("use 1–80 letters, digits, underscores or hyphens")
    return value


def positive(value):
    result = int(value)
    if result <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return result


def command(args, job):
    # No shell, arbitrary executable or extra-arguments escape hatch. Every run is offscreen.
    return [str(BINARY), str(args.fa_root / "maps" / args.map / f"{args.map}.scmap"),
            "--gamedata", str(args.fa_root / "gamedata"), "--skirmish", "--observer",
            "--armies", str(args.armies), "--ai-personality", args.personality,
            "--ai-sanity", "--mute", "--play", str(args.seconds),
            "--command-log", str(job / "commands.log"), "--hash-log", str(job / "ticks.hash"),
            "--screenshot", str(job / "final.png"), "1000", "700"]


def process_identity(pid):
    # macOS ps supplies creation time plus the full executable/arguments. A recycled
    # PID is insufficient authority to signal a process; both must still match.
    result = subprocess.run(["ps", "-ww", "-p", str(pid), "-o", "lstart=", "-o", "command="],
                            capture_output=True, text=True)
    if result.returncode == 1 and not result.stdout.strip():
        return None
    result.check_returncode()
    return result.stdout.strip() or None


def save(job, state):
    temporary = job / "process.json.tmp"
    temporary.write_text(json.dumps(state, indent=2) + "\n")
    temporary.replace(job / "process.json")


def stop(job, force):
    state = json.loads((job / "process.json").read_text())
    if "returncode" in state:
        print(f"{job.name}: already finished ({state['returncode']})")
        return
    identity = process_identity(state["pid"])
    if identity is None:
        print(f"{job.name}: process has exited")
        return
    if not state.get("identity") or identity != state["identity"]:
        raise RuntimeError(f"{job.name}: PID is not the recorded match; refusing to signal it")
    try:
        os.kill(state["pid"], signal.SIGKILL if force else signal.SIGTERM)
    except ProcessLookupError:
        pass  # It completed between the identity check and signal.
    print(f"{job.name}: {'kill' if force else 'termination'} requested")


def status(job):
    state = json.loads((job / "process.json").read_text())
    if "returncode" in state:
        label = f"finished (exit {state['returncode']})"
    elif state.get("identity") and process_identity(state["pid"]) == state["identity"]:
        label = f"running (PID {state['pid']})"
    else:
        label = "not running; completion was not recorded"
    print(f"{job.name}: {label}\n  artifacts: {job}")


def run(args, job):
    argv = command(args, job)
    if not BINARY.is_file() or not Path(argv[1]).is_file():
        raise RuntimeError("build/recoil-metal and the selected map must exist before starting")
    job.mkdir(parents=True, exist_ok=False)  # A fresh job never overwrites earlier evidence.
    with (job / "stdout.log").open("w") as stdout, (job / "stderr.log").open("w") as stderr:
        process = subprocess.Popen(argv, cwd=ROOT, stdout=stdout, stderr=stderr,
                                   start_new_session=True)
        try:
            state = {"pid": process.pid, "command": argv,
                     "identity": process_identity(process.pid)}
            save(job, state)
            print(f"{job.name}: started PID {process.pid}\n  artifacts: {job}", flush=True)
            try:
                code = process.wait(timeout=args.timeout)
            except (KeyboardInterrupt, subprocess.TimeoutExpired) as error:
                state["stop_reason"] = type(error).__name__
                process.terminate()
                try:
                    code = process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    code = process.wait()
            state["returncode"] = code
            save(job, state)
        finally:
            # Even a failed identity lookup or metadata write must not orphan a match.
            if process.poll() is None:
                process.kill()
                process.wait()
    status(job)
    return 0 if code == 0 else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest="action", required=True)
    start = actions.add_parser("run", help="start a tracked match; may run in a tool session")
    start.add_argument("name", type=job_name)
    start.add_argument("--seconds", type=positive, default=1800, help="simulation duration")
    start.add_argument("--timeout", type=positive, help="optional wall-clock limit in seconds")
    start.add_argument("--personality", choices=PERSONALITIES, default="tech")
    start.add_argument("--armies", type=int, choices=range(2, 9), default=2)
    start.add_argument("--map", type=job_name, default="SCMP_009")
    start.add_argument("--fa-root", type=Path,
                       default=Path("/Volumes/Samsung_T5/faf/Supreme Commander Forged Alliance"))
    inspect = actions.add_parser("status", help="report one job or all tracked jobs")
    inspect.add_argument("name", type=job_name, nargs="?")
    terminate = actions.add_parser("stop", help="stop only the recorded match process")
    terminate.add_argument("name", type=job_name)
    terminate.add_argument("--force", action="store_true", help="SIGKILL instead of SIGTERM")
    args = parser.parse_args()
    try:
        if args.action == "status" and args.name is None:
            for record in sorted(JOBS.glob("*/process.json")):
                status(record.parent)
            return 0
        job = JOBS / args.name
        if args.action == "run":
            return run(args, job)
        if args.action == "stop":
            stop(job, args.force)
        else:
            status(job)
        return 0
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        print(f"ai-match: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
