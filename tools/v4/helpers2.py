"""Deep pass: follow the register a skipped 64.0 is loaded into, through the function.
A site is tick-related if a value multiplied by it reaches a tick helper (modff GameTick+fraction
builder) or a float->int conversion within a few instructions. Writes <dll>.deep.json."""
import sys, json, collections, pefile, capstone

CVT = ('cvttss2si', 'cvtss2si', 'roundss', 'cvttsd2si', 'cvtsd2si', 'roundsd')

def main(name, helpers):
    path = name + '.dll'
    pe = pefile.PE(path, fast_load=True); d = pe.__data__; t = pe.sections[0]
    def off(a): return a - t.VirtualAddress + t.PointerToRawData
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    # find modff-style helpers by the prologue bytes of known ones
    tickfns = set(helpers)
    text = d[t.PointerToRawData:t.PointerToRawData + t.Misc_VirtualSize]
    for h in list(helpers):
        sig = d[off(h):off(h) + 14]
        i = text.find(sig)
        while i != -1:
            tickfns.add(t.VirtualAddress + i); i = text.find(sig, i + 1)
    plan = json.load(open(path + '.plan.json'))
    out = []
    for s in plan:
        if s['act'] != 'skip' or s['const'] not in ('f_64', 'd_64') or s['kind'] != 'rip': continue
        reg = s['ops'].split(',')[0].strip()
        if not reg.startswith('xmm'): continue
        ins = list(md.disasm(d[off(s['addr']):off(s['addr']) + 3000], s['addr']))[:500]
        const_regs = {reg}; scaled = {}   # reg -> index where it became scaled
        if s['mn'] in ('mulss', 'mulsd'): scaled[reg] = 0; const_regs = set()
        why = None
        for k, i in enumerate(ins[1:], 1):
            m, o = i.mnemonic, i.op_str
            p = [x.strip() for x in o.split(',')]
            if m in ('ret', 'int3'): break
            if len(p) == 2 and m in ('mulss', 'mulsd') and p[1] in const_regs: scaled[p[0]] = k
            elif len(p) == 2 and m in ('mulss', 'mulsd') and p[0] in const_regs: scaled[p[0]] = k; const_regs.discard(p[0])
            elif len(p) == 2 and m in ('movaps', 'movss', 'movsd') and p[1] in scaled: scaled[p[0]] = k
            elif len(p) >= 1 and p[0] in const_regs and m not in ('comiss', 'ucomiss', 'comisd', 'ucomisd') and not (m.startswith('mov') and p[1] in const_regs):
                const_regs.discard(p[0])
            live = {r for r, at in scaled.items() if k - at <= 8}
            if m in CVT and len(p) == 2 and p[1] in live: why = 'cvt'; break
            if m == 'call' and o.startswith('0x') and int(o, 16) in tickfns and live & {'xmm0', 'xmm1', 'xmm2', 'xmm3'}: why = 'tick helper %s' % o; break
            if not const_regs and not live and k > 12: break
        if why: out.append({'addr': s['addr'], 'why': why})
    json.dump(out, open(path + '.deep.json', 'w'))
    print(name, 'tick helpers', sorted(hex(x) for x in tickfns))
    for x in out: print('  %x %s' % (x['addr'], x['why']))

main('server', [0x13ae3d0, 0x13aea10])
main('client', [0x15efb10])
main('engine2', [])
