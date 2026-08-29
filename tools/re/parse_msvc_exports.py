#!/usr/bin/env python3
"""Parse MSVC-mangled PE export names into a coarse class -> method dictionary.

This is deliberately a *heuristic* parser, not a full MSVC demangler. The goal is
an inventory of engine classes and their member names, not exact signatures:
Ghidra demangles properly once the DLL is imported, and this runs in a second.

MSVC name-mangling shape we exploit:

    ?Method@Class@Namespace@@<signature>
    ??0Class@Namespace@@<signature>     constructor
    ??1Class@Namespace@@<signature>     destructor
    ??_7Class@Namespace@@6B@            vtable
    ??$TemplateFn@<targs>@Namespace@@   template  (skipped: nested @@)

The name-fragment chain is everything before the first "@@" that is not inside a
template argument, and it is stored innermost-first, so reversing it yields
Namespace::Class::Method.
"""

import re
import sys
from collections import defaultdict
from pathlib import Path

EXPORT_LINE = re.compile(r"^\s*(\d+)\s+(0x[0-9a-f]+)\s+(\S+)\s*$")

# Special MSVC "??<code>" prefixes we care to label rather than drop.
SPECIAL = {
    "0": "<ctor>",
    "1": "<dtor>",
    "_7": "<vftable>",
    "_8": "<vbtable>",
    "_E": "<vector deleting dtor>",
    "_G": "<scalar deleting dtor>",
}


def split_chain(mangled: str):
    """Return (kind, fragments) where fragments are innermost-first, or None."""
    if not mangled.startswith("?"):
        # Plain C export (__cdecl, extern "C") — no class information.
        return ("c", [mangled])

    body = mangled[1:]
    kind = "method"

    if body.startswith("?"):
        body = body[1:]
        if body.startswith("$"):
            # Template function/method. Its argument list embeds further "@@"
            # runs, which this parser cannot segment reliably. Recorded raw so
            # the count is honest, but not attributed to a class.
            return ("template", [mangled])
        for code, label in SPECIAL.items():
            if body.startswith(code):
                body = body[len(code):]
                kind = label
                break
        else:
            # Operator overload (??2 = new, ??8 = ==, ...). Same chain shape.
            body = body[1:]
            kind = "<operator>"

    # The fragment chain ends at the first "@@"; the signature follows.
    end = body.find("@@")
    if end < 0:
        return None
    chain = [f for f in body[:end].split("@") if f]
    if kind != "method":
        chain = [kind] + chain
    return (kind, chain)


def main(paths):
    classes = defaultdict(set)   # "Moho::CUnit" -> {method names}
    free = defaultdict(set)      # namespace -> {free function names}
    stats = defaultdict(int)

    for path in paths:
        for line in Path(path).read_text(errors="replace").splitlines():
            m = EXPORT_LINE.match(line)
            if not m:
                continue
            mangled = m.group(3)
            stats["exports"] += 1
            parsed = split_chain(mangled)
            if parsed is None:
                stats["unparsed"] += 1
                continue
            kind, chain = parsed
            if kind in ("c", "template"):
                stats[kind] += 1
                continue
            if len(chain) < 2:
                free["<global>"].add(chain[0])
                stats["free"] += 1
                continue
            member, scope = chain[0], chain[1:]
            owner = "::".join(reversed(scope))
            # A single-fragment scope that is a known namespace means the symbol
            # is a free function in that namespace, not a class member.
            if owner in ("Moho", "gpg", "Wm3", "LuaPlus", "std"):
                free[owner].add(member)
                stats["free"] += 1
            else:
                classes[owner].add(member)
                stats["member"] += 1

    print(f"# exports parsed: {dict(stats)}", file=sys.stderr)
    for owner in sorted(classes, key=lambda k: (-len(classes[k]), k)):
        print(f"{owner}\t{len(classes[owner])}\t{' '.join(sorted(classes[owner]))}")
    for ns in sorted(free):
        print(f"[free]{ns}\t{len(free[ns])}\t{' '.join(sorted(free[ns]))}")


if __name__ == "__main__":
    main(sys.argv[1:])
