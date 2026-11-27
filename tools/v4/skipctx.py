"""For every skipped site: enclosing function + strings it references."""
import sys, json, struct, bisect, re, pefile, capstone

def main(name):
    path = name + '.dll'
    pe = pefile.PE(path, fast_load=True); d = pe.__data__
    secs = [(s.Name.rstrip(b'\0').decode(), s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData) for s in pe.sections]
    def off(r):
        for n, va, vs, raw, rs in secs:
            if va <= r < va + vs: return raw + r - va
    rd = [s for s in secs if s[0] == '.rdata'][0]
    pd = [s for s in secs if s[0] == '.pdata'][0]
    fs = sorted(f for f in (struct.unpack_from('<III', d, pd[3] + 12 * k) for k in range(pd[2] // 12)) if f[0])
    st = [f[0] for f in fs]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
    plan = json.load(open(path + '.plan.json'))
    out = []
    for s in plan:
        if s['act'] != 'skip': continue
        i = bisect.bisect_right(st, s['addr']) - 1
        b, e = fs[i][0], fs[i][1]
        # walk back to the primary function if this is a chained chunk: just use chunk + neighbours
        lo = max(0, i - 3); hi = min(len(fs), i + 4)
        strs = []
        for k in range(lo, hi):
            fb, fe = fs[k][0], fs[k][1]
            o = off(fb)
            for ins in md.disasm(d[o:o + (fe - fb)], fb):
                if 'rip' not in ins.op_str: continue
                for op in ins.operands:
                    if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                        t = ins.address + ins.size + op.mem.disp
                        if rd[1] <= t < rd[1] + rd[2]:
                            so = off(t); m = re.match(rb'[\x20-\x7e]{4,80}', d[so:so + 80])
                            if m and d[so + len(m.group())] == 0:
                                v = m.group().decode()
                                if v not in strs: strs.append(v)
        ctx = '; '.join('%s %s' % (m, o) for _, _, m, o in s['next'][:5])
        out.append('%x %-6s %-8s %s | %s\n     strings: %s' % (s['addr'], s['const'], s['mn'], s['why'], ctx, ' || '.join(strs[:8])))
    open(name + '.skipped.txt', 'w').write('\n'.join(out) + '\n')
    print(name, len(out))

for n in sys.argv[1:]: main(n)
