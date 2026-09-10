#!/usr/bin/env python3
"""Offline AI personality x faction matrix: slow headless 1v1s, one JSON each.

NOT part of the test suite — a manual overnight-style harness. For every run it
plays a full 1v1 on the default map at headless speed (victory or the sim cap),
keeps a live scoreboard on the console, then prints one card per run, a
personality-vs-personality results grid, and writes one JSON doc per run
(config + snapshots + final outcome) for later processing.

THE CONSOLE SHOWS ONLY WHAT MATTERS WHILE A RUN PLAYS. The binary narrates
every asset load, every completed building and every attack wave — two
thousand lines a run — and all of that still lands in each run's stdout.log.
What reaches the terminal is: which run is playing and how far along the sim
clock is, one row per army (standing units, income, kills), a two-sided
front-line bar of who holds more of the field, the templates FAF chose, and
the latest attack wave. On a terminal the block redraws in place; in a pipe
it appends one block per progress report.

Each run's binary invocation mirrors the `ai-sanity` make target: --play is the
sim TIME CAP (a victory ends the run early via --matrix-out), --timeout is the
wall failsafe, and --screenshot is what lets the pre-run exit without opening
a window — never drop it.

`--report <name>` re-renders the cards and grid of a finished job without
playing anything.
"""
import argparse
import datetime
import itertools
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BINARY = ROOT / "build/recoil-metal"
JOBS = ROOT / "build/ai-matrix"
PERSONALITIES = ("easy", "medium", "tech", "rushland", "rushair", "rushnaval",
                 "rushbalanced", "turtle", "adaptive", "random")
FACTIONS = ("uef", "aeon", "cybran", "seraphim")
# How the binary spells them in its progress lines and the JSON (`rm::sim::factionName`).
FACTION_TITLE = {"uef": "UEF", "aeon": "Aeon", "cybran": "Cybran", "seraphim": "Seraphim"}
DEFAULT_PERSONALITIES = ("easy", "turtle", "tech")
# `rm::sim::kDefaultTicksPerSecond`: the JSON and the progress lines count ticks, the
# console shows a sim clock.
TICKS_PER_SECOND = 10

# --- What the binary prints that the console keeps ----------------------------------------
#
# Anchored on the exact formats in `Match.cpp` (`printMatrixProgress`, the FAF seating
# report and the tick narration). A format change there shows up here as a silent
# console — which is why `test_ai_matrix.py` pins these against the literal lines.
PROGRESS = re.compile(r"^matrix: \[tick (\d+), (\d+)s wall\] army (\d+) \((\w+)/(\w+)\): "
                      r"(\d+) alive, \+([\d.]+)/\+([\d.]+) per s, (\d+) kills")
TEMPLATE = re.compile(r"^faf: army (\d+) selected (\S+)")
ATTACK = re.compile(r"^\s*\[\s*([\d.]+)s\] army (\d+) ATTACKS with (\d+) of (\d+) (\w+)")
DECIDED = re.compile(r"^\s*\[\s*([\d.]+)s\] (?:team (\d+) WINS|a DRAW)")
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


# --- Paint --------------------------------------------------------------------------------
#
# The HUD's own palette, so the terminal reads like the game's readouts: mass is the
# green bar, energy the amber one, chrome the cyan frame. Factions wear Forged
# Alliance's team paints. Truecolor escapes; every terminal this runs on (Terminal.app,
# iTerm2, VS Code) has them, and a pipe gets none.
class Palette:
    MASS = (63, 214, 122)
    ENERGY = (240, 160, 48)
    CHROME = (90, 200, 215)
    ALERT = (224, 80, 60)
    GOLD = (240, 200, 90)
    DIM = (120, 130, 135)
    FACTION = {"uef": (96, 150, 235), "aeon": (96, 205, 150), "cybran": (225, 90, 90),
               "seraphim": (222, 185, 90)}

    def __init__(self, enabled):
        self.enabled = enabled

    def rgb(self, text, colour, bold=False):
        if not self.enabled:
            return text
        red, green, blue = colour
        prefix = "\x1b[1m" if bold else ""
        return f"{prefix}\x1b[38;2;{red};{green};{blue}m{text}\x1b[0m"

    def dim(self, text):
        return self.rgb(text, self.DIM)

    def bold(self, text):
        return f"\x1b[1m{text}\x1b[0m" if self.enabled else text

    def faction(self, name, text=None, bold=False):
        colour = self.FACTION.get(name.lower(), self.CHROME)
        return self.rgb(name if text is None else text, colour, bold)


