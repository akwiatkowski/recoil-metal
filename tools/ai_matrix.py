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
a window — never drop it. That capture is the run's last frame, written at 4K
by default (see --shot-size).

`--action-shots N` then adds up to N pictures of the fighting. The sim is
deterministic, so each one is a second run of the same match stopped at a tick
where a lot of units died, with the camera pointed at where they died — no
mid-match capture hook, at the cost of replaying the match once per picture.

`--report <name>` re-renders the cards and grid of a finished job without
playing anything.
"""
import argparse
import collections
import datetime
import math
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
# How an action frame is chosen.
#
# The window is what counts as one fight: fifteen sim seconds either side, because a
# battle is a half-minute of trading, not a tick where four things happened at once, and
# scoring the whole engagement is what puts the camera on the engagement. The cooldown is
# what counts as a DIFFERENT fight, so five frames are five battles rather than five
# views of the best one. The floor is what counts as a fight at all. The lag looks a
# beat past the busiest tick, where the wrecks are down and the survivors are still
# shooting — the frame wants both, so it sits in the middle of the dying rather than
# after it.
ACTION_WINDOW_TICKS = 150
ACTION_COOLDOWN_MIN_TICKS = 300
ACTION_MIN_DEATHS = 6
ACTION_LAG_TICKS = 4
# What the camera holds, in elmos — and this window is the whole reason an action frame
# looks like a battle rather than a map. Units draw as strategic glyphs once a model
# falls under `kIconThresholdPoints` (8 points, core/scene/UnitIcons.hpp), so a wide view
# is a picture of icons. A tank is about 4 elmos of radius, and the view is 2160 points
# tall, which keeps meshes readable out to roughly 540 elmos of radius. Below 350 the
# frame is grass and trees with a fight somewhere off the edge.
ACTION_RADIUS_MIN = 350.0
ACTION_RADIUS_MAX = 540.0

# Rows in each roster table. Ten covers what a player would name — the factories, the
# artillery, the mass farm — and a full Forged Alliance roster runs to thirty types, most
# of them a handful of engineers and walls.
ROSTER_ROWS = 10

# --- What the binary prints that the console keeps ----------------------------------------
#
# Anchored on the exact formats in `Match.cpp` (`printMatrixProgress`, the FAF seating
# report and the tick narration). A format change there shows up here as a silent
# console — which is why `test_ai_matrix.py` pins these against the literal lines.
PROGRESS = re.compile(r"^matrix: \[tick (\d+), (\d+)s wall\] army (\d+) \((\w+)/(\w+)\): "
                      r"(\d+) alive, \+([\d.]+)/\+([\d.]+) per s, (\d+) kills")
TEMPLATE = re.compile(r"^faf: army (\d+) selected (\S+)")
ATTACK = re.compile(r"^\s*\[\s*([\d.]+)s\] army (\d+) ATTACKS with (\d+) of (\d+) (\w+)")
# The roster lines that accompany each progress report, one per standing type. The name is
# last because it is the only field that can hold spaces — or be empty, for content that
# states no description.
STANDING = re.compile(r"^matrix: standing \[tick (\d+)\] army (\d+) tech (\d+) count (\d+) "
                      r"mass ([\d.]+) energy ([\d.]+) mobile ([01]) commander ([01]) "
                      r"bp (\S+) name (.*)$")
# Forged Alliance names a blueprint <faction><domain><role>: UEB2301 and URB2301 are the
# UEF and Cybran versions of one turret. The role — domain letter plus number — is the only
# thing the four factions agree on, since half of them rename the unit and a third of them
# price it differently, so it is what a cross-faction row is keyed on.
BLUEPRINT_ID = re.compile(r"^[A-Z]{3}\d{4}$")
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


def type_name(row):
    """What to call a type: its own description, else the blueprint's stem — a path is a
    worse name than "Mass Extractor", but it beats a blank row."""
    described = (row.get("description") or "").strip()
    return described or blueprint_id(row) or "unknown"


def blueprint_id(row):
    """`/units/UEB2301/UEB2301_unit.bp` → `UEB2301`."""
    stem = row.get("blueprint", "").rsplit("/", 1)[-1]
    if stem.endswith("_unit.bp"):
        stem = stem[: -len("_unit.bp")]
    return stem.upper()


def roster_key(row):
    """What makes two sides' versions of one thing the same row.

    The blueprint's ROLE — `B2301` out of `UEB2301` — so a turret is one row whichever
    factions are playing. Content whose ids are not shaped that way (Beyond All Reason's,
    or a test's) keys on its own id, which merges nothing and duplicates nothing."""
    ident = blueprint_id(row)
    return ident[2:] if BLUEPRINT_ID.fullmatch(ident) else ident


def price(value):
    """A cost, short enough to sit in a column: thousands stay readable, millions become
    `5,000k` rather than eating the row."""
    return f"{value / 1000:,.0f}k" if value >= 100_000 else f"{value:,.0f}"


def price_span(low, high):
    """One price, or the range the factions charge for the same role — 480–540 for a T2
    turret. Never an average: nobody pays that."""
    return price(high) if low == high else f"{price(low)}–{price(high)}"


def roster_rows(per_army):
    """Merge every side's standing types into rows, dearest per unit first.

    `per_army` is one list of type rows per army — the shape both `result.json` and the
    progress lines carry. Commanders are left out: there is exactly one, it outprices
    everything, and holding it is not a decision the AI made. Price is per unit, since
    "expensive" in this game is what ONE of them costs, and the sort takes the dearest
    variant when the factions disagree. A row's name is the one most of its sides use."""
    merged = {}
    for seat, rows in enumerate(per_army):
        for row in rows:
            if row.get("commander"):
                continue
            key = roster_key(row)
            entry = merged.get(key)
            if entry is None:
                entry = merged[key] = {
                    "tech": row["tech"], "mobile": bool(row.get("mobile")),
                    "names": [], "mass": (row["mass"], row["mass"]),
                    "energy": (row["energy"], row["energy"]),
                    "counts": [0] * len(per_army)}
            entry["names"].append(type_name(row))
            entry["tech"] = max(entry["tech"], row["tech"])
            entry["mobile"] = entry["mobile"] or bool(row.get("mobile"))
            for field in ("mass", "energy"):
                low, high = entry[field]
                entry[field] = (min(low, row[field]), max(high, row[field]))
            entry["counts"][seat] += row["count"]
    for entry in merged.values():
        entry["name"] = collections.Counter(entry["names"]).most_common(1)[0][0]
    return sorted(merged.values(),
                  key=lambda e: (-e["mass"][1], -e["energy"][1], e["name"]))


def seat_labels(armies):
    """A column head per side: the faction, plus the seat when both sides share one, so a
    mirror match still tells its columns apart."""
    names = [FACTION_TITLE.get(str(name).lower(), str(name)) for name in armies]
    if len(set(names)) == len(names):
        return names
    return [f"{name} {seat}" for seat, name in enumerate(names)]


def roster_table(paint, rows, armies, width, title, limit=ROSTER_ROWS):
    """One roster list as a table: dearest on top, a count column per side.

    Zero reads as a dim dot rather than a 0 — an army that never built a type and one
    whose last of them just died are different facts, and the eye should not have to
    subtract to see which side owns the expensive things."""
    labels = seat_labels(armies)
    count_width = max(7, max((len(label) for label in labels), default=7) + 1)
    mass_width, energy_width, tech_width = 13, 13, 3
    # The names are the widest thing here and still only reach thirty characters, so the
    # column is capped rather than stretched: numbers next to their labels beat numbers
    # pushed to the far edge of a box.
    name_width = max(12, min(30, width - tech_width - mass_width - energy_width
                             - count_width * len(labels) - 2))
    shown = rows[:limit]
    out = [paint.rgb(pad(title, tech_width + name_width), Palette.CHROME, bold=True)
           + paint.rgb(pad("mass", mass_width, ">"), Palette.MASS)
           + paint.rgb(pad("energy", energy_width, ">"), Palette.ENERGY)
           + "".join(paint.faction(armies[seat], pad(label, count_width, ">"), bold=True)
                     for seat, label in enumerate(labels))]
    for row in shown:
        # Tech 0 is a prop or a wall section: no tier to print, and the padding keeps
        # every name in one column anyway.
        tier = f"T{row['tech']}" if row["tech"] > 0 else ""
        name = row["name"]
        if len(name) > name_width - 1:
            name = name[: name_width - 2] + "…"
        line = (paint.dim(pad(tier, tech_width)) + pad(name, name_width)
                + paint.rgb(pad(price_span(*row["mass"]), mass_width, ">"), Palette.MASS)
                + paint.rgb(pad(price_span(*row["energy"]), energy_width, ">"),
                            Palette.ENERGY))
        for seat, count in enumerate(row["counts"]):
            cell = pad(f"x{count}" if count else "·", count_width, ">")
            line += (paint.faction(armies[seat], cell, bold=True) if count
                     else paint.dim(cell))
        out.append(line)
    if not shown:
        out.append(paint.dim("  nothing standing"))
    elif len(rows) > len(shown):
        out.append(paint.dim(f"  … and {len(rows) - len(shown)} cheaper"))
    return out


def roster_sections(paint, per_army, armies, width):
    """Both tables: what walks, and what was built into the ground. Two lists because
    they answer different questions — the army, and the investment."""
    rows = roster_rows(per_army)
    out = roster_table(paint, [row for row in rows if row["mobile"]], armies, width, "units")
    out.append("")
    out.extend(roster_table(paint, [row for row in rows if not row["mobile"]], armies, width,
                            "buildings"))
    return out


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
        # army -> {"tick", "rows"}: the roster lines of the report being read. Keyed by
        # tick rather than by arrival order, so a report is never a mix of two.
        self.roster = {}

    def feed(self, line):
        """Returns True when the line changed what the console should show."""
        if match := STANDING.match(line):
            (tick, army, tech, count, mass, energy, mobile, commander,
             blueprint, name) = match.groups()
            self.roster_for(int(army), int(tick)).append({
                "blueprint": blueprint, "description": name, "tech": int(tech),
                "count": int(count), "mass": float(mass), "energy": float(energy),
                "mobile": mobile == "1", "commander": commander == "1"})
            return False  # the army's own progress line closes the report
        if match := PROGRESS.match(line):
            tick, wall, army, faction, personality, alive, mass, energy, kills = match.groups()
            row = self.row(int(army), faction, personality)
            row.alive, row.mass, row.energy, row.kills = (int(alive), float(mass), float(energy),
                                                          int(kills))
            self.tick, self.wall = int(tick), int(wall)
            # A wiped army sends no roster lines at all, so its list is emptied here
            # rather than left showing units that died two reports ago.
            self.roster_for(int(army), int(tick))
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

    def roster_for(self, army, tick):
        """This army's roster list for `tick`, emptied when the report is a new one."""
        held = self.roster.get(army)
        if held is None or held["tick"] != tick:
            held = {"tick": tick, "rows": []}
            self.roster[army] = held
        return held["rows"]

    def standing(self):
        """One roster list per seat, in seat order."""
        return [self.roster.get(seat, {}).get("rows", []) for seat in range(len(self.rows))]

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
        if self.reports:
            lines.append("")
            lines.extend(("  " + line) if line else "" for line in
                         roster_sections(paint, self.standing(),
                                         [row.faction for row in self.rows], width - 2))
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
    width, height = args.shot_size
    return [str(BINARY), str(args.fa_root / "maps" / args.map / f"{args.map}.scmap"),
            "--gamedata", str(args.fa_root / "gamedata"), "--skirmish", "--observer",
            "--armies", "2", "--factions", ",".join(factions),
            "--ai-personalities", ",".join(personalities),
            "--mute", "--play", str(args.seconds),
            "--matrix-out", str(run_dir / "result.json"),
            "--matrix-commit", commit,
            "--report-interval", str(args.interval),
            "--screenshot", str(run_dir / "final.png"), str(width), str(height),
            "--backing", str(args.shot_backing)]


def action_command(args, run_dir, personalities, factions, moment, index):
    """The same match, replayed to one moment, framed on it, captured once.

    Deliberately NOT carrying --matrix-out or --report-interval: this run must not
    overwrite the result of the one that measured the match, and its progress is the
    driver's line, not fifty of the binary's."""
    # Same pixels as the final frame, but asked for as points at backing 1: a unit's
    # apparent size is measured in POINTS, so this is what keeps tanks as models rather
    # than glyphs across a battle-wide view. The interface lays out smaller as a result,
    # which suits a picture of a fight.
    width, height = shot_pixels(args)
    return [str(BINARY), str(args.fa_root / "maps" / args.map / f"{args.map}.scmap"),
            "--gamedata", str(args.fa_root / "gamedata"), "--skirmish", "--observer",
            "--armies", "2", "--factions", ",".join(factions),
            "--ai-personalities", ",".join(personalities),
            "--mute", "--play", f"{moment['seconds']:.1f}",
            "--look", f"{moment['x']:.0f}", f"{moment['z']:.0f}",
            f"{moment['radius']:.0f}",
            "--screenshot", str(run_dir / f"action-{index}.png"), str(width), str(height),
            "--backing", "1"]


def action_cooldown(played, limit):
    """How far apart two frames must be to be two battles.

    Scaled to the match, because a fixed spacing does the wrong thing at both ends: three
    sim minutes leaves a ten-minute rush with room for two frames, and gives an hour-long
    turtle game five pictures of its first skirmish. Half the match divided by the frame
    count spreads them across the fighting, with a thirty-second floor so no two frames
    are the same brawl."""
    return max(ACTION_COOLDOWN_MIN_TICKS, int(played) // max(1, 2 * limit))


def action_moments(deaths, limit, played=0, window=ACTION_WINDOW_TICKS, cooldown=None,
                   floor=ACTION_MIN_DEATHS):
    """The `limit` busiest moments of dying, far enough apart to be different battles.

    A moment scores as every death within `window` ticks of it, so a score is "how much
    died at once" rather than "on this exact tick". Picking is greedy: take the highest,
    blank out `cooldown` ticks either side of it so the next pick is another fight rather
    than the same one two seconds later, repeat. Moments below `floor` deaths are not
    battles — a lone scout being shot down is not a picture worth four megabytes."""
    if cooldown is None:
        cooldown = action_cooldown(played or (deaths[-1]["tick"] if deaths else 0), limit)
    rows = sorted(deaths, key=lambda row: row["tick"])
    scored = []
    for centre in rows:
        near = [row for row in rows if abs(row["tick"] - centre["tick"]) <= window]
        toll = sum(row["deaths"] for row in near)
        if toll < floor:
            continue
        # Where to point the camera: the victims' centre of mass over the window, and a
        # radius that holds the whole brawl with room to see it.
        weight = float(toll)
        x = sum(row["x"] * row["deaths"] for row in near) / weight
        z = sum(row["z"] * row["deaths"] for row in near) / weight
        reach = max((math.dist((x, z), (row["x"], row["z"])) + row["spread"]
                     for row in near), default=0.0)
        scored.append({"tick": centre["tick"], "deaths": toll, "x": x, "z": z,
                       "radius": min(ACTION_RADIUS_MAX,
                                     max(ACTION_RADIUS_MIN, reach * 1.5))})
    scored.sort(key=lambda moment: (-moment["deaths"], moment["tick"]))
    taken = []
    for moment in scored:
        if all(abs(moment["tick"] - chosen["tick"]) > cooldown for chosen in taken):
            taken.append(moment)
        if len(taken) == limit:
            break
    for moment in taken:
        # A few ticks past the peak: the wrecks are down and the explosions are still up.
        moment["seconds"] = (moment["tick"] + ACTION_LAG_TICKS) / TICKS_PER_SECOND
    return sorted(taken, key=lambda moment: moment["tick"])


def capture_action(args, run_dir, personalities, factions, console, paint):
    """Replay the run once per chosen moment and write the pictures beside its result."""
    if args.action_shots <= 0:
        return []
    result_path = run_dir / "result.json"
    if not result_path.is_file():
        return []
    doc = json.loads(result_path.read_text())
    moments = action_moments(doc.get("deaths") or [], args.action_shots,
                             played=doc.get("ticksPlayed", 0))
    if not moments:
        console.line(paint.dim("  no battle big enough to photograph"))
        return []
    width, height = shot_pixels(args)
    console.line(paint.dim(f"  {len(moments)} action frame(s) at {width}x{height}, one match "
                           f"replay each"))
    taken = []
    for index, moment in enumerate(moments, start=1):
        argv = action_command(args, run_dir, personalities, factions, moment, index)
        with (run_dir / f"action-{index}.log").open("w") as log:
            code = subprocess.run(argv, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT).returncode
        image = run_dir / f"action-{index}.png"
        if code != 0 or not image.is_file():
            console.line(paint.rgb(f"  ✗ action {index} failed (exit {code})", Palette.ALERT))
            continue
        moment["file"] = image.name
        taken.append(moment)
        console.line("  " + paint.rgb("✦", Palette.GOLD, bold=True)
                     + paint.dim(f" action {index} · {clock(moment['seconds'])} sim · ")
                     + paint.bold(f"{moment['deaths']} deaths")
                     + paint.dim(f" · {image.name}"))
    (run_dir / "action.json").write_text(json.dumps(taken, indent=2) + "\n")
    return taken


def shot_pixels(args):
    """The image the capture will actually write, in pixels."""
    return (round(args.shot_size[0] * args.shot_backing),
            round(args.shot_size[1] * args.shot_backing))


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
    capture_action(args, run_dir, personalities, factions, console, paint)
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
    box()
    box(paint.dim("standing at the end · dearest per unit · the commander left out"))
    for line in roster_sections(paint, [army.get("standing") or [] for army in armies],
                                [army["faction"] for army in armies], inner):
        box(line)
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
    # The last frame is the only picture a finished run leaves, and 320x180 was a
    # thumbnail. Size follows the binary's own contract: LOGICAL points plus a backing
    # scale, so the interface lays out for a 1920x1080 screen and rasterises at 2x —
    # 4K pixels that look like a screen rather than a screen with ant-sized chrome.
    # 1920x1080 at 2 is 4K; 1280x720 at 2 is 1440p; 1350x760 at 2 is 2.7K.
    parser.add_argument("--shot-size", type=positive, nargs=2, default=[1920, 1080],
                        metavar=("W", "H"), help="capture size in logical points")
    parser.add_argument("--shot-backing", type=float, default=2.0, metavar="S",
                        help="display scale the capture stands in for, 1 to 4")
    parser.add_argument("--action-shots", type=int, default=5, metavar="N",
                        help="pictures of the fighting, each one a replay of the match "
                             "stopped where units were dying (0 for none)")
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
        shot_width, shot_height = shot_pixels(args)
        subtitle = (f"{len(pairs)} run(s) · {', '.join(args.personalities)} · "
                    f"{args.factions[0].upper()} vs {args.factions[1].upper()} · {args.map} · "
                    f"cap {clock(args.seconds)} sim · frame {shot_width}x{shot_height} · "
                    f"{args.action_shots} action frame(s) · commit {commit[:12]}")
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
