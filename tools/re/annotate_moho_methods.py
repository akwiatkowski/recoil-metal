#!/usr/bin/env python3
"""Give every recovered Moho callable a subsystem, a purpose line, and a canonical name.

WHY. `extract_moho_methods.py` produces 1,182 rows of `scope`, `name`, `signature`, address.
That is a lookup table, not a map: `Unit::SetConsumptionActive` and `Unit::GetSkirtRect` sit
next to each other alphabetically and have nothing to do with one another, and roughly a third
of the rows ship no documentation string at all. Answering "what in this API touches the
economy" meant reading all of it.

This adds three columns:

  subsystem   which part of the engine the callable belongs to, from scope + name evidence
  purpose     one line of prose: the engine's own documentation where it ships one, otherwise
              a description derived from the verb and the noun in the name
  canonical   `fa_<subsystem>_<verb><Noun>`, the naming convention this campaign's symbol
              ledger uses, so a name recovered here can be pasted straight into it

TWO RULES THIS FOLLOWS, both about not inventing knowledge:

  1. **A shipped doc string always wins.** Where the engine documents a method, that text is
     the purpose verbatim. Generated prose is only ever a fallback, and the `purpose_source`
     column says which it was, so a reader can tell recovered fact from inference at a glance.
  2. **Classification is evidence-ranked, not keyword soup.** Scope decides first (everything
     on `CAiBrain` is AI), and only then does the name get a vote. A name-keyword match that
     contradicts a confident scope is discarded rather than blended.

Read-only; consumes the extractor's TSV and writes another.
"""

import argparse
import re
import sys
from collections import Counter

# --- subsystem classification -------------------------------------------------------------
#
# Scope-level rules come first because they are the strongest evidence available: a method's
# owning class is a fact recovered from the binary, whereas a keyword in its name is a guess
# about English. Only scopes whose whole purpose is unambiguous appear here.
SCOPE_SUBSYSTEM = {
    "CAiBrain": "ai",
    "CAiPersonality": "ai",
    "CPlatoon": "ai",
    "CAiAttackerImpl": "ai",
    "CAiNavigatorImpl": "movement",
    "CPathDebugger": "movement",
    "UnitWeapon": "combat",
    "Projectile": "combat",
    "CollisionBeamEntity": "combat",
    "CDamage": "combat",
    "ReconBlip": "intel",
    "CLobby": "session",
    "CDiscoveryService": "session",
    "CSteamDiscoveryService": "session",
    "HSound": "audio",
    "IEffect": "fx",
    "ScriptedDecal": "fx",
    "CDecalHandle": "fx",
    "EntityCategory": "blueprint",
    "CPrefetchSet": "resources",
    "CUnitScriptTask": "orders",
}

# Any scope beginning with one of these is UI or animation wholesale.
SCOPE_PREFIX_SUBSYSTEM = [
    ("CMaui", "ui"),
    ("CUIWorld", "ui"),
    ("CUIMapPreview", "ui"),
    ("CLuaWldUIProvider", "ui"),
    ("UserUnit", "ui"),
    ("CameraImpl", "ui"),
    ("CAnimationManipulator", "animation"),
    ("CAimManipulator", "animation"),
    ("CRotateManipulator", "animation"),
    ("CSlideManipulator", "animation"),
    ("CSlaveManipulator", "animation"),
    ("CThrustManipulator", "animation"),
    ("CStorageManipulator", "animation"),
    ("CBuilderArmManipulator", "animation"),
    ("CBoneEntityManipulator", "animation"),
    ("CCollisionManipulator", "animation"),
    ("CFootPlantManipulator", "animation"),
    ("IAniManipulator", "animation"),
    ("MotorFallDown", "animation"),
]

