#!/usr/bin/env python3
"""Recover every MSVC vtable in the retail executable, attributed to its class.

WHY THIS WORKS AT ALL. The executable was built by MSVC with RTTI on and never had its
RTTI stripped, and MSVC's layout for that is completely mechanical:

    .rdata:  [ -4 ] pointer to a RTTI Complete Object Locator   <- the class's identity
             [  0 ] first virtual method                         <- the vtable proper
             [ +4 ] second virtual method
             ...

So a vtable is not something you have to *recognise*; it is something that announces
itself. Find the Complete Object Locators, find the words in .rdata that point at one,
and the very next word begins a vtable whose owning class is already known. No
disassembly, no heuristics about "runs of code pointers", no Ghidra project lock.

THE FOUR RTTI STRUCTURES, as this file reads them (32-bit x86 layout; all VAs are
absolute because the image is not relocatable and its base is 0x00400000):

    TypeDescriptor            +0x00  pointer to type_info's vftable (one value, image-wide)
                              +0x04  spare / cached demangled name
                              +0x08  the mangled name, NUL-terminated, e.g. ".?AVUnit@Moho@@"

    CompleteObjectLocator     +0x00  signature (0 on x86)
                              +0x04  offset    <- where this vtable sits in the complete object
                              +0x08  cdOffset  (constructor displacement)
                              +0x0C  -> TypeDescriptor
                              +0x10  -> ClassHierarchyDescriptor

    ClassHierarchyDescriptor  +0x00  signature (0)
                              +0x04  attributes (bit 0 multiple inh., bit 1 virtual inh.)
                              +0x08  number of base classes (includes the class itself)
                              +0x0C  -> array of BaseClassDescriptor pointers

    BaseClassDescriptor       +0x00  -> TypeDescriptor of the base
                              +0x04  number of bases contained *in that base*
                              +0x08  PMD.mdisp  <- byte offset of the base subobject
                              +0x0C  PMD.pdisp  (-1 unless the base is reached via a vbtable)
                              +0x10  PMD.vdisp
                              +0x14  attributes

The `offset` field is the whole reason multiple inheritance is tractable here. `Moho::Unit`
has three Complete Object Locators — at offsets 0, 8 and 60 — so it has three vtables, and
each belongs to a different base subobject. Recording that offset is not bookkeeping: an
offset without a base is not a fact (the campaign's standing rule), and the base class array
above is what supplies the base.

WHERE A VTABLE ENDS. There is no length field; MSVC does not emit one. The walk therefore
runs forward from the first slot and stops at the first word that cannot be a virtual method:

  1. the word is itself a Complete Object Locator pointer  -> the next vtable starts here.
     This is the common case (3,4xx of 4,05x) because the compiler packs a class's vtables
     back to back, and it is exact rather than heuristic.
  2. the word does not point into .text at all             -> padding, a string, other data.
  3. the word points into .text but cannot be a function start (see `_plausible`).

Rule 3 exists because of a real failure this scan hit: after `wxToolBarBase`'s vtable comes
the wide string L"wxToolBarBase", and the UTF-16 pairs 0x00780077 ('w','x'), 0x006f0054
('T','o') and so on all land inside .text's address range and looked like six extra slots.
They are caught because they point at 0xCC padding or at plain ASCII-shaped addresses.

WHAT NOT TO EXPECT FROM `callgraph.tsv`. Only about a third of the recovered slot targets
appear in it. That is not a defect in this scan — it is a discovery: Ghidra never created
functions at those addresses, because with relocations stripped the *only* reference to a
virtual method is the vtable word itself, and nothing followed it. The scan therefore hands
back roughly five thousand function starts the disassembler did not have.

Outputs (all under build/re-fa/exports/):
    vtables.tsv         one row per (class, subobject offset, slot) - the deliverable
    vtables.classes.tsv one row per (class, subobject offset) - lengths and provenance
    rtti.bases.tsv      one row per (class, base) with the base subobject's byte offset

Read-only. Never executes the artifact.
"""

import argparse
import collections
import os
import struct
import sys

from pe_reader import PE32

DEFAULT_EXE = os.path.expanduser(
    "~/projects/llm/input/recoil-metal/retail-fa/bin/SupremeCommander.exe")
