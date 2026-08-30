#!/usr/bin/env python3
"""Recover the native blueprint struct layout from the retail executable's own reflection.

Every blueprint class `RXxx` publishes itself to the engine's reflection system through
three small functions the compiler emits next to each other:

    <describe>      mov $<sizeof>, 0x8(%esi) ; call <base ctor> ; mov %esi,%eax ; call <registrar>
    <name getter>   mov $"RXxx", %eax ; ret
    <registrar>     one entry per member, in declaration order

Members come in two shapes and BOTH are needed -- reading only the first loses every
sub-struct and every enum-typed key:

  A. scalar / string / vector key
        push $<offset in this struct>
        push $<key name>
        call <per-type register fn>      ; the ONLY encoding of the member's type
        mov  %edi, 0xc(%eax)             ; a flag, 3 for every blueprint key seen
        mov  $<doc>, 0x10(%eax)          ; the engine's own one-line description

  B. sub-struct / enum member, emitted as its own thunk the registrar CALLs
        mov  $<member type descriptor>, 0x4(%esp)
        mov  $<offset in this struct>,   0xc(%esp)
        call <generic member register>
     The type descriptor is a real MSVC RTTI record, so the member's exact C++ type
     (`Moho::RUnitBlueprintDefenseShield`, `std::vector<Moho::RUnitBlueprintWeapon>`,
     `Moho::ELayer`) is recovered rather than guessed.

Parentage is READ, not inferred by offset plausibility: a registrar belongs to the class
whose describe function calls it, and a thunk belongs to the registrar that calls it.
Absolute blueprint offsets follow by composing the nesting chain.

Self-check: eleven offsets recovered by unrelated means in earlier sessions (the economy
allocator, the guard ladder, the transport bone matcher, the layer evaluator, the footprint
default) are re-derived here and printed as OK/MISMATCH on every run.  Regenerate with

    python3 tools/re/extract_blueprint_registrars.py \
        --exe <SupremeCommander.exe> --out build/re-fa/exports/blueprint_layout.tsv

Read-only.  Uses `objdump` and `pe_reader`; never opens the Ghidra project.
"""

import argparse
import collections
import re
import struct
import sys

from pe_reader import PE32

IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")

# --- byte patterns, all fixed-length MSVC codegen -----------------------------------
P_SIZEOF = re.compile(rb"\xc7\x46\x08(....)", re.S)          # mov $imm, 0x8(%esi)
P_NAMEGET = re.compile(rb"\xb8(....)\xc3", re.S)             # mov $imm, %eax ; ret
P_THUNK = re.compile(rb"\xc7\x44\x24\x04(....)", re.S)       # mov $imm, 0x4(%esp)
P_THUNK_OFF = re.compile(rb"\xc7\x44\x24\x0c(....)", re.S)   # mov $imm, 0xc(%esp)

# The per-type registration helpers.  Types with an RTTI descriptor in the helper body are
# read from it; the primitives carry no descriptor, so they are named from the observed
# offset stride between consecutive members of the same helper (the count is in brackets).
TYPE_FNS = {
    0x0040DFA0: "float",            # stride 4, 200/201 gaps; every documented "% per second"
    0x004F4710: "int32",            # stride 4; TransportClass, StorageSlots, MinBounceCount
    0x0040E020: "uint32",           # stride 4; the eight Intel radii only, distinct helper
    0x005178D0: "bool",             # stride 1; CanFly, StandUpright, NaturalProducer
    0x00513B10: "uint8",            # stride 1; SFootprint.SizeX/SizeZ (C-109 reads a byte)
    0x00514CF0: "std::string",      # RTTI: std::basic_string<char>
    0x00517850: "Moho::RResId",     # RTTI
    0x00519D30: "std::vector<std::string>",
    0x00519E30: "Moho::SFootprint",
    0x0052BEA0: "enum Moho::ERuleBPUnitMovementType",
    0x0052C020: "std::vector<float>",
    0x0052C120: "Moho::SMinMax<unsigned>",
}