def colour_wanted(choice, stream=None):
    stream = stream or sys.stdout
    if choice == "always":
        return True
    if choice == "never" or os.environ.get("NO_COLOR") or os.environ.get("TERM") == "dumb":
        return False
    return bool(getattr(stream, "isatty", lambda: False)())


def visible(text):
    return len(ANSI.sub("", text))


def pad(text, width, align="<"):
    gap = max(0, width - visible(text))
    if align == ">":
        return " " * gap + text
    if align == "^":
        left = gap // 2
        return " " * left + text + " " * (gap - left)
    return text + " " * gap


def clock(seconds):
    seconds = max(0, int(round(seconds)))
    hours, rest = divmod(seconds, 3600)
    minutes, secs = divmod(rest, 60)
    return f"{hours}:{minutes:02d}:{secs:02d}" if hours else f"{minutes}:{secs:02d}"


def wall_clock(seconds):
    return f"{seconds:.1f}s" if seconds < 60 else clock(seconds)


def number(value):
    return f"{value:,.0f}"


def console_width():
    columns = shutil.get_terminal_size((100, 24)).columns
    return max(60, min(columns, 110))


# --- ASCII art ----------------------------------------------------------------------------

BANNER = ("▄▀█ █   █▀▄▀█ ▄▀█ ▀█▀ █▀█ █ ▀▄▀",
          "█▀█ █   █ ▀ █ █▀█  █  █▀▄ █ █ █")


def banner(paint, subtitle, width):
    lines = [paint.rgb(row, Palette.CHROME, bold=True) for row in BANNER]
    lines.append(paint.dim(subtitle))
    lines.append(paint.dim("─" * width))
    return lines


def rule(paint, title, width):
    """A titled horizontal rule: `── title ───────`."""
    head = f"── {title} "
    return paint.rgb(head, Palette.CHROME) + paint.dim("─" * max(0, width - visible(head)))


def meter(filled_share, width, on="█", off="░"):
    filled = int(round(max(0.0, min(1.0, filled_share)) * width))
    return on * filled + off * (width - filled)


def front_line(paint, left, right, width, factions):
    """The signature: one bar, two armies, filled from each edge in faction paint, with a
    marker where the field's standing units balance. Left fills solid, right fills hatched,
    so a mirror match (same faction, same paint) still reads as two sides."""
    total = left + right
    share = 0.5 if total == 0 else left / total
    cells = width - 1
    left_cells = int(round(share * cells))
    bar = (paint.faction(factions[0], "█" * left_cells)
           + paint.bold("◆")
           + paint.faction(factions[1], "▓" * (cells - left_cells)))
    return bar, share


def sparkline(values):
    """Eight-level bars from a series; an empty series is an empty string."""
    blocks = "▁▂▃▄▅▆▇█"
    if not values:
        return ""
    top = max(values) or 1
    return "".join(blocks[min(7, int(value / top * 7.999))] for value in values)


# --- Live state of one run --------------------------------------------------------------------

class ArmyRow:
    def __init__(self, faction, personality):
        self.faction = faction
        self.personality = personality
        self.alive = 0
        self.mass = 0.0
        self.energy = 0.0
        self.kills = 0
        self.template = None


