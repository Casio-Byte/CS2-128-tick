"""Decide which sites to patch. Writes <dll>.plan.json and prints a breakdown."""
import os, sys, json, struct, bisect, collections, pefile

CONF = {'time2tick', 'tick2time', 'time2tick_div', 'tick2time_div'}

def funcs_of(path):
    pe = pefile.PE(path, fast_load=True); d = pe.__data__
    pd = [s for s in pe.sections if s.Name.startswith(b'.pdata')][0]
    raw = pd.PointerToRawData
    out = []
    for k in range(pd.Misc_VirtualSize // 12):
        b, e, u = struct.unpack_from('<III', d, raw + 12 * k)
        if b: out.append((b, e))
    out.sort()
    return out

def decide(path):
    sites = json.load(open(path + '.sites.json'))
    fs = funcs_of(path); starts = [f[0] for f in fs]
    def fid(a):
        i = bisect.bisect_right(starts, a) - 1
        return fs[i][0] if i >= 0 and fs[i][0] <= a < fs[i][1] else None
    conf_funcs = set()
    for s in sites:
        s['func'] = fid(s['addr'])
        if s['cls'] in CONF: conf_funcs.add(s['func'])
    for s in sites:
        m, c = s['mn'], s['const']
        nx = s['next']
        if s['kind'] == 'rip' and s.get('size') == 16 or m in ('movdqa', 'movaps', 'movups', 'movdqu', 'lea'):
            s['act'], s['why'] = 'skip', 'vector/address use'
        elif s['kind'] == 'imm':
            # mov dword [mem], 0x3C800000 -> interval store. 0x42800000 imm stores: only with tick context.
            if c == 'f_inv' and m == 'mov' and s['ops'].startswith('dword ptr'):
                s['act'], s['why'] = 'patch', 'interval store'
            elif s['func'] in conf_funcs:
                s['act'], s['why'] = 'patch', 'imm in tick function'
            else:
                s['act'], s['why'] = 'skip', 'imm, no tick context'
        elif s['cls'] in CONF:
            s['act'], s['why'] = 'patch', s['cls']
        elif c in ('f_inv', 'd_inv') and m in ('movss', 'movsd') and nx and nx[0][2] == 'ret':
            s['act'], s['why'] = 'patch', 'getter returns interval'
        elif s['func'] in conf_funcs:
            s['act'], s['why'] = 'patch', 'same function as tick math'
        elif c in ('f_inv', 'd_inv'):
            s['act'], s['why'] = 'patch', '1/64 scalar (interval)'
        else:
            s['act'], s['why'] = 'skip', '64.0 without tick context'
    # reviewed overrides (tools/overrides.json): manual audit of every skipped site
    ov = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'overrides.json'))).get(path.replace('.dll', ''), {})
    for s in sites:
        k = '%x' % s['addr']
        if k in ov.get('patch', {}): s['act'], s['why'] = 'patch', 'reviewed: ' + ov['patch'][k]
        if k in ov.get('skip', {}): s['act'], s['why'] = 'skip', 'reviewed: ' + ov['skip'][k]
    json.dump(sites, open(path + '.plan.json', 'w'))
    print(path, collections.Counter(s['act'] for s in sites))
    for k, v in collections.Counter((s['act'], s['why']) for s in sites).most_common(): print('   ', k, v)
    return sites

if __name__ == '__main__':
    for p in sys.argv[1:]: decide(p)
