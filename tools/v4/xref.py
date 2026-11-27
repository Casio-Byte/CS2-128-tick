"""Find code that references given strings; dump the enclosing function (via .pdata)."""
import os, sys, struct, bisect, pefile, capstone
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scan import chunked

def main(path, needles, dump=True, maxlines=120):
    pe = pefile.PE(path, fast_load=True); data = pe.__data__
    secs = [(s.Name.rstrip(b'\0').decode(), s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData, s.Characteristics) for s in pe.sections]
    def rva2off(r):
        for n, va, vsz, raw, rsz, ch in secs:
            if va <= r < va + vsz: return raw + r - va
    targets = {}
    for n, va, vsz, raw, rsz, ch in secs:
        if n != '.rdata': continue
        blob = data[raw:raw + rsz]
        for nd in needles:
            i = blob.find(nd.encode())
            while i != -1:
                if i == 0 or blob[i - 1] == 0: targets[va + i] = nd
                i = blob.find(nd.encode(), i + 1)
    # .pdata functions
    pd = [s for s in secs if s[0] == '.pdata'][0]
    funcs = []
    for k in range(0, pd[2] // 12):
        b, e, u = struct.unpack_from('<III', data, pd[3] + 12 * k)
        if b: funcs.append((b, e))
    funcs.sort(); starts = [f[0] for f in funcs]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.skipdata = True
    hits = []
    for n, va, vsz, raw, rsz, ch in secs:
        if not ch & 0x20000000: continue
        code = data[raw:raw + min(vsz, rsz)]
        for ins in chunked(md, code, va):
            if ins.mnemonic == 'lea' and 'rip' in ins.op_str:
                disp = int(ins.op_str.split('rip ')[1].split(']')[0].replace(' ', '').replace('+', ''), 16) if '+' in ins.op_str or '- ' in ins.op_str else 0
                if '- 0x' in ins.op_str: disp = -int(ins.op_str.split('- ')[1].split(']')[0], 16)
                t = ins.address + ins.size + disp
                if t in targets: hits.append((ins.address, targets[t]))
    for a, nd in hits:
        i = bisect.bisect_right(starts, a) - 1
        b, e = funcs[i]
        print('### xref %x -> %r   func %x-%x' % (a, nd, b, e))
        if dump:
            md2 = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
            off = rva2off(b)
            for k, ins in enumerate(md2.disasm(data[off:off + (e - b)], b)):
                if k >= maxlines: print('   ...'); break
                mark = '>>' if ins.address == a else '  '
                print('%s %x %s %s' % (mark, ins.address, ins.mnemonic, ins.op_str))

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2:])
