"""Map 1/64 and 64.0 constants and their uses in a CS2 x64 DLL."""
import sys, struct, collections, pefile, capstone
from capstone import x86

PAT = {
    'f_inv': struct.pack('<f', 1/64),   # 0x3C800000
    'f_64':  struct.pack('<f', 64.0),   # 0x42800000
    'd_inv': struct.pack('<d', 1/64),
    'd_64':  struct.pack('<d', 64.0),
}

def chunked(md, code, va, step=1 << 16):
    off = 0
    n = len(code)
    while off < n:
        end = min(n, off + step + 32)
        last = off
        for ins in md.disasm(code[off:end], va + off):
            if ins.address - va >= off + step:
                break
            last = ins.address - va + ins.size
            yield ins
        off = max(last, off + 1)

def main(path):
    pe = pefile.PE(path, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    data = pe.__data__
    secs = []
    for s in pe.sections:
        name = s.Name.rstrip(b'\0').decode()
        secs.append((name, s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData, s.Characteristics))
    print(path, 'imagebase %x' % base)
    for s in secs: print('  %-8s rva %08x vsz %08x raw %08x rsz %08x ch %08x' % s)
    # constants in non-exec sections
    const = {}
    for name, va, vsz, raw, rsz, ch in secs:
        if ch & 0x20000000: continue
        blob = data[raw:raw + min(vsz, rsz)]
        for k, p in PAT.items():
            step = len(p)
            i = blob.find(p)
            while i != -1:
                if i % step == 0:
                    const[va + i] = k
                i = blob.find(p, i + 1)
    cnt = collections.Counter(const.values())
    print('  constants:', dict(cnt))
    text = [s for s in secs if s[5] & 0x20000000]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    md.skipdata = True
    refs = collections.defaultdict(list)      # const rva -> [(insn rva, mnemonic, opstr)]
    vecrefs = collections.Counter()
    imms = []
    mn = collections.Counter()
    for name, va, vsz, raw, rsz, ch in text:
        code = data[raw:raw + min(vsz, rsz)]
        for ins in chunked(md, code, va):
            if ins.mnemonic in ('int3', 'nop', '.byte'): continue
            ops = ins.op_str
            if 'rip' in ops:
                for op in ins.operands:
                    if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
                        tgt = ins.address + ins.size + op.mem.disp
                        if tgt in const:
                            refs[tgt].append((ins.address, ins.mnemonic, ops))
                            mn[(const[tgt], ins.mnemonic)] += 1
                        else:
                            # 16-byte vector constant overlapping a pattern
                            for d in (4, 8, 12):
                                if tgt + d in const and op.size == 16:
                                    vecrefs[(const[tgt + d], ins.mnemonic)] += 1
            if ('0x3c800000' in ops or '0x42800000' in ops) and ins.mnemonic.startswith('mov'):
                imms.append((ins.address, ins.mnemonic, ops))
    print('  rip refs by (const, mnemonic):')
    for k, v in sorted(mn.items(), key=lambda x: -x[1]): print('    ', k, v)
    print('  vector refs overlapping:', dict(vecrefs))
    ic = collections.Counter((('inv' if '0x3c8' in o else '64'), m, o.split(',')[0].split(' ')[0]) for a, m, o in imms)
    print('  mov imms:', len(imms))
    for k, v in ic.most_common(): print('    ', k, v)
    return const, refs, imms

if __name__ == '__main__':
    main(sys.argv[1])