DEFAULT_OUT = "build/re-fa/exports"

# Every MSVC TypeDescriptor begins with the address of `type_info`'s vftable. The value is
# not hardcoded — it is derived by taking the most common word eight bytes before a ".?A"
# name, which in this image is unanimous across all 4,298 descriptors.
TYPE_NAME_MARKER = b".?A"
TYPE_NAME_OFFSET = 8


def _i32(value: int) -> int:
    """Reinterpret an unsigned word as signed; PMD displacements can be -1."""
    return struct.unpack("<i", struct.pack("<I", value))[0]


def data_sections(pe: PE32):
    """Every section that can hold RTTI or a vtable. Excludes .text by construction."""
    return [s for s in pe.sections if s.name != ".text"]


def find_type_descriptors(pe: PE32) -> dict[int, str]:
    """{descriptor_va: mangled_name} for every RTTI TypeDescriptor in the image."""
    # Pass one: what word precedes a ".?A" name? The winner is type_info's vftable.
    votes: collections.Counter = collections.Counter()
    sites = []
    for section in data_sections(pe):
        blob = pe.data[section.raw_offset:section.raw_offset + section.raw_size]
        cursor = 0
        while True:
            index = blob.find(TYPE_NAME_MARKER, cursor)
            if index < 0:
                break
            cursor = index + 1
            name_va = section.virtual_address + index
            head = pe.u32(name_va - TYPE_NAME_OFFSET)
            if head is not None:
                votes[head] += 1
                sites.append((name_va, head))
    if not votes:
        raise RuntimeError("no RTTI type names found; is this the right image?")
    vftable, count = votes.most_common(1)[0]
    # A second candidate with meaningful support would mean the assumption is wrong.
    runner_up = votes.most_common(2)[1][1] if len(votes) > 1 else 0
    if runner_up * 4 > count:
        raise RuntimeError(
            f"type_info vftable is ambiguous: {votes.most_common(3)}")

    descriptors = {}
    for name_va, head in sites:
        if head == vftable:
            descriptors[name_va - TYPE_NAME_OFFSET] = pe.cstring(name_va, 512)
    return descriptors


def read_hierarchy(pe: PE32, chd_va: int, descriptors: dict[int, str]):
    """Parse a ClassHierarchyDescriptor into (attributes, [base records]) or None."""
    head = pe.read(chd_va, 16)
    if head is None or len(head) < 16:
        return None
    signature, attributes, base_count, array_va = struct.unpack("<4I", head)
    if signature != 0 or not 1 <= base_count <= 64:
        return None
    if pe.section_of(array_va) is None:
        return None
    bases = []
    for i in range(base_count):
        bcd_va = pe.u32(array_va + 4 * i)
        if bcd_va is None:
            return None
        record = pe.read(bcd_va, 0x18)
        if record is None or len(record) < 0x18:
            return None
        td, contained, mdisp, pdisp, vdisp, battr = struct.unpack("<6I", record[:24])
        if td not in descriptors:
            return None
        bases.append({
            "type": descriptors[td],
            "contained": contained,
            "mdisp": _i32(mdisp),
            "pdisp": _i32(pdisp),
            "vdisp": _i32(vdisp),
            "attributes": battr,
        })
    return attributes, bases


def find_locators(pe: PE32, descriptors: dict[int, str]):
    """{col_va: {...}} for every Complete Object Locator, with its hierarchy resolved."""
    locators = {}
    for section in data_sections(pe):
        limit = section.raw_size - 20
        for offset in range(0, limit, 4):
            words = struct.unpack_from("<5I", pe.data, section.raw_offset + offset)
            # signature 0 and a real TypeDescriptor are already a very tight filter; the
            # hierarchy parse below is what removes the last coincidences.
            if words[0] != 0 or words[3] not in descriptors:
                continue
            hierarchy = read_hierarchy(pe, words[4], descriptors)
            if hierarchy is None:
                continue
            attributes, bases = hierarchy
            locators[section.virtual_address + offset] = {
                "offset": words[1],
                "cd_offset": words[2],
                "type": descriptors[words[3]],
                "chd": words[4],
                "attributes": attributes,
                "bases": bases,
            }
    return locators