# Cross-checks from claims derived by unrelated means, as `(absolute_offset, key)`.
CHECKS = [
    (0x4F8, "Economy.StorageEnergy"),   # C-159, from the economy allocator
    (0x4FC, "Economy.StorageMass"),     # C-159
    (0x500, "Economy.NaturalProducer"), # C-164, from a cmpb
    (0x460, "AI.GuardScanRadius"),      # C-183, from the guard ladder
    (0x48C, "AI.NeedUnpack"),           # C-183
    (0x3F8, "Transport.TransportClass"),# C-198, from the transport bone matcher
    (0x290, "Physics.MotionType"),      # C-205, Physics base + 0x18
    (0x0D8, "(top level).Footprint"),   # C-109 / C-205, the footprint record
    (0x0AC, "(top level).SizeX"),       # C-109, the float the footprint default ceil()s
    (0x0D8, "Footprint.SizeX"),         # C-109, the byte the default is written into
    (0x0D9, "Footprint.SizeZ"),         # C-109
]


ELEMENT_BASE = -(1 << 24)   # sentinel: offsets under a vector element are element-relative


class Image:
    def __init__(self, path: str):
        self.pe = PE32(path)
        text = next(s for s in self.pe.sections if s.name == ".text")
        self.text = text
        self.blob = self.pe.data[text.raw_offset:text.raw_offset + text.raw_size]
        self.base = text.virtual_address
        self.calls: dict[int, list[int]] = collections.defaultdict(list)
        self._index_calls()

    def off(self, va: int) -> int:
        return va - self.base

    def in_text(self, va: int) -> bool:
        return 0 <= self.off(va) < len(self.blob)

    def _index_calls(self) -> None:
        blob, base = self.blob, self.base
        for i in range(len(blob) - 5):
            if blob[i] == 0xE8:
                rel = struct.unpack_from("<i", blob, i + 1)[0]
                self.calls[base + i + 5 + rel].append(base + i)

    def ident(self, va: int) -> str | None:
        text = self.pe.identifier(va, 64)
        if text and 2 <= len(text) <= 48 and IDENT.match(text):
            return text
        return None

    def rtti_name(self, descriptor_va: int) -> str | None:
        """An MSVC TypeDescriptor holds its mangled name at +0x08."""
        name = self.pe.cstring(descriptor_va + 8, 200)
        if name and name.startswith(".?A"):
            return name
        return None


def demangle(mangled: str) -> str:
    """Enough of MSVC's scheme for the shapes that actually occur here."""
    if mangled.startswith(".?AV?$vector@V") or mangled.startswith(".?AV?$list@U"):
        inner = mangled.split("@", 1)[1]
        head = inner.split("V?$allocator")[0].split("@")[0].lstrip("VU")
        return f"vector<{head}>"
    body = mangled[4:] if mangled[3] in "VU" else mangled[4:]
    parts = [p for p in body.split("@") if p]
    if mangled.startswith(".?AW4"):
        parts = [p for p in mangled[5:].split("@") if p]
        return "enum " + parts[0]
    return parts[0] if parts else mangled


class Member:
    __slots__ = ("name", "offset", "kind", "type_name", "type_ref", "doc", "doc_va",
                 "site_va", "child")

    def __init__(self, name, offset, kind, type_name, type_ref, doc, doc_va, site_va):
        self.name, self.offset, self.kind = name, offset, kind
        self.type_name, self.type_ref = type_name, type_ref
        self.doc, self.doc_va, self.site_va = doc, doc_va, site_va
        self.child = None


class Klass:
    def __init__(self, name, describe_va, registrar_va, sizeof):
        self.name, self.describe_va = name, describe_va
        self.registrar_va, self.sizeof = registrar_va, sizeof
        self.members: list[Member] = []


def find_classes(img: Image, prefix: str) -> dict[str, Klass]:
    """Every `mov $"RXxx",%eax ; ret` is a reflection name getter; the describe function
    is the immediately preceding one, and it names both the size and the registrar."""
    out: dict[str, Klass] = {}
    for m in P_NAMEGET.finditer(img.blob):
        va = int.from_bytes(m.group(1), "little")
        name = img.ident(va)
        if not name or not name.startswith(tuple(prefix)):
            continue
        start = max(0, m.start() - 0x90)
        window = img.blob[start:m.start()]
        matches = list(P_SIZEOF.finditer(window))
        if not matches:
            continue
        sm = matches[-1]   # the one closest to the name getter is this class's
        sizeof = int.from_bytes(sm.group(1), "little")
        describe = img.base + start + sm.start() - 3
        if name in out:
            continue
        out[name] = Klass(name, describe, None, sizeof)
    return out


