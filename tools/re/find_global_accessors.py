"""Find global-address accessor functions: `mov eax, imm32; ret`.

MSVC compiles a function that returns the address of a global -- a Meyers
singleton getter, a type-info getter, a static table accessor -- to exactly
two instructions: B8 <imm32> C3. Scanning .text for that pair enumerates the
engine's globals *that something bothered to expose*, which is a far better
filter than "every address that appears in .data".

The immediate must land inside a data section to count; a code address would
mean a function pointer, not a global.
"""
import sys, struct, collections
sys.path.insert(0, 'tools/re')
from pe_reader import PE32

EXE = '/Users/olek/projects/llm/input/recoil-metal/retail-fa/bin/SupremeCommander.exe'
pe = PE32(EXE)

text = next(s for s in pe.sections if s.name.startswith('.text'))
blob = pe.read(text.virtual_address, text.virtual_size)

hits = []
i = 0
while True:
    i = blob.find(b'\xb8', i)
    if i < 0 or i + 6 > len(blob):
        break
    if blob[i + 5] == 0xC3:                      # B8 imm32 C3
        imm = struct.unpack('<I', blob[i + 1:i + 5])[0]
        sec = pe.section_of(imm)
        if sec and not pe.is_code(imm):
            hits.append((text.virtual_address + i, imm, sec.name))
    i += 1

print(f'{len(hits)} `mov eax, <global>; ret` accessors')
by_sec = collections.Counter(h[2] for h in hits)
print('by section:', dict(by_sec))
uniq = sorted({h[1] for h in hits})
print(f'{len(uniq)} distinct globals exposed this way')
print(f'range: {uniq[0]:#010x} .. {uniq[-1]:#010x}')
with open('build/re-fa/exports/global_accessors.tsv', 'w') as f:
    f.write('accessor_va\tglobal_va\tsection\n')
    for va, imm, sec in hits:
        f.write(f'{va:08x}\t{imm:08x}\t{sec}\n')
print('wrote build/re-fa/exports/global_accessors.tsv')