class RunState:
    """Everything the console shows for the run in progress, fed one binary line at a time."""

    def __init__(self, personalities, factions, cap_seconds):
        self.cap_seconds = cap_seconds
        self.rows = [ArmyRow(FACTION_TITLE.get(faction.lower(), faction), personality)
                     for faction, personality in zip(factions, personalities)]
        self.tick = 0
        self.wall = 0
        self.latest_attack = None
        self.decided = None
        self.reports = 0

    def feed(self, line):
        """Returns True when the line changed what the console should show."""
        if match := PROGRESS.match(line):
            tick, wall, army, faction, personality, alive, mass, energy, kills = match.groups()
            row = self.row(int(army), faction, personality)
            row.alive, row.mass, row.energy, row.kills = (int(alive), float(mass), float(energy),
                                                          int(kills))
            self.tick, self.wall = int(tick), int(wall)
            self.reports += 1
            return int(army) == len(self.rows) - 1
        if match := TEMPLATE.match(line):
            army, template = match.groups()
            if int(army) < len(self.rows):
                self.rows[int(army)].template = template
            return True
        if match := ATTACK.match(line):
            seconds, army, sent, of, kind = match.groups()
            self.latest_attack = (float(seconds), int(army), int(sent), int(of), kind)
            return False  # the ticker updates with the next redraw, not on every wave
        if match := DECIDED.match(line):
            seconds, winner = match.groups()
            self.decided = (float(seconds), None if winner is None else int(winner))
            return True
        return False

    def row(self, army, faction, personality):
        while len(self.rows) <= army:
            self.rows.append(ArmyRow(faction, personality))
        row = self.rows[army]
        row.faction, row.personality = faction, personality
        return row

    def render(self, paint, width):
        sim_seconds = self.tick / TICKS_PER_SECOND
        lines = []
        # Line 1: the sim clock against the cap, and the wall clock beside it.
        bar_width = max(10, width - 44)
        lines.append("  " + paint.dim("sim ") + paint.bold(pad(clock(sim_seconds), 7, ">"))
                     + " ▕" + paint.rgb(meter(sim_seconds / self.cap_seconds, bar_width),
                                        Palette.CHROME)
                     + "▏ " + paint.dim(clock(self.cap_seconds))
                     + paint.dim(f"   wall {wall_clock(self.wall)}"))
        # One row per army: faction and personality in faction paint, then the readouts in
        # the HUD's colours — green mass, amber energy.
        name_width = max(visible(self.label(paint, row)) for row in self.rows) if self.rows else 0
        for row in self.rows:
            lines.append("  " + pad(self.label(paint, row), name_width)
                         + "  " + paint.bold(pad(str(row.alive), 4, ">")) + paint.dim(" alive")
                         + "  " + paint.rgb(pad(f"+{row.mass:.1f}", 7, ">"), Palette.MASS)
                         + paint.dim(" m/s")
                         + "  " + paint.rgb(pad(f"+{row.energy:.0f}", 6, ">"), Palette.ENERGY)
                         + paint.dim(" e/s")
                         + "  " + paint.bold(pad(str(row.kills), 4, ">")) + paint.dim(" kills"))
        if len(self.rows) >= 2:
            bar, share = front_line(paint, self.rows[0].alive, self.rows[1].alive,
                                    max(20, width - 30), [row.faction for row in self.rows[:2]])
            lines.append("  " + bar + paint.dim(f"  front line {share:.0%} : {1 - share:.0%}"))
        if self.decided is not None:
            seconds, winner = self.decided
            verdict = ("a DRAW — every commander fell" if winner is None
                       else f"{self.rows[winner].personality.upper()} WINS (seat {winner})")
            lines.append("  " + paint.rgb("♛ " + verdict, Palette.GOLD, bold=True)
                         + paint.dim(f" at {clock(seconds)} sim"))
        elif self.latest_attack is not None:
            seconds, army, sent, of, kind = self.latest_attack
            who = self.rows[army].personality if army < len(self.rows) else f"army {army}"
            lines.append("  " + paint.dim(f"↳ {clock(seconds)}  {who} attacks with {sent} of {of} {kind}"))
        elif self.reports == 0:
            lines.append("  " + paint.dim("↳ waiting for the first progress report…"))
        return lines

    def label(self, paint, row):
        text = paint.faction(row.faction, f"{row.faction} {row.personality}", bold=True)
        if row.template:
            text += paint.dim(f" ({row.template})")
        return text


