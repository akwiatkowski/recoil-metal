#!/usr/bin/env python3
"""Generate the FAF binding table from the vendored AI corpus (ADR-039).

WHY THIS IS GENERATED. The adapter must bind every engine name FAF's AI calls — 222 of them —
and a hand-written list is wrong the day the pin in tools/fetch_ai.sh moves. Worse, it is wrong
*silently*: a missing binding is a Lua error forty minutes into a match, not a build failure.
So the list is derived from the corpus itself, and `--check` fails if the committed table has
drifted from what the corpus actually calls.

WHY THE OUTPUT IS COMMITTED even though vendor/ai/ is not. A checkout that has not run `make ai`
still has to build. The generated file is a list of NAMES, which is ours; the corpus it was
derived from is FAF's and stays out of history.

HOW THE SURFACE IS DECIDED. Two sources, intersected:

  * vendor/ai/faf/engine/  — LuaLS annotation stubs, 12k lines reconstructing the closed Moho
    API. This is what the engine is documented to provide.
  * the AI corpus itself   — what is actually called. Binding the whole documented API would be
    511 globals and 688 methods, most of which no AI touches.

A name in the corpus but not the annotations is reported separately: those are the 33 that leak
into game Lua outside the AI subset, and they need shims rather than bindings.

    tools/gen_ai_bindings.py            regenerate src/app/FafApi.inc
    tools/gen_ai_bindings.py --check    fail if it would differ (for CI / a test)
"""

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
FAF = ROOT / "vendor" / "ai" / "faf"
OUT = ROOT / "src" / "app" / "FafApi.inc"

# The AI corpus: what an adapter has to keep running. Mirrors FAF_PATHS in tools/fetch_ai.sh
# minus engine/, which is the specification rather than the subject.
CORPUS = [
    "lua/AI",
    "lua/aibrains",
    "lua/aibrain.lua",
    "lua/aibrainPlans.lua",
    "lua/aipersonality.lua",
    "lua/platoon.lua",
    "lua/sim/Builder.lua",
    "lua/sim/BuilderManager.lua",
    "lua/sim/EngineerManager.lua",
    "lua/sim/FactoryBuilderManager.lua",
    "lua/sim/PlatoonFormManager.lua",
    "lua/sim/BrainConditionsMonitor.lua",
]

# Names Lua itself provides, which must not be bound over: doing so would replace the real
# implementation with a counted stub and break the corpus in ways that look like our bug.
STDLIB = {
    "assert", "error", "pcall", "xpcall", "print", "type", "tostring", "tonumber",
    "pairs", "ipairs", "next", "select", "rawget", "rawset", "rawequal", "unpack",
    "setmetatable", "getmetatable", "require", "loadstring", "load", "dofile",
    "collectgarbage", "table", "string", "math", "os", "io", "coroutine", "debug",
}


def read_corpus() -> str:
    parts = []
    for entry in CORPUS:
        path = FAF / entry
        if path.is_dir():
            parts.extend(f.read_text(errors="replace") for f in sorted(path.rglob("*.lua")))
        elif path.is_file():
            parts.append(path.read_text(errors="replace"))
    return "\n".join(parts)


def annotated_names() -> tuple[set[str], set[str]]:
    """(globals, methods) the engine annotations declare."""
    globals_, methods = set(), set()
    for f in sorted((FAF / "engine").rglob("*.lua")):
        text = f.read_text(errors="replace")
        globals_.update(re.findall(r"^function\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(", text, re.M))
        methods.update(re.findall(r"^function\s+[A-Za-z0-9_.]+:([A-Za-z0-9_]+)\s*\(", text, re.M))
    return globals_, methods


def called_names(corpus: str) -> tuple[dict[str, int], dict[str, int]]:
    """(globals, methods) the corpus calls, with call counts."""
    calls: dict[str, int] = {}
    for name in re.findall(r"(?<![\w.:])([A-Za-z_][A-Za-z0-9_]*)\s*\(", corpus):
        calls[name] = calls.get(name, 0) + 1
    methods: dict[str, int] = {}
    for name in re.findall(r":([A-Za-z0-9_]+)\s*\(", corpus):
        methods[name] = methods.get(name, 0) + 1
    return calls, methods


