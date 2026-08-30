"""Find literal 4-byte VA references to a target address, section by section.

LIMITATION -- THIS IS A DATA-REFERENCE TOOL. It matches absolute 4-byte VAs only
and is blind to `E8 rel32` calls, so it will report "0 refs" for a function that
plainly has callers. Two functions did exactly that in session 14. Use
`callgraph.tsv` or an explicit E8 scan for call references; use this for vtables,
strings, globals and other data.

Relocations are stripped and the image base is fixed, so an absolute address
appears verbatim in the file bytes. A byte-scan is therefore an exact xref
search with no disassembly involved.
"""
import sys, struct
sys.path.insert(0, 'tools/re')
from pe_reader import PE32

EXE = '/Users/olek/projects/llm/input/recoil-metal/retail-fa/bin/SupremeCommander.exe'
pe = PE32(EXE)

targets = [int(a, 16) for a in sys.argv[1:]]
for t in targets:
    pat = struct.pack('<I', t)
    hits = []
    for s in pe.sections:
        blob = pe.read(s.virtual_address, s.virtual_size)
        if not blob:
            continue
        start = 0
        while True:
            i = blob.find(pat, start)
            if i < 0:
                break
            hits.append((s.name, s.virtual_address + i))
            start = i + 1
    print(f'{t:#010x}: {len(hits)} refs')
    for name, va in hits[:16]:
        print(f'    {name:8s} {va:#010x}')