class Console:
    """Writes the live block; on a terminal it redraws the same lines in place."""

    def __init__(self, stream, live):
        self.stream = stream
        self.live = live
        self.shown = 0

    def block(self, lines):
        if self.live and self.shown:
            # Cursor to the first line of the previous block, clear to the end of screen.
            self.stream.write(f"\x1b[{self.shown}F\x1b[J")
        elif not self.live and self.shown:
            self.stream.write("\n")
        self.stream.write("\n".join(lines) + "\n")
        self.stream.flush()
        self.shown = len(lines)

    def line(self, text):
        self.stream.write(text + "\n")
        self.stream.flush()
        self.shown = 0


# --- Job plumbing (unchanged contract: dirs, JSON files, exit codes) ---------------------------

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


def run_title(personalities, factions):
    return f"{personalities[0]} ⚔ {personalities[1]} · {factions[0].upper()} vs {factions[1].upper()}"


def play(args, commit, run_dir, personalities, factions, console, paint, width, index, total):
    """Play one run in the foreground: the binary's whole output goes to stdout.log, the
    console gets the live block. Returns True when result.json was produced."""
    argv = command(args, commit, run_dir, personalities, factions)
    if args.dry_run:
        console.line(" ".join(argv))
        return True
    if not BINARY.is_file():
        raise RuntimeError("build/recoil-metal is missing; drop --no-build only after building")
    run_dir.mkdir(parents=True, exist_ok=False)
    (run_dir / "config.json").write_text(json.dumps({
        "personalities": list(personalities), "factions": list(factions),
        "map": args.map, "seconds": args.seconds, "commit": commit,
        "command": argv}, indent=2) + "\n")
    console.line("")
    console.line(rule(paint, f"run {index} of {total} · {run_title(personalities, factions)}", width))
    state = RunState(personalities, factions, args.seconds)
    console.block(state.render(paint, width))
    dirty = False

    def show():
        nonlocal dirty
        if dirty:
            console.block(state.render(paint, width))
            dirty = False

    timed_out = False
    with (run_dir / "stdout.log").open("w") as stdout, \
         (run_dir / "stderr.log").open("w") as stderr:
        process = subprocess.Popen(argv, cwd=ROOT, stdout=subprocess.PIPE,
                                   stderr=stderr, text=True, start_new_session=True)

        def drain():
            nonlocal dirty
            for line in process.stdout:
                stdout.write(line)
                if state.feed(line):
                    dirty = True
                    if console.live or state.reports or state.decided:
                        show()

        try:
            drain()
            code = process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            process.terminate()
            try:
                code = process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                code = process.wait()
            drain()
    show()
    if timed_out:
        # No result.json: SIGTERM kills the binary before it reports. The timeout
        # record keeps the config so the gap is visible, not silently missing.
        (run_dir / "timeout.json").write_text(json.dumps({
            "personalities": list(personalities), "factions": list(factions),
            "map": args.map, "seconds": args.seconds, "commit": commit,
            "timeout_seconds": args.timeout}, indent=2) + "\n")
        console.line("  " + paint.rgb(f"✗ TIMEOUT after {args.timeout}s wall — no result.json",
                                      Palette.ALERT, bold=True))
        return False
    if code != 0 or not (run_dir / "result.json").is_file():
        console.line("  " + paint.rgb(f"✗ FAILED (exit {code}) — no result.json",
                                      Palette.ALERT, bold=True))
        return False
    return True


# --- The report: one card per run, then the grid ---------------------------------------------

def outcome_of(doc):
    """('win', seat) | ('draw', None) | ('cap', None) from a result.json document."""
    if doc["finished"]:
        return ("win", doc["winner"]) if doc["winner"] is not None else ("draw", None)
    return ("cap", None)


