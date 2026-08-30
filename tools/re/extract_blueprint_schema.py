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

pairs, i = [], 0
while i < len(rows) - 1:
    va, kind, text = rows[i]
    nva, nkind, ntext = rows[i + 1]
    # key immediately followed by prose (a space-bearing sentence) = schema entry
    if (LO <= va < HI and KEY.match(text) and text not in LUA_CALLABLES
            and ' ' in ntext and len(ntext) > 12 and not KEY.match(ntext)):
        pairs.append((va, text, nva, ntext))
        i += 2
    else:
        i += 1

with open(OUT, 'w', encoding='utf-8') as f:
    f.write('key_va\tkey\tdoc_va\tdoc\n')
    for va, k, dva, d in pairs:
        f.write(f'{va:08x}\t{k}\t{dva:08x}\t{d}\n')

print(f'{len(pairs)} documented schema keys -> {OUT}')
known = {'StorageEnergy': 0x4f8, 'StorageMass': 0x4fc,
         'NaturalProducer': 0x500, 'RebuildBonusIds': 0x518}
found = [(k, va) for va, k, _, _ in pairs if k in known]
found.sort(key=lambda kv: kv[1])
print('validation -- string order vs known offset order:')
for k, va in found:
    print(f'  {va:#010x}  {k:<18} offset {known[k]:#x}')
offs = [known[k] for k, _ in found]
print('  ascending offsets?', offs == sorted(offs), offs)