def parse_thunk(img: Image, va: int) -> tuple[str, int, str] | None:
    """Shape B: a one-member thunk.  Returns (member name, offset, member type)."""
    window = img.blob[img.off(va):img.off(va) + 0x80]
    tm = P_THUNK.search(window)
    om = P_THUNK_OFF.search(window)
    if not tm or not om:
        return None
    name_va = int.from_bytes(tm.group(1), "little")
    offset = int.from_bytes(om.group(1), "little")
    name = img.ident(name_va)
    if name is None:
        return None
    # the lazily-resolved member type: `push $<TypeDescriptor> ; call <lookup>`
    type_name = None
    for pm in re.finditer(rb"\x68(....)\xe8", window[:tm.start() + 0x20], re.S):
        mangled = img.rtti_name(int.from_bytes(pm.group(1), "little"))
        if mangled:
            type_name = demangle(mangled)
    return name, offset, type_name


def parse_registrar(img: Image, klass: Klass, start_va: int | None = None,
                    limit: int = 0x900, depth: int = 0) -> list[tuple[int, str]]:
    """Walk a registrar body once, in address order, collecting both member shapes.

    Every line is classified; anything the grammar does not know is returned so that a
    gap shows up as a printed count instead of a silent skip (evidence trap 3).
    """
    from subprocess import run
    if start_va is None:
        start_va = klass.describe_va
    end = start_va + limit
    out = run(["objdump", "-d", f"--start-address={start_va:#x}",
               f"--stop-address={end:#x}", ARGS.exe],
              capture_output=True, text=True, check=True).stdout
    line_re = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*\t(.*)$")
    regs: dict[str, int] = {}
    pushes: list[int] = []
    current: Member | None = None
    unparsed: list[tuple[int, str]] = []
    esi_to_eax = False       # `mov %esi,%eax` marks the next call as a sub-registrar

    for raw in out.splitlines():
        lm = line_re.match(raw)
        if not lm:
            continue
        va, text = int(lm.group(1), 16), lm.group(3).split("#")[0].strip()
        if text.startswith("int3") and current is not None and len(klass.members) > 0:
            break                                   # padding after the body
        if text in ("retl", "ret"):
            break
        m = re.match(r"^pushl\s+\$(0x[0-9a-f]+|-?\d+)$", text)
        if m:
            pushes.append(int(m.group(1), 0))
            continue
        m = re.match(r"^movl\s+\$(0x[0-9a-f]+),\s+%(e[a-z]{2})$", text)
        if m:
            regs[m.group(2)] = int(m.group(1), 16)
            continue
        m = re.match(r"^movl\s+%(e[a-z]{2}),\s+%(e[a-z]{2})$", text)
        if m:
            if m.group(1) == "esi" and m.group(2) == "eax":
                esi_to_eax = True
            if m.group(1) in regs:
                regs[m.group(2)] = regs[m.group(1)]
            else:
                regs.pop(m.group(2), None)
            continue
        m = re.match(r"^movl\s+\$(0x[0-9a-f]+),\s+(0x[0-9a-f]+)\(%eax\)$", text)
        if m and current is not None:
            slot, value = int(m.group(2), 16), int(m.group(1), 16)
            if slot == 0x10:
                current.doc_va, current.doc = value, img.pe.cstring(value, 400)
            continue
        m = re.match(r"^movl\s+%(e[a-z]{2}),\s+(0x[0-9a-f]+)\(%eax\)$", text)
        if m and current is not None:
            slot, value = int(m.group(2), 16), regs.get(m.group(1))
            if slot == 0x10 and value is not None:
                current.doc_va, current.doc = value, img.pe.cstring(value, 400)
            continue
        m = re.match(r"^(?:calll?|jmpl?)\s+(0x[0-9a-f]+)", text)
        if m:
            target = int(m.group(1), 16)
            if esi_to_eax and not pushes and depth == 0 and img.in_text(target):
                # the describe function delegating to the class's own registrar
                klass.registrar_va = target
                unparsed += parse_registrar(img, klass, target, limit, depth + 1)
                esi_to_eax = False
                current = None
                continue
            esi_to_eax = False
            name = img.ident(pushes[-1]) if pushes else None
            if name is not None and len(pushes) >= 2:
                current = Member(name, pushes[-2], "key", None, f"{target:08x}",
                                 None, None, va)
                klass.members.append(current)
            else:
                thunk = parse_thunk(img, target)
                if thunk is not None:
                    current = Member(thunk[0], thunk[1], "member", thunk[2],
                                     f"{target:08x}", None, None, va)
                    klass.members.append(current)
                else:
                    current = None
            pushes.clear()
            continue
        # `push %reg` where the register holds a known constant IS an operand: MSVC
        # reuses the hoisted flag constant 3 as the offset for a member at +3
        # (RProjectileBlueprintPhysics.VelocityAlign).  Dropping it loses a member
        # silently -- evidence trap 3, caught by a struct-tiling check.
        m = re.match(r"^pushl\s+%(e[a-z]{2})$", text)
        if m:
            if m.group(1) in regs:
                pushes.append(regs[m.group(1)])
            continue
        if re.match(r"^popl\s+%e[a-z]{2}$", text):
            continue
        if re.match(r"^(xorl)\s+%(e[a-z]{2}),\s+%\2$", text):
            regs[re.match(r"^xorl\s+%(e[a-z]{2}),", text).group(1)] = 0
            continue
        if re.match(r"^(movl|leal)\s+.*%e(bp|sp)", text) or text == "nop":
            continue
        unparsed.append((va, text))
    return unparsed


