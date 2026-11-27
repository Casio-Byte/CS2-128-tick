"""Independently verify a patched DLL against its original."""
import sys, json, struct, pefile, capstone
from capstone import x86

def main(name, rate):
    orig = open(name + '.dll', 'rb').read()
    pat = open('%s.%d.dll' % (name, rate), 'rb').read()
    assert len(orig) == len(pat)
    plan = [s for s in json.load(open(name + '.dll.plan.json')) if s['act'] == 'patch']
    pe = pefile.PE(name + '.dll', fast_load=True); t = pe.sections[0]
    def off(rva): return rva - t.VirtualAddress + t.PointerToRawData
    def rva(o): return o - t.PointerToRawData + t.VirtualAddress
    changed = [i for i in range(len(orig)) if orig[i] != pat[i]]
    allowed = set()
    for s in plan:
        o = off(s['addr']); allowed.update(range(o, o + s['size_ins']))
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
    expect = {'f_64': ('<f', float(rate), 4), 'f_inv': ('<f', 1 / rate, 4), 'd_64': ('<d', float(rate), 8), 'd_inv': ('<d', 1 / rate, 8)}
    pads = set(); bad = 0
    for s in plan:
        o = off(s['addr']); L = s['size_ins']
        a = next(md.disasm(orig[o:o + L], s['addr'])); b = next(md.disasm(pat[o:o + L], s['addr']))
        if a.size != b.size or a.mnemonic != b.mnemonic:
            print('DECODE CHANGE', hex(s['addr']), a.mnemonic, b.mnemonic); bad += 1; continue
        fmt, val, sz = expect[s['const']]
        if s['kind'] == 'rip':
            mem = [op for op in b.operands if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP][0]
            tgt = b.address + b.size + mem.mem.disp
            pads.update(range(off(tgt), off(tgt) + sz))
            got = struct.unpack_from(fmt, pat, off(tgt))[0]
        else:
            imm = [op for op in b.operands if op.type == x86.X86_OP_IMM][0].imm & 0xffffffff
            got = struct.unpack('<f', struct.pack('<I', imm))[0]; fmt = '<f'
        want = struct.unpack(fmt, struct.pack(fmt, val))[0]
        if got != want:
            print('VALUE', hex(s['addr']), s['mn'], got, want); bad += 1
    stray = [i for i in changed if i not in allowed and i not in pads]
    # every changed padding byte must have been int3 before
    padbad = [i for i in changed if i in pads and orig[i] != 0xCC]
    print('%s %d: %d sites, %d bytes changed, stray %d, non-int3 pad %d, bad %d' %
          (name, rate, len(plan), len(changed), len(stray), len(padbad), bad))
    return bad + len(stray) + len(padbad)

if __name__ == '__main__':
    r = 0
    for n in ('engine2', 'server', 'client'):
        for rate in (128, 256): r += main(n, rate)
    sys.exit(1 if r else 0)
