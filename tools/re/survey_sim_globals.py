"""Survey the globals the SIMULATION frontier touches -- from real disassembly.

An earlier byte-scan version of this was abandoned because it was dominated by
false positives: `44 24` is the SIB byte for [esp+disp], so any 4-byte window
spanning it decodes as a plausible 0x01xx2444 "address". Parsing actual
operands removes that entire class of error.

WP-01 is held back from Confirmed by "globals are unsurveyed". The answerable
question is not how many globals exist, but how many can affect simulation
state -- so the scan is restricted to the measured depth-1 sim frontier.
"""
import re, sys, collections
sys.path.insert(0, 'tools/re')
from pe_reader import PE32

SCRATCH = '/private/tmp/claude-501/-Users-olek-projects-llm-games-recoil-metal/1230bc61-a6b1-428a-b4e8-7a4ccae73b37/scratchpad'
EXE = '/Users/olek/projects/llm/input/recoil-metal/retail-fa/bin/SupremeCommander.exe'
pe = PE32(EXE)

extent = {}
for line in open('build/re-fa/exports/callgraph.tsv'):
    p = line.rstrip('\n').split('\t')
    if len(p) >= 2:
        extent[int(p[0], 16)] = int(p[1])

frontier = sorted(int(l.strip(), 16) for l in open('build/re-fa/exports/frontier_d1.clean.txt') if l.strip())
# Interval list so an address maps back to its owning frontier function.
spans = [(f, f + extent.get(f, 0)) for f in frontier]

LINE = re.compile(r'^\s*([0-9a-f]+):\s+(.*)$')
# Operands that name an absolute address: $0x... immediates and bare 0x... disps.
OPERAND = re.compile(r'(?:\$)?0x([0-9a-f]{6,8})\b')

refs = collections.defaultdict(set)
cur = None
si = 0
for line in open(f'{SCRATCH}/text.asm'):
    m = LINE.match(line)
    if not m:
        continue
    va = int(m.group(1), 16)
    # advance the span cursor; frontier and disassembly are both address-ordered
    while si < len(spans) and spans[si][1] <= va:
        si += 1
    if si >= len(spans) or va < spans[si][0]:
        continue
    cur = spans[si][0]
    insn = m.group(2)
    if insn.startswith(('call', 'j')):        # control flow: targets are code
        continue
    for hexs in OPERAND.findall(insn):
        v = int(hexs, 16)
        sec = pe.section_of(v)
        if sec and not pe.is_code(v) and sec.name in ('.data', '.rdata', '.bss'):
            refs[v].add(cur)

print(f'frontier functions: {len(frontier)}')
print(f'distinct globals referenced by the sim frontier: {len(refs)}')
shared = {g: f for g, f in refs.items() if len(f) >= 3}
print(f'referenced by 3+ frontier functions: {len(shared)}')
data_only = {g: f for g, f in refs.items() if pe.section_of(g).name == '.data'}
print(f'MUTABLE (.data) globals reachable from the sim frontier: {len(data_only)}')
print(f'  ...of those, shared by 3+: {sum(1 for g in data_only if len(refs[g]) >= 3)}')

print('\ntop shared MUTABLE globals -- candidate engine singletons:')
for g, fns in sorted(data_only.items(), key=lambda kv: -len(kv[1]))[:20]:
    print(f'  {g:#010x}  {len(fns):3d} refs')

with open('build/re-fa/exports/sim_globals.tsv', 'w') as f:
    f.write('global_va\tsection\tn_frontier_refs\treferring_funcs\n')
    for g, fns in sorted(refs.items(), key=lambda kv: -len(kv[1])):
        f.write(f'{g:08x}\t{pe.section_of(g).name}\t{len(fns)}\t{",".join(f"{x:08x}" for x in sorted(fns))}\n')
print('\nwrote build/re-fa/exports/sim_globals.tsv')
