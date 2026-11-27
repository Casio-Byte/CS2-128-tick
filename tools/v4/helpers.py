"""Find 64.0 sites whose scaled value goes into a helper that converts float -> int (time -> ticks).
Writes <dll>.helper.json: list of site addresses + the helper they reach."""
import sys, json, struct, bisect, collections, pefile, capstone
from capstone import x86

CVT = ('cvttss2si', 'cvtss2si', 'roundss', 'cvttsd2si', 'cvtsd2si', 'roundsd', 'cvttps2dq', 'cvtps2dq')

def main(name):
    path = name + '.dll'
    pe = pefile.PE(path, fast_load=True); d = pe.__data__; t = pe.sections[0]
    def off(a): return a - t.VirtualAddress + t.PointerToRawData
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
    cache = {}
    def helper_converts(fn, depth=0):
        """does fn (or a function it tail-jumps / calls first) convert float->int within its first ~80 insns?"""
        if fn in cache: return cache[fn]
        cache[fn] = False
        o = off(fn)
        if o is None or o < 0 or o >= len(d): return False
        n = 0; res = False
        for i in md.disasm(d[o:o + 600], fn):
            n += 1
            if i.mnemonic in CVT: res = True; break
            if i.mnemonic == 'jmp' and depth < 2 and i.op_str.startswith('0x'):
                res = helper_converts(int(i.op_str, 16), depth + 1); break
            if i.mnemonic == 'call' and depth < 1 and i.op_str.startswith('0x') and n < 12:
                if helper_converts(int(i.op_str, 16), depth + 1): res = True; break
            if i.mnemonic in ('ret', 'int3') or n > 80: break
        cache[fn] = res
        return res
    plan = json.load(open(path + '.plan.json'))
    hits = []
    for s in plan:
        if s['act'] != 'skip' or s['const'] not in ('f_64', 'd_64') or s['kind'] != 'rip': continue
        # destination register of the scaled value
        ops = s['ops'].split(',')
        reg = ops[0].strip()
        if not reg.startswith('xmm'): continue
        live = {reg}
        for a, sz, m, o in s['next'][:12]:
            parts = [p.strip() for p in o.split(',')]
            dst = parts[0] if parts else ''
            if m in ('movaps', 'movss', 'movsd') and len(parts) == 2 and parts[1] in live and parts[0].startswith('xmm'): live.add(parts[0])
            if m == 'call' and o.startswith('0x'):
                fn = int(o, 16)
                if (live & {'xmm0', 'xmm1', 'xmm2', 'xmm3'}) and helper_converts(fn):
                    hits.append({'addr': s['addr'], 'helper': fn}); break
            if m in CVT and len(parts) == 2 and parts[1] in live:
                hits.append({'addr': s['addr'], 'helper': 0}); break
            if m in ('ret', 'jmp'): break
    json.dump(hits, open(path + '.helper.json', 'w'))
    print(name, len(hits), 'sites reach a float->int helper:', collections.Counter(hex(h['helper']) for h in hits).most_common(8))

for n in sys.argv[1:]: main(n)