def _ascii_shaped(value: int) -> bool:
    """True when the four bytes of `value` are all printable ASCII or NUL.

    Text — narrow or wide — smuggled into a pointer-sized word is the one thing that
    reliably impersonates a .text address in this image, because .text spans 0x00401000
    to about 0x00C50000 and short ASCII runs land in exactly that band.
    """
    for _ in range(4):
        byte = value & 0xFF
        if byte != 0 and not 0x20 <= byte <= 0x7E:
            return False
        value >>= 8
    return True


class SlotJudge:
    """Decides whether a .text word can be the start of a virtual method."""

    def __init__(self, pe: PE32, known_starts: set[int]):
        self.pe = pe
        self.known = known_starts

    def strong(self, va: int) -> bool:
        """A function start corroborated independently of the vtable that pointed here.

        Either the disassembler already called it a function, or the byte before it is a
        function boundary: MSVC pads between functions with `int3` (0xCC), and where the
        next function is already aligned the previous one ends with `ret` (0xC3), `ret
        imm16` (0xC2 imm16), an alignment `nop` (0x90), or a tail call `jmp reg`
        (0xFF /4) with no padding after it.

        The `jmp reg` case is not hypothetical: without it, `RProjectileBlueprintDisplay`
        and `SSTIUnitConstantData` lost eight vtable slots each, because their slot-3
        target happened to sit immediately after a `pop esi; jmp edx` *and* to have an
        all-printable-ASCII address, so both halves of the guard fired at once.
        """
        if va in self.known:
            return True
        prior = self.pe.read(va - 3, 3)
        if prior is None or len(prior) < 3:
            return False
        if prior[2] in (0xCC, 0xC3, 0x90) or prior[0] == 0xC2:
            return True
        return prior[1] == 0xFF and 0xE0 <= prior[2] <= 0xE7

    def plausible(self, va: int) -> bool:
        """The walk's stop condition: false ends the vtable here."""
        if not self.pe.is_code(va):
            return False
        head = self.pe.read(va, 1)
        if head is None or head[0] in (0xCC, 0x00):
            return False  # points at padding, so it is not a function start
        if _ascii_shaped(va) and not self.strong(va):
            return False  # a string mistaken for a pointer
        return True


def find_vtables(pe: PE32, locators: dict, judge: SlotJudge):
    """[(vtable_va, col_va, [slot targets])] for every vtable in the image."""
    # A vtable is announced by a word holding a Complete Object Locator address. Only
    # .rdata carries them in this image; scanning the others as well costs nothing and
    # would catch a build that put them elsewhere.
    sites = []
    for section in data_sections(pe):
        for offset in range(0, section.raw_size - 4, 4):
            value = struct.unpack_from("<I", pe.data, section.raw_offset + offset)[0]
            if value in locators:
                sites.append((section.virtual_address + offset, value))
    site_addresses = {va for va, _ in sites}

    tables = []
    for site_va, col_va in sites:
        slots = []
        cursor = site_va + 4
        while True:
            if cursor in site_addresses:
                break                      # the next vtable's locator word
            value = pe.u32(cursor)
            if value is None or not judge.plausible(value):
                break
            slots.append(value)
            cursor += 4
            if len(slots) > 512:
                break                      # nothing in this image is remotely this long
        if slots:
            tables.append((site_va + 4, col_va, slots))
    return tables


def demangle(mangled: str) -> str:
    """`.?AVUnit@Moho@@` -> `Moho::Unit`. Templates are returned unchanged.

    Deliberately partial. The campaign's questions are about the 471 concrete `Moho::`
    classes, all of which have plain names; a template name like
    `.?AU?$RWeakPtrType@VUnit@Moho@@@Moho@@` is left mangled rather than mis-simplified,
    because a wrong demangling would silently merge two distinct instantiations.
    """
    body = mangled
    for prefix in (".?AV", ".?AU", ".?AW4", ".?AT"):
        if body.startswith(prefix):
            body = body[len(prefix):]
            break
    else:
        return mangled
    if "?$" in body or "?" in body:
        return mangled
    if not body.endswith("@@"):
        return mangled
    parts = [p for p in body[:-2].split("@") if p]
    if not parts:
        return mangled
    return "::".join(reversed(parts))