def main() -> int:
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--root", default="RUnitBlueprint,RPropBlueprint,RProjectileBlueprint,"
                                      "RMeshBlueprint,REmitterBlueprint,RBeamBlueprint,"
                                      "RTrailBlueprint")
    ap.add_argument("--rtti", default="build/re-fa/exports/rtti.bases.tsv")
    ARGS = ap.parse_args()

    img = Image(ARGS.exe)
    classes = find_classes(img, ("R", "S"))
    print(f"{len(classes)} reflection classes with an R-prefixed name getter", file=sys.stderr)

    unparsed_total: list[tuple[int, str]] = []
    for klass in classes.values():
        if not img.in_text(klass.describe_va):
            continue
        unparsed_total += parse_registrar(img, klass)
    print(f"{sum(len(k.members) for k in classes.values())} members, "
          f"{len(unparsed_total)} unrecognised instruction lines", file=sys.stderr)
    kinds = collections.Counter(t.split()[0] for _, t in unparsed_total)
    print(f"  unrecognised by mnemonic: {dict(kinds)}", file=sys.stderr)

    # ---- single-inheritance bases: the base subobject is at offset 0, so its members
    # ---- are members of the derived class at the same offsets.  Read from RTTI.
    bases: dict[str, list[str]] = collections.defaultdict(list)
    with open(ARGS.rtti) as handle:
        next(handle)
        for line in handle:
            f = line.rstrip("\n").split("\t")
            derived, base, offset = f[0], f[2], f[4]
            if derived.startswith("Moho::R") and base.startswith("Moho::R") and offset == "0x0":
                bases[derived.split("::")[1]].append(base.split("::")[1])

    def inherited(name: str, seen: set) -> list:
        """Base members first, in base-to-derived order, deduplicated."""
        if name in seen:
            return []
        seen.add(name)
        chain: list = []
        for base in bases.get(name, []):
            chain = inherited(base, seen) + chain
        klass = classes.get(name)
        return chain + (klass.members if klass else [])

    # ---- compose absolute offsets by walking the nesting chain -----------------------
    rows = []

    def walk(klass: Klass, base: int, path: str, root: str = "", depth: int = 0) -> None:
        if depth > 4:
            return
        root = root or klass.name
        for member in inherited(klass.name, set()):
            label = (member.type_name
                     or TYPE_FNS.get(int(member.type_ref, 16), f"call:{member.type_ref}"))
            type_name = label.replace("Moho::", "")
            element = None
            if type_name.startswith("vector<") and type_name[7:-1] in classes:
                element = classes[type_name[7:-1]]
            child = classes.get(type_name)
            section = path or "(top level)"
            absolute = ("element+0x%x" % member.offset if base <= ELEMENT_BASE
                        else "0x%x" % (base + member.offset))
            rows.append((root, section, member.name, member.offset, absolute,
                         label, member.doc or "", klass.name, member.site_va))
            if child is not None and child is not klass:
                walk(child, base + member.offset,
                     f"{path}.{member.name}" if path else member.name, root, depth + 1)
            elif element is not None:
                # a vector element has no fixed blueprint offset; offsets below are
                # relative to the element, and the element base is reported as -1
                walk(element, ELEMENT_BASE,
                     f"{path}.{member.name}[]" if path else f"{member.name}[]",
                     root, depth + 1)

    for root_name in ARGS.root.split(","):
        walk(classes[root_name], 0, "")

    with open(ARGS.out, "w") as handle:
        handle.write("blueprint\tsection\tkey\tstruct_offset\tabsolute_blueprint_offset\t"
                     "type_if_recoverable\tdoc\towning_struct\tsite_va\n")
        for bp, section, key, off, absolute, type_name, doc, owner, site in rows:
            handle.write(f"{bp}\t{section}\t{key}\t0x{off:x}\t{absolute}\t{type_name}\t"
                         f"{doc}\t{owner}\t{site:08x}\n")
    print(f"{len(rows)} rows -> {ARGS.out}", file=sys.stderr)

    # ---- cross-check against offsets derived by unrelated means ----------------------
    index = {f"{section}.{key}": int(absolute, 16)
             for bp, section, key, _, absolute, _, _, _, _ in rows
             if bp == "RUnitBlueprint" and absolute.startswith("0x")}
    ok = bad = 0
    for expected, key in CHECKS:
        got = index.get(key)
        mark = "OK " if got == expected else "MISMATCH"
        if got == expected:
            ok += 1
        else:
            bad += 1
        print(f"  {mark} {key:<28} expected 0x{expected:x} got "
              f"{'None' if got is None else hex(got)}", file=sys.stderr)
    print(f"cross-check: {ok} agree, {bad} disagree", file=sys.stderr)

    # ---- tiling self-check: a struct whose members do not cover it, beyond 3 bytes of
    # ---- alignment padding, means the walk MISSED a member.  This is what caught the
    # ---- `push %edi` offset for VelocityAlign; without it the miss looked like success.
    widths = {"float": 4, "int32": 4, "uint32": 4, "bool": 1, "uint8": 1,
              "std::string": 28, "Moho::RResId": 28, "std::vector<std::string>": 16,
              "std::vector<float>": 16, "Moho::SFootprint": 16,
              "Moho::SMinMax<unsigned>": 8}

    def width(label: str) -> int | None:
        """None means "unknown" -- the class is then skipped rather than mis-reported."""
        if label in widths:
            return widths[label]
        if label.startswith("enum"):
            return 4
        bare = label.replace("Moho::", "")
        if label.startswith("vector<"):
            return 20
        if bare in classes:
            return classes[bare].sizeof
        return None

    for name, klass in sorted(classes.items()):
        if not klass.members or klass.sizeof > 0x600:
            continue
        cover = bytearray(klass.sizeof)
        unknown = False
        for member in inherited(klass.name, set()):
            label = (member.type_name
                     or TYPE_FNS.get(int(member.type_ref, 16), "?"))
            size = width(label)
            if size is None:
                unknown = True
                break
            for i in range(member.offset, min(member.offset + size, klass.sizeof)):
                cover[i] = 1
        if unknown:
            continue
        runs, i = [], 8 if klass.name.startswith("R") else 0
        while i < klass.sizeof:
            if not cover[i]:
                j = i
                while j < klass.sizeof and not cover[j]:
                    j += 1
                if j - i > 3:
                    runs.append((i, j - i))
                i = j
            else:
                i += 1
        if runs:
            gaps = ", ".join(f"+0x{a:x}..+0x{a + n - 1:x} ({n} B)" for a, n in runs)
            print(f"  UNCOVERED {name}: {gaps}", file=sys.stderr)

    for name, klass in sorted(classes.items()):
        if klass.members:
            reg = "-" if klass.registrar_va is None else f"{klass.registrar_va:08x}"
            print(f"  {name:<34} sizeof=0x{klass.sizeof:<5x} describe={klass.describe_va:08x} "
                  f"registrar={reg} members={len(klass.members)}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
