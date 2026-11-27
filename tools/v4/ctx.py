"""Collect instruction context around every 1/64 / 64.0 use and classify it."""
import os, sys, struct, collections, json, pefile, capstone
from capstone import x86
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scan import PAT, chunked

W = 14

def load(path):
    pe = pefile.PE(path, fast_load=True)
    data = pe.__data__
    secs = [(s.Name.rstrip(b'\0').decode(), s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData, s.Characteristics) for s in pe.sections]
    return pe, data, secs

def consts(data, secs):
    const = {}
    for name, va, vsz, raw, rsz, ch in secs:
        if ch & 0x20000000: continue
        blob = data[raw:raw + min(vsz, rsz)]
        for k, p in PAT.items():
            i = blob.find(p)
            while i != -1:
                if i % len(p) == 0: const[va + i] = k
                i = blob.find(p, i + 1)
    return const

def collect(path):
    pe, data, secs = load(path)
    const = consts(data, secs)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True; md.skipdata = True
    sites = []
    prev = collections.deque(maxlen=W)
    pending = []   # sites waiting for following context
    for name, va, vsz, raw, rsz, ch in secs:
        if not ch & 0x20000000: continue
        code = data[raw:raw + min(vsz, rsz)]
        for ins in chunked(md, code, va):
            t = (ins.address, ins.size, ins.mnemonic, ins.op_str)
            for p in pending:
                if len(p['next']) < W: p['next'].append(t)
            pending = [p for p in pending if len(p['next']) < W]
            hit = None
            if 'rip' in ins.op_str:
                for op in ins.operands:
                    if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
                        tgt = ins.address + ins.size + op.mem.disp
                        if tgt in const:
                            hit = dict(kind='rip', const=const[tgt], target=tgt, size=op.size,
                                       disp_off=ins.disp_offset if hasattr(ins, 'disp_offset') else None)
            if ins.mnemonic.startswith('mov') and ('0x3c800000' in ins.op_str or '0x42800000' in ins.op_str):
                hit = dict(kind='imm', const='f_inv' if '0x3c8' in ins.op_str else 'f_64')
            if hit:
                hit.update(addr=ins.address, size_ins=ins.size, mn=ins.mnemonic, ops=ins.op_str,
                           bytes=ins.bytes.hex(), prev=list(prev), next=[])
                sites.append(hit); pending.append(hit)
            prev.append(t)
    return sites

CVT_TO_INT = ('cvttss2si', 'cvtss2si', 'cvttsd2si', 'cvtsd2si', 'roundss', 'roundsd')
CVT_TO_FLT = ('cvtsi2ss', 'cvtsi2sd', 'cvtdq2ps', 'cvtdq2pd')

def classify(s):
    nx = [m for _, _, m, _ in s['next'][:8]]
    pv = [m for _, _, m, _ in s['prev'][-8:]]
    c = s['const']
    if s['kind'] == 'imm':
        return 'imm'
    if c in ('f_64', 'd_64'):
        if any(m in CVT_TO_INT for m in nx): return 'time2tick'
        if s['mn'] in ('divss', 'divsd') and any(m in CVT_TO_FLT for m in pv): return 'tick2time_div'
    if c in ('f_inv', 'd_inv'):
        if any(m in CVT_TO_FLT for m in pv) and s['mn'] in ('mulss', 'mulsd'): return 'tick2time'
        if any(m in CVT_TO_INT for m in nx) and s['mn'] in ('divss', 'divsd'): return 'time2tick_div'
    return 'other'

if __name__ == '__main__':
    path = sys.argv[1]
    sites = collect(path)
    for s in sites: s['cls'] = classify(s)
    json.dump(sites, open(path + '.sites.json', 'w'))
    print(path, len(sites), collections.Counter(s['cls'] for s in sites))
    print(collections.Counter((s['cls'], s['const'], s['mn']) for s in sites).most_common(40))