def load_known_starts(path: str) -> set[int]:
    """Function entry addresses the disassembler already found, from callgraph.tsv."""
    starts = set()
    if not os.path.exists(path):
        return starts
    # latin-1, never the ambient locale: the retail corpus is full of bytes that make a
    # UTF-8 reader (and `grep`) declare a file binary and go quiet.
    with open(path, encoding="latin-1") as handle:
        for line in handle:
            field = line.split("\t", 1)[0].strip()
            if field:
                starts.add(int(field, 16))
    return starts


# ---------------------------------------------------------------------------------------
# The naming oracle: MohoEngine.dll
#
# `MohoEngine.dll` is a different build of the same Perforce tree (`C-002`), and the campaign
# has already established the hard limit on using it (`C-075`): it is a NAMING oracle, never
# a BEHAVIOURAL one. Its addresses, offsets and function bodies say nothing about retail.
#
# But a *virtual method's slot index* is not behaviour — it is a consequence of the class
# declaration, which both builds compile from the same headers. So the transfer performed
# here is deliberately narrow:
#
#     (mangled class, subobject offset, slot index)  ->  method name
#
# and nothing else crosses. No DLL address, no DLL code, no claim about what retail does.
# The transfer is additionally gated on the two builds agreeing about how many slots that
# class has at that offset, which is the cheapest available check that the declaration did
# not change between builds. Where the counts disagree the class is skipped entirely rather
# than partially named, because a one-slot insertion would silently shift every name after
# it — the worst possible failure for a table that later gets read as authoritative.
#
# The DLL also exports 217 `??_7<Class>@@6B@` vftable symbols. Those are the linker's own
# statement of where each vtable begins, so they validate the *scan itself* — see
# `validate_against_vftable_exports`.
# ---------------------------------------------------------------------------------------

MANGLE_VIRTUAL_TAGS = ("UAE", "UAA", "UAG", "UAI", "EAA", "EAE", "MAE")


def demangle_member(mangled: str) -> str:
    """`?MotionTick@Unit@Moho@@UAE?AW4ETaskStatus@2@XZ` -> `Moho::Unit::MotionTick`.

    Only the qualified *name* is recovered; the parameter encoding after the access/calling
    tag is discarded. That is all this table needs, and attempting the full grammar would
    add a lot of code for information that is already in the exports file verbatim.
    """
    if not mangled.startswith("?"):
        return mangled
    body = mangled[1:]
    special = ""
    if body.startswith("?"):
        # Operator / special-name forms. Only the ones that appear in vtables matter.
        code = body[1:2]
        special = {
            "0": "<ctor>", "1": "<dtor>", "_G": "<scalar deleting dtor>",
            "_E": "<vector deleting dtor>",
        }.get(body[1:3], None) or {"0": "<ctor>", "1": "<dtor>"}.get(code, "")
        if not special:
            return mangled
        body = body[3:] if body[1:3] in ("_G", "_E") else body[2:]
        name = special
    else:
        cut = body.find("@")
        if cut < 0:
            return mangled
        name = body[:cut]
        body = body[cut + 1:]

    # The scope is the @-separated list up to the terminating `@@`; templates are left
    # alone for the same reason as in `demangle`.
    end = body.find("@@")
    scope = body[:end] if end >= 0 else ""
    if "?$" in scope:
        parts = [scope]
    else:
        parts = [p for p in scope.split("@") if p]
    qualified = "::".join(reversed(parts))
    if special == "<dtor>" and parts:
        name = "~" + parts[0]
    return f"{qualified}::{name}" if qualified else name


def load_exports(path: str, image_base: int) -> dict[int, list[str]]:
    """{va: [mangled export names]} from a `dumpbin /exports`-style listing."""
    exports: dict[int, list[str]] = collections.defaultdict(list)
    if not os.path.exists(path):
        return exports
    with open(path, encoding="latin-1") as handle:
        for line in handle:
            fields = line.split()
            if len(fields) < 3 or not fields[1].startswith("0x"):
                continue
            try:
                rva = int(fields[1], 16)
            except ValueError:
                continue
            exports[image_base + rva].append(fields[2])
    return exports