# Name-level rules, consulted only when the scope did not decide. Ordered most specific
# first: `Veteran` must be tested before `Vet`-free generic health words, and `Shield` before
# the broader `Health`.
NAME_SUBSYSTEM = [
    (r"Veteran", "veterancy"),
    (r"Shield", "shields"),
    (r"Silo|Nuke|Tactical.*Ammo|Ammo", "ordnance"),
    (r"Consumption|Production|Econom|Resource|Mass|Energy|Storage|Upkeep", "economy"),
    (r"Reclaim|Capture|Repair|Build|Construct|Upgrade|Rally|Factory|WorkProgress", "build"),
    (r"Weapon|Damage|Armor|Attack|Target|Fire|Missile|Impact|Kill|Overcharge", "combat"),
    (r"Intel|Radar|Sonar|Omni|Vision|Cloak|Stealth|Jam|Blip|Recon|Scan", "intel"),
    (r"Move|Nav|Path|Goal|Speed|Accel|Turn|Steer|Heading|Waypoint|Formation|Elevation",
     "movement"),
    (r"Transport|Cargo|Attach|Detach|Ferry|Dock|Teleport", "transport"),
    (r"Sound|Cue|Music|Volume|Audio", "audio"),
    (r"Emitter|Effect|Trail|Beam|Decal|Particle|Mesh|Anim|Bone|Texture|Scroll", "fx"),
    (r"Command|Order|Queue|Task|Patrol|Guard", "orders"),
    (r"Blueprint|Category|Stat\b|Prop\b", "blueprint"),
    (r"Army|Alliance|Focus|Player|Team|Score|Victory", "session"),
    (r"Control|Frame|Text|Button|Cursor|Font|Layout|Render|Draw|Colou?r|Alpha|Hidden|Window",
     "ui"),
    (r"Thread|Fork|Wait|Tick|Beat|Time|Clock|Random|Sync|Checksum", "runtime"),
    (r"Health|Regen|Destroy|Create|Entity|Id\b|Alive|Dead", "lifecycle"),
    (r"Position|Transform|Rotation|Scale|Vector|Rect|Bounds|Footprint|Terrain|Height|Water",
     "world"),
    # Host-side services rather than simulation. These sit last because a gameplay word in
    # the same name should win — `SaveState` is persistence, but `GetUnitSaveFile` is not a
    # unit method that happens to mention saving.
    (r"^Disk|File|Path\b|Dirname|Basename|Directory|Folder", "filesystem"),
    (r"Con[EGS]|Console|Debug|Cheat|Profil|Trace|Assert|Breakpoint|Warn", "debug"),
    (r"^Log|Logging", "debug"),
    (r"Select", "selection"),
    (r"Replay|Save|Load|Serial", "persistence"),
    (r"Steam|Achievement|Locale|Language|Preference|Option|Setting", "platform"),
    (r"Net|Lobby|Chat|Ping|Connect", "session"),
]

# --- purpose generation -------------------------------------------------------------------
#
# Verb prefixes, longest first so `GetOrCreate` is not read as `Get`. The phrasing is
# deliberately flat and declarative; this is an index, not documentation.
VERBS = [
    ("GetOrCreate", "gets or creates"),
    ("CanConsider", "tests whether it may consider"),
    ("Recalculate", "recalculates"),
    ("Calculate", "calculates"),
    ("Initialize", "initialises"),
    ("Disconnect", "disconnects"),
    ("Establish", "establishes"),
    ("Abandon", "abandons"),
    ("Acquire", "acquires"),
    ("Restore", "restores"),
    ("Request", "requests"),
    ("Suspend", "suspends"),
    ("Disable", "disables"),
    ("Destroy", "destroys"),
    ("Process", "processes"),
    ("Toggle", "toggles"),
    ("Update", "updates"),
    ("Remove", "removes"),
    ("Create", "creates"),
    ("Enable", "enables"),
    ("Revert", "reverts"),
    ("Attach", "attaches"),
    ("Detach", "detaches"),
    ("Change", "changes"),
    ("Assign", "assigns"),
    ("Should", "tests whether it should"),
    ("Clear", "clears"),
    ("Reset", "resets"),
    ("Check", "checks"),
    ("Alter", "changes"),
    ("Issue", "issues"),
    ("Apply", "applies"),
    ("Force", "forces"),
    ("Start", "starts"),
    ("Build", "builds"),
    ("Count", "counts"),
    ("Play", "plays"),
    ("Stop", "stops"),
    ("Kill", "kills"),
    ("Show", "shows"),
    ("Hide", "hides"),
    ("Find", "finds"),
    ("Pick", "picks"),
    ("Fork", "forks"),
    ("Wait", "waits for"),
    ("Warp", "teleports"),
    ("Add", "adds"),
    ("Set", "sets"),
    ("Get", "reads"),
    ("Can", "tests whether it can"),
    ("Has", "tests whether it has"),
    ("Is", "tests whether it is"),
    ("Do", "performs"),
    ("On", "handles the event"),
]


