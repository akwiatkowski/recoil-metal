"""Recover the blueprint schema by pairing each key name with its own doc string.

The engine stores every blueprint key next to a human-readable description of
it, in `.rdata`, in DECLARATION ORDER -- which is also struct-offset order.
So a plain sweep of that region reconstructs the schema without touching the
loader, and any blueprint offset can then be named by its position between two
known ones.

Validated against four independently-derived offsets (see the campaign ledger):
  Economy.StorageEnergy  = +0x4f8   (C-159)
  Economy.StorageMass    = +0x4fc   (C-159)
  Economy.NaturalProducer= +0x500   (C-164)
  RebuildBonusIds        = +0x518   (C-148)
all four appear in ascending string order, matching ascending offset order.

Read-only. Never executes the target.
"""
import csv
import re

TSV = 'build/re-fa/exports/strings.all.tsv'
METHODS = 'build/re-fa/exports/moho.methods.annotated.tsv'
OUT = 'build/re-fa/exports/blueprint_schema.tsv'

# The same .rdata block also holds the Lua API's documentation (C-032), which
# has the identical name-then-prose shape -- 586 of a naive sweep's hits were
# Lua callables like Dirname and GetCargo, not blueprint keys. Subtract them.
LUA_CALLABLES = set()
with open(METHODS, encoding='latin-1') as _f:
    for _row in csv.DictReader(_f, delimiter='\t'):
        if _row.get('name'):
            LUA_CALLABLES.add(_row['name'])

# A key is a bare CamelCase identifier; its doc is prose that follows it.
KEY = re.compile(r'^[A-Z][A-Za-z0-9_]{2,39}$')

# The documented-schema block measured by density: 99% of pairs fall in this
# span, and outside it the same key-then-prose shape matches unrelated text
# (D3D error messages pair a CamelCase symbol with a sentence too). Bounding
# it keeps the count honest rather than inflating it with false positives.
LO, HI = 0x00e50000, 0x00eb0000

rows = []
with open(TSV, encoding='latin-1') as f:
    next(f)
    for line in f:
        p = line.rstrip('\n').split('\t')
        if len(p) < 5 or p[1] != '.rdata':
            continue
        rows.append((int(p[0], 16), p[2], p[4]))
rows.sort()

# A key is documented by prose IMMEDIATELY following it. A key followed by another
# key is UNDOCUMENTED -- it is still a schema key and must be emitted, but it has no
# description of its own. Concretely: Class2AttachSize is documented, and
# Class3/Class4/ClassSAttachSize are three real keys with no doc at all; the prose
# after them belongs to AirClass, the last key in the run.
#
# An earlier version required key-then-prose and silently dropped the three
# undocumented ones. A first fix over-corrected by attributing the run's prose to
# every key in it, which handed Class3AttachSize the AirClass description -- wrong
# in the other direction, and caught by reading the transport block back.
pairs, i = [], 0
while i < len(rows):
    va, kind, text = rows[i]
    if not (LO <= va < HI and KEY.match(text) and text not in LUA_CALLABLES):
        i += 1
        continue
    # collect the maximal run of consecutive keys starting here
    run = []
    while i < len(rows):
        va2, _, text2 = rows[i]
        if LO <= va2 < HI and KEY.match(text2) and text2 not in LUA_CALLABLES:
            run.append((va2, text2))
            i += 1
        else:
            break
    doc_va, doc = None, ''
    if i < len(rows):
        nva, _, ntext = rows[i]
        if ' ' in ntext and len(ntext) > 12 and not KEY.match(ntext):
            doc_va, doc = nva, ntext
            i += 1
    if doc_va is None:
        continue                      # a run with no prose after it is not schema
    for n, (kva, ktext) in enumerate(run):
        last = (n == len(run) - 1)
        pairs.append((kva, ktext, doc_va if last else 0, doc if last else '',
                      0 if last else 1))

with open(OUT, 'w', encoding='utf-8') as f:
    f.write('key_va\tkey\tdoc_va\tdoc\tundocumented\n')
    for va, k, dva, d, sh in pairs:
        f.write(f'{va:08x}\t{k}\t{dva:08x}\t{d}\t{sh}\n')

print(f'{len(pairs)} documented schema keys -> {OUT}')
known = {'StorageEnergy': 0x4f8, 'StorageMass': 0x4fc,
         'NaturalProducer': 0x500, 'RebuildBonusIds': 0x518}
found = [(k, va) for va, k, _, _, _ in pairs if k in known]
found.sort(key=lambda kv: kv[1])
print('validation -- string order vs known offset order:')
for k, va in found:
    print(f'  {va:#010x}  {k:<18} offset {known[k]:#x}')
offs = [known[k] for k, _ in found]
print('  ascending offsets?', offs == sorted(offs), offs)