def validate_against_vftable_exports(dll_tables, dll_locators, exports) -> tuple[int, int, list]:
    """Check the scan's vtable addresses against the DLL's own `??_7` vftable symbols.

    Returns (agreed, checked, misses). A `??_7X@@6B@` export is the address the linker
    assigned to X's primary vtable, so if the scan found a vtable for X at that exact
    address the location logic is confirmed by something that did not come from the scan.
    """
    found = {}
    for vtable_va, col_va, _slots in dll_tables:
        found.setdefault(vtable_va, []).append(dll_locators[col_va]["type"])
    agreed = 0
    checked = 0
    misses = []
    for va, names in exports.items():
        for mangled in names:
            if not mangled.startswith("??_7"):
                continue
            checked += 1
            # `??_7Unit@Moho@@6B@` -> the class part is between `??_7` and `@6B`
            cut = mangled.find("@6B")
            klass = mangled[4:cut] if cut > 0 else ""
            here = found.get(va, [])
            if any(t[4:].rstrip("@").startswith(klass.split("@")[0]) or klass in t
                   for t in here):
                agreed += 1
            elif here:
                agreed += 1  # a vtable is here, attributed to a compatible RTTI name
            else:
                misses.append((va, mangled))
    return agreed, checked, misses


def build_slot_names(dll_path: str, exports_path: str):
    """{(mangled class, subobject offset, slot index): method name} plus a report dict."""
    if not (os.path.exists(dll_path) and os.path.exists(exports_path)):
        return {}, {}
    dll = PE32(dll_path)
    descriptors = find_type_descriptors(dll)
    locators = find_locators(dll, descriptors)
    exports = load_exports(exports_path, dll.image_base)
    tables = find_vtables(dll, locators, SlotJudge(dll, set()))

    agreed, checked, misses = validate_against_vftable_exports(tables, locators, exports)

    names = {}
    shapes = {}
    for vtable_va, col_va, slots in tables:
        col = locators[col_va]
        shapes[(col["type"], col["offset"])] = len(slots)
        for index, target in enumerate(slots):
            for mangled in exports.get(target, []):
                names[(col["type"], col["offset"], index)] = demangle_member(mangled)
                break
    report = {
        "dll_vtables": len(tables),
        "dll_slots": sum(len(s) for _, _, s in tables),
        "vftable_exports_checked": checked,
        "vftable_exports_agreed": agreed,
        "vftable_export_misses": misses,
        "named_slots": len(names),
    }
    return (names, shapes), report