def build() -> tuple[list[tuple[str, int]], list[tuple[str, int]], list[tuple[str, int]]]:
    corpus = read_corpus()
    ann_globals, ann_methods = annotated_names()
    call_globals, call_methods = called_names(corpus)

    globals_ = sorted(
        ((n, c) for n, c in call_globals.items() if n in ann_globals and n not in STDLIB),
        key=lambda p: (-p[1], p[0]))
    methods = sorted(
        ((n, c) for n, c in call_methods.items() if n in ann_methods),
        key=lambda p: (-p[1], p[0]))
    # Called as a method, never annotated, and not defined anywhere in the corpus either: the
    # leak into game Lua the adapter has to shim.
    defined = set(re.findall(r"^\s*(?:function\s+[A-Za-z0-9_.]*[:.]|)([A-Za-z0-9_]+)\s*=\s*function",
                             corpus, re.M))
    defined.update(re.findall(r"function\s+[A-Za-z0-9_.]+[:.]([A-Za-z0-9_]+)\s*\(", corpus))
    shims = sorted(
        ((n, c) for n, c in call_methods.items() if n not in ann_methods and n not in defined),
        key=lambda p: (-p[1], p[0]))
    return globals_, methods, shims


def render(globals_, methods, shims) -> str:
    lines = [
        "// GENERATED by tools/gen_ai_bindings.py — do not edit.",
        "//",
        "// The engine surface FAF's AI actually calls, derived from the vendored corpus at the",
        "// pin in tools/fetch_ai.sh. Counts are call sites in that corpus, which is what makes",
        "// the ordering a work queue: the top of each list is what an unimplemented binding",
        "// costs the most.",
        "//",
        "// Consumed as X-macros so one list serves registration, the stub table and the",
        "// instrumentation report without three copies drifting apart.",
        "",
        f"// {len(globals_)} globals, {len(methods)} methods, {len(shims)} shims"
        f" — {len(globals_) + len(methods) + len(shims)} names in total.",
        "",
        "#ifndef RM_FAF_GLOBAL",
        "#define RM_FAF_GLOBAL(name, calls)",
        "#endif",
        "#ifndef RM_FAF_METHOD",
        "#define RM_FAF_METHOD(name, calls)",
        "#endif",
        "#ifndef RM_FAF_SHIM",
        "#define RM_FAF_SHIM(name, calls)",
        "#endif",
        "",
        "// Engine globals: free functions the corpus calls by name.",
    ]
    lines += [f'RM_FAF_GLOBAL("{n}", {c})' for n, c in globals_]
    lines += ["", "// Engine methods: called on a unit, brain, platoon or other engine object."]
    lines += [f'RM_FAF_METHOD("{n}", {c})' for n, c in methods]
    lines += ["",
              "// Called as methods, annotated nowhere, defined nowhere in the corpus: these leak",
              "// into game Lua outside the AI subset and need shims rather than bindings."]
    lines += [f'RM_FAF_SHIM("{n}", {c})' for n, c in shims]
    lines += ["", "#undef RM_FAF_GLOBAL", "#undef RM_FAF_METHOD", "#undef RM_FAF_SHIM", ""]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true",
                        help="exit non-zero if the committed table is out of date")
    args = parser.parse_args()

    if not FAF.is_dir():
        print(f"vendor/ai/faf missing — run `make ai` first ({FAF})", file=sys.stderr)
        return 2

    text = render(*build())
    if args.check:
        current = OUT.read_text() if OUT.exists() else ""
        if current != text:
            print(f"{OUT.relative_to(ROOT)} is out of date — run tools/gen_ai_bindings.py",
                  file=sys.stderr)
            return 1
        print(f"{OUT.relative_to(ROOT)} is up to date")
        return 0

    OUT.write_text(text)
    globals_, methods, shims = build()
    print(f"{OUT.relative_to(ROOT)}: {len(globals_)} globals, {len(methods)} methods, "
          f"{len(shims)} shims")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