def split_words(identifier: str) -> str:
    """`GetMaxHealth` -> `max health`. Keeps runs of capitals together (`GetAIBrain`)."""
    spaced = re.sub(r"(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])", " ", identifier)
    return spaced.replace("_", " ").strip().lower()


def classify(scope: str, name: str) -> str:
    if scope in SCOPE_SUBSYSTEM:
        return SCOPE_SUBSYSTEM[scope]
    for prefix, subsystem in SCOPE_PREFIX_SUBSYSTEM:
        if scope.startswith(prefix):
            return subsystem
    for pattern, subsystem in NAME_SUBSYSTEM:
        if re.search(pattern, name):
            return subsystem
    if scope in ("Unit", "Entity", "Prop"):
        return "lifecycle"
    return "misc"


def describe(scope: str, name: str, doc: str, signature: str = ""):
    """(purpose, source). The engine's own words when it has any.

    Two shapes count as shipped documentation. The obvious one is the text after `" - "` in a
    signature. The second is a signature that is prose rather than a parameter list at all —
    `Unit::GetConsumptionPerSecondEnergy` ships "Get the consumption of energy of the unit"
    and no parentheses, and treating that as a signature rather than as documentation threw
    away 270 real descriptions.
    """
    if doc:
        return doc, "engine"
    if signature and "(" not in signature and signature != name:
        return signature, "engine"
    for prefix, phrase in VERBS:
        if name.startswith(prefix) and len(name) > len(prefix):
            rest = split_words(name[len(prefix):])
            subject = "" if scope == "<global>" else f" of the {split_words(scope)}"
            return f"{phrase} the {rest}{subject}", "derived"
    return f"{split_words(name)}", "derived"


def canonical(subsystem: str, scope: str, name: str) -> str:
    """`fa_<subsystem>_<Scope><Name>`, matching the symbol ledger's convention."""
    owner = "" if scope == "<global>" else re.sub(r"^(C|I)(?=[A-Z])", "", scope)
    return f"fa_{subsystem}_{owner}{name}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tsv", default="build/re-fa/exports/moho.methods.tsv")
    parser.add_argument("--out", default="build/re-fa/exports/moho.methods.annotated.tsv")
    args = parser.parse_args()

    with open(args.tsv) as handle:
        header = handle.readline().rstrip("\n").split("\t")
        rows = [l.rstrip("\n").split("\t") for l in handle]

    col = {n: i for i, n in enumerate(header)}
    subsystems, sources = Counter(), Counter()

    with open(args.out, "w") as out:
        print("\t".join(["subsystem", "canonical", "purpose", "purpose_source"] + header),
              file=out)
        for row in rows:
            scope, name = row[col["scope"]], row[col["name"]]
            doc = row[col["doc"]] if col["doc"] < len(row) else ""
            subsystem = classify(scope, name)
            purpose, source = describe(scope, name, doc, row[col["signature"]])
            subsystems[subsystem] += 1
            sources[source] += 1
            print("\t".join([subsystem, canonical(subsystem, scope, name), purpose, source]
                            + row), file=out)

    print(f"# annotated {len(rows)} callables -> {args.out}", file=sys.stderr)
    print(f"# purpose from engine docs: {sources['engine']}, derived: {sources['derived']}",
          file=sys.stderr)
    for subsystem, count in subsystems.most_common():
        print(f"#   {subsystem:12s} {count:4d}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