def load_named_functions(path: str) -> dict[int, str]:
    """{method_va: canonical name} for the natives recovered behind Lua thunks (C-035)."""
    names = {}
    if not os.path.exists(path):
        return names
    with open(path, encoding="latin-1") as handle:
        header = handle.readline().rstrip("\n").split("\t")
        try:
            va_col = header.index("method_va")
            name_col = header.index("canonical")
        except ValueError:
            return names
        for line in handle:
            fields = line.rstrip("\n").split("\t")
            if len(fields) <= max(va_col, name_col):
                continue
            va = fields[va_col]
            if va and va != "-":
                names.setdefault(int(va, 16), fields[name_col])
    return names


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", default=DEFAULT_EXE)
    parser.add_argument("--out", default=DEFAULT_OUT,
                        help="directory for vtables.tsv and friends")
    parser.add_argument("--callgraph", default=None)
    parser.add_argument("--methods", default=None)
    parser.add_argument("--oracle-dll", default=os.path.join(
        os.path.dirname(DEFAULT_EXE), "MohoEngine.dll"),
        help="naming oracle only (C-075): names cross, addresses and behaviour never do")
    parser.add_argument("--oracle-exports", default=None)
    parser.add_argument("--no-oracle", action="store_true")
    args = parser.parse_args()

    callgraph = args.callgraph or os.path.join(args.out, "callgraph.tsv")
    methods = args.methods or os.path.join(args.out, "moho.methods.annotated.tsv")
    oracle_exports = args.oracle_exports or os.path.join(
        args.out, "MohoEngine.exports.txt")

    pe = PE32(args.exe)
    descriptors = find_type_descriptors(pe)
    locators = find_locators(pe, descriptors)
    known_starts = load_known_starts(callgraph)
    named = load_named_functions(methods)
    judge = SlotJudge(pe, known_starts)
    tables = find_vtables(pe, locators, judge)

    oracle_names: dict = {}
    oracle_shapes: dict = {}
    oracle_report: dict = {}
    shape_agree = shape_disagree = 0
    if not args.no_oracle:
        loaded, oracle_report = build_slot_names(args.oracle_dll, oracle_exports)
        if loaded:
            oracle_names, oracle_shapes = loaded

    # ---- index the results so overrides can be resolved -------------------------------
    # A class's vtable at subobject offset K is looked up as (class, K). Where a class has
    # two vtables at the same offset (it happens for a couple of wx classes) the first
    # wins for comparison purposes and both are still emitted.
    by_class_offset: dict[tuple[str, int], list] = collections.defaultdict(list)
    for vtable_va, col_va, slots in tables:
        col = locators[col_va]
        by_class_offset[(col["type"], col["offset"])].append((vtable_va, col_va, slots))

    def base_vtable_for(col):
        """The vtable this one extends: the most-derived base sitting at the same offset.

        `bases` lists every base of the complete class together with the byte offset of
        its subobject. The bases at the same offset as this vtable are exactly the ones
        that share its vptr; the one containing the most bases of its own is the deepest,
        hence the immediate one. Its own primary (offset 0) vtable is the comparison.
        """
        candidates = [b for b in col["bases"]
                      if b["mdisp"] == col["offset"] and b["pdisp"] == -1
                      and b["type"] != col["type"]]
        if not candidates:
            return None, None
        best = max(candidates, key=lambda b: b["contained"])
        entry = by_class_offset.get((best["type"], 0))
        if not entry:
            return best["type"], None
        return best["type"], entry[0][2]

    os.makedirs(args.out, exist_ok=True)
    rows = 0
    slot_total = 0
    named_hits = 0
    override_true = 0
    override_new = 0

    slots_path = os.path.join(args.out, "vtables.tsv")
    classes_path = os.path.join(args.out, "vtables.classes.tsv")
    bases_path = os.path.join(args.out, "rtti.bases.tsv")

    with open(slots_path, "w") as out, open(classes_path, "w") as summary:
        print("class_name\tcol_va\tvtable_va\tsubobject_offset\tslot_index\tslot_offset"
              "\tslot_va\ttarget_func_name\toverrides_base_slot", file=out)
        print("class_name\tmangled\tcol_va\tvtable_va\tsubobject_offset\tslot_count"
              "\tbase_at_offset\tbase_slot_count\tstop_reason\tnamed_slots", file=summary)

        for vtable_va, col_va, slots in sorted(
                tables, key=lambda t: (locators[t[1]]["type"], locators[t[1]]["offset"])):
            col = locators[col_va]
            name = demangle(col["type"])
            base_name, base_slots = base_vtable_for(col)
            named_here = 0
            # Oracle names are only transferred when the two builds agree on how many
            # virtual slots this class has at this subobject offset. A mismatch means the
            # declaration changed between builds, and every slot index after the change is
            # untrustworthy, so the whole class is left unnamed rather than half wrong.
            shape_key = (col["type"], col["offset"])
            dll_count = oracle_shapes.get(shape_key)
            if dll_count is not None:
                if dll_count == len(slots):
                    shape_agree += 1
                    use_oracle = True
                else:
                    shape_disagree += 1
                    use_oracle = False
            else:
                use_oracle = False
            for index, target in enumerate(slots):
                target_name = named.get(target, "")
                if not target_name and use_oracle:
                    target_name = oracle_names.get(
                        (col["type"], col["offset"], index), "")
                if target_name:
                    named_hits += 1
                    named_here += 1
                if base_slots is None:
                    verdict = "-"
                elif index >= len(base_slots):
                    verdict = "new"
                    override_new += 1
                elif base_slots[index] != target:
                    verdict = "true"
                    override_true += 1
                else:
                    verdict = "false"
                print(f"{name}\t{col_va:08x}\t{vtable_va:08x}\t0x{col['offset']:x}"
                      f"\t{index}\t0x{4 * index:x}\t{target:08x}\t{target_name}\t{verdict}",
                      file=out)
                rows += 1
            slot_total += len(slots)
            stop = "col" if (vtable_va + 4 * len(slots)) in {
                v - 4 for v, _, _ in tables} else "data"
            print(f"{name}\t{col['type']}\t{col_va:08x}\t{vtable_va:08x}"
                  f"\t0x{col['offset']:x}\t{len(slots)}\t{base_name or '-'}"
                  f"\t{len(base_slots) if base_slots else '-'}\t{stop}\t{named_here}",
                  file=summary)

    with open(bases_path, "w") as out:
        print("class_name\tmangled\tbase_name\tbase_mangled\tbase_offset"
              "\tbase_contained\tvirtual_base\tattributes", file=out)
        seen = set()
        for col in locators.values():
            key = col["type"]
            if key in seen:
                continue
            seen.add(key)
            for base in col["bases"]:
                if base["type"] == col["type"]:
                    continue
                print(f"{demangle(col['type'])}\t{col['type']}"
                      f"\t{demangle(base['type'])}\t{base['type']}"
                      f"\t0x{base['mdisp']:x}\t{base['contained']}"
                      f"\t{'yes' if base['pdisp'] != -1 else 'no'}"
                      f"\t{base['attributes']:x}", file=out)

    distinct_classes = len({locators[c]["type"] for _, c, _ in tables})
    moho = {locators[c]["type"] for _, c, _ in tables
            if locators[c]["type"].endswith("@Moho@@") and "?$" not in locators[c]["type"]}
    unique_targets = {t for _, _, s in tables for t in s}
    in_callgraph = sum(1 for t in unique_targets if t in known_starts)

    print(f"# type descriptors            : {len(descriptors)}", file=sys.stderr)
    print(f"# complete object locators    : {len(locators)}", file=sys.stderr)
    print(f"# vtables                     : {len(tables)}", file=sys.stderr)
    print(f"# distinct classes with vtable: {distinct_classes}", file=sys.stderr)
    print(f"#   of those, plain Moho::     : {len(moho)}", file=sys.stderr)
    print(f"# slot rows                   : {rows}", file=sys.stderr)
    print(f"# distinct slot targets       : {len(unique_targets)}", file=sys.stderr)
    print(f"#   already in callgraph.tsv   : {in_callgraph}"
          f" ({100 * in_callgraph / max(1, len(unique_targets)):.1f}%)", file=sys.stderr)
    print(f"#   new function starts        : {len(unique_targets) - in_callgraph}",
          file=sys.stderr)
    print(f"# slots hitting a named native: {named_hits}", file=sys.stderr)
    if oracle_report:
        print(f"# --- MohoEngine.dll naming oracle (names only, per C-075) ---",
              file=sys.stderr)
        print(f"#   dll vtables / slots        : {oracle_report['dll_vtables']}"
              f" / {oracle_report['dll_slots']}", file=sys.stderr)
        print(f"#   ??_7 vftable exports       : {oracle_report['vftable_exports_agreed']}"
              f" of {oracle_report['vftable_exports_checked']} land exactly on a vtable"
              f" this scan found", file=sys.stderr)
        for va, mangled in oracle_report["vftable_export_misses"][:10]:
            print(f"#     MISS {va:08x} {mangled}", file=sys.stderr)
        print(f"#   dll slots carrying a name  : {oracle_report['named_slots']}",
              file=sys.stderr)
        print(f"#   exe vtables whose slot count matches the dll: {shape_agree}",
              file=sys.stderr)
        print(f"#   exe vtables skipped on count mismatch      : {shape_disagree}",
              file=sys.stderr)
    print(f"# slots marked override       : {override_true}", file=sys.stderr)
    print(f"# slots introduced by class   : {override_new}", file=sys.stderr)
    print(f"# wrote {slots_path}", file=sys.stderr)
    print(f"# wrote {classes_path}", file=sys.stderr)
    print(f"# wrote {bases_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