def card(paint, doc, width):
    """A boxed scoreboard for one finished run: verdict, then the two armies side by side
    with the HUD-coloured readouts, then how the field swung over the run's snapshots."""
    armies = doc["armies"]
    kind, seat = outcome_of(doc)
    inner = width - 4
    out = []

    def box(text=""):
        out.append(paint.dim("│ ") + pad(text, inner) + paint.dim(" │"))

    title = " ⚔ ".join(paint.faction(army["faction"], f"{army['faction']} {army['personality']}",
                                      bold=True) for army in armies)
    head = paint.dim("┌ ") + title + " "
    out.append(head + paint.dim("─" * max(0, width - visible(head) - 1) + "┐"))
    sim = clock(doc["ticksPlayed"] / TICKS_PER_SECOND)
    wall = wall_clock(doc["wallSeconds"])
    if kind == "win":
        winner = armies[seat]
        verdict = paint.rgb(f"♛ {winner['personality'].upper()} wins", Palette.GOLD, bold=True)
        verdict += paint.dim(f" (seat {seat}, {winner['faction']})")
    elif kind == "draw":
        verdict = paint.rgb("⚖ DRAW — every commander fell", Palette.GOLD, bold=True)
    else:
        verdict = paint.rgb("⌛ sim cap reached, no winner", Palette.DIM, bold=True)
    box(verdict + paint.dim(f"   {sim} sim · {wall} wall"))
    box()
    label_width = 20
    column = max(14, (inner - label_width) // 2)
    header = pad("", label_width)
    for index, army in enumerate(armies):
        crown = " ♛" if kind == "win" and seat == index else ""
        header += pad(paint.faction(army["faction"], f"{army['faction']} {army['personality']}{crown}",
                                    bold=True), column, "^")
    box(header)

    def stat(label, key, colour=None, fmt=number):
        row = paint.dim(pad(label, label_width))
        for army in armies:
            text = fmt(army[key])
            row += pad(paint.rgb(text, colour) if colour else text, column, "^")
        box(row)

    stat("standing units", "alive")
    stat("mass generated", "generatedMass", Palette.MASS)
    stat("energy generated", "generatedEnergy", Palette.ENERGY)
    stat("kills", "kills")
    stat("kill worth (mass)", "killMassWorth", Palette.MASS)
    stat("kill worth (energy)", "killEnergyWorth", Palette.ENERGY)
    box()
    snapshots = doc.get("snapshots") or []
    if snapshots:
        row = paint.dim(pad("standing over time", label_width))
        for index, army in enumerate(armies):
            series = [snap["armies"][index].get("alive", 0) for snap in snapshots
                      if index < len(snap.get("armies", []))]
            row += pad(paint.faction(army["faction"], sparkline(series)), column, "^")
        box(row)
    else:
        box(paint.dim(pad("standing over time", label_width)
                      + "— no snapshots: the run ended before the first progress report"))
    out.append(paint.dim("└" + "─" * (width - 2) + "┘"))
    return out


def grid_cell(results, row, col):
    """The row personality's result against the column personality, from the pairs the
    matrix actually played: 'W', 'L', 'D', 'cap' or '✗' (no JSON). A mirror shows which
    seat won ('0>1' / '1>0'). '·' when the pair was not in this matrix."""
    for (first, second), doc in results:
        if {first, second} != {row, col} or (row == col) != (first == second):
            continue
        if doc is None:
            return "✗"
        kind, seat = outcome_of(doc)
        if kind == "cap":
            return "cap"
        if kind == "draw":
            return "D"
        if row == col:
            return "0>1" if seat == 0 else "1>0"
        winner = (first, second)[seat]
        return "W" if winner == row else "L"
    return "·"


def grid(paint, results, personalities, width):
    """Rows beat columns: every cell is the ROW personality's result against the column."""
    colours = {"W": (Palette.MASS, True), "L": (Palette.ALERT, True), "D": (Palette.GOLD, True),
               "cap": (Palette.DIM, False), "✗": (Palette.ALERT, False), "·": (Palette.DIM, False),
               "0>1": (Palette.CHROME, False), "1>0": (Palette.CHROME, False)}
    label = max(len(name) for name in personalities) + 2
    cell = max(6, max(len(name) for name in personalities) + 2)
    out = [rule(paint, "results grid · a cell is the row's result against the column", width)]
    out.append(pad("", label + 1)
               + " ".join(pad(paint.bold(name), cell, "^") for name in personalities))
    out.append(paint.dim(" " * label + "┌" + "┬".join("─" * cell for _ in personalities) + "┐"))
    for row in personalities:
        cells = []
        for col in personalities:
            value = grid_cell(results, row, col)
            colour, bold = colours[value]
            cells.append(pad(paint.rgb(value, colour, bold), cell, "^"))
        out.append(paint.bold(pad(row, label)) + paint.dim("│") + paint.dim("│").join(cells)
                   + paint.dim("│"))
    out.append(paint.dim(" " * label + "└" + "┴".join("─" * cell for _ in personalities) + "┘"))
    out.append(paint.dim("  W won · L lost · D draw · cap sim cap, no winner · ✗ no result · "
                         "0>1 mirror, seat 0 won"))
    return out


def load_results(job, pairs, factions):
    results = []
    for first, second in pairs:
        path = job / f"{first}-vs-{second}_{'-'.join(factions)}" / "result.json"
        results.append(((first, second), json.loads(path.read_text()) if path.is_file() else None))
    return results


def report(paint, console, job, pairs, factions, personalities, width):
    """Cards for every run with a result, the grid, and the tally line. Returns the failure count."""
    results = load_results(job, pairs, factions)
    console.line("")
    console.line(rule(paint, "results", width))
    failures = 0
    for (first, second), doc in results:
        console.line("")
        if doc is None:
            failures += 1
            console.line(paint.rgb(f"  ✗ {run_title((first, second), factions)} — no result.json",
                                   Palette.ALERT, bold=True))
            continue
        for line in card(paint, doc, width):
            console.line(line)
    console.line("")
    for line in grid(paint, results, personalities, width):
        console.line(line)
    console.line("")
    done = len(results) - failures
    tally = paint.rgb(f"  {done}/{len(results)} run(s) produced JSON", Palette.MASS if not failures
                      else Palette.ALERT, bold=True)
    console.line(tally + paint.dim(f"  →  {job}"))
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
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
    parser.add_argument("--interval", type=positive, default=5,
                        help="progress report cadence, in wall seconds (the sim runs ~100x "
                             "real time headless, so 5 s is roughly one report per 8 sim minutes)")
    parser.add_argument("--map", type=job_name, default="SCMP_009")
    parser.add_argument("--fa-root", type=Path,
                        default=Path("/Volumes/Samsung_T5/faf/Supreme Commander Forged Alliance"))
    parser.add_argument("--no-build", action="store_true", help="skip the rebuild")
    parser.add_argument("--dry-run", action="store_true", help="print commands, run nothing")
    parser.add_argument("--report", type=job_name, default=None, metavar="NAME",
                        help="re-render the cards and grid of a finished job, play nothing")
    parser.add_argument("--color", choices=("auto", "always", "never"), default="auto",
                        help="ANSI colour (auto: only on a terminal, never under NO_COLOR)")
    args = parser.parse_args()

    paint = Palette(colour_wanted(args.color))
    console = Console(sys.stdout, live=paint.enabled and sys.stdout.isatty())
    width = console_width()
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    job = JOBS / (args.report or args.name or f"matrix-{stamp}")
    try:
        pairs = run_pairs(args.personalities)
        if args.report:
            if not job.is_dir():
                raise RuntimeError(f"no job at {job}")
            for line in banner(paint, f"report · {job.name} · {len(pairs)} run(s)", width):
                console.line(line)
            return 1 if report(paint, console, job, pairs, args.factions, args.personalities,
                               width) else 0
        commit = commit_hash()
        if not args.no_build and not args.dry_run:
            build_binary()
        subtitle = (f"{len(pairs)} run(s) · {', '.join(args.personalities)} · "
                    f"{args.factions[0].upper()} vs {args.factions[1].upper()} · {args.map} · "
                    f"cap {clock(args.seconds)} sim · commit {commit[:12]}")
        for line in banner(paint, subtitle, width):
            console.line(line)
        console.line(paint.dim(f"  → {job}"))
        results = []
        for index, (first, second) in enumerate(pairs, start=1):
            run_dir = job / f"{first}-vs-{second}_{'-'.join(args.factions)}"
            ok = play(args, commit, run_dir, (first, second), args.factions, console, paint, width,
                      index, len(pairs))
            results.append((run_dir, ok))
        if args.dry_run:
            console.line(f"matrix: dry run, {len(results)} command(s) printed, nothing executed")
            return 0
        return 1 if report(paint, console, job, pairs, args.factions, args.personalities,
                           width) else 0
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        print(f"ai-matrix: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
