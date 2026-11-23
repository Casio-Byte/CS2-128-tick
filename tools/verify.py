"""Apply work/patches.json to copies of the DLLs at each tick rate and re-disassemble every edited instruction."""
import os, json, pefile, struct, re, sys
from capstone import *
WORK = os.environ.get("CS2_WORK", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "work"))
P = json.load(open(os.path.join(WORK, "patches.json")))
md = Cs(CS_ARCH_X86, CS_MODE_64)
NK2KIND = {'f1/128': 'f_inv', 'f128': 'f_rate', 'd1/128': 'd_inv', 'd128': 'd_rate'}

def value(kind, rate):
    if kind == 'f_inv':  return struct.pack('<f', 1.0 / rate)
    if kind == 'f_rate': return struct.pack('<f', float(rate))
    if kind == 'd_inv':  return struct.pack('<d', 1.0 / rate)
    if kind == 'd_rate': return struct.pack('<d', float(rate))

def apply(raw, patches, tier, rate):
    raw = bytearray(raw)
    for p in patches:
        if p['tier'] == 'aggressive' and tier != 'aggressive': continue
        o = p['off']; old = bytes.fromhex(p['old'])
        assert raw[o:o + len(old)] == old, p['what']
        new = bytes.fromhex(p['new']) if p['kind'] == 'fixed' else value(p['kind'], rate)
        raw[o:o + len(new)] = new
    return bytes(raw)

fails = 0
for rate in (128, 256):
    for tier in ('safe', 'aggressive'):
        for fn, info in P['files'].items():
            raw = open(os.path.join(WORK, fn), 'rb').read()
            out = apply(raw, info['patches'], tier, rate)
            open(os.path.join(WORK, f'out_{rate}_{tier}_{fn}'), 'wb').write(out)
            pe = pefile.PE(data=out)
            bad = n = 0
            for p in info['patches']:
                if p['tier'] == 'aggressive' and tier != 'aggressive': continue
                m = re.search(r'@rva (0x[0-9a-f]+)', p['what'])
                if not m or p['what'].startswith('const slot'): continue
                a = int(m.group(1), 16); n += 1
                ins = next(md.disasm(pe.get_data(a, 16), a))
                if p['what'].startswith('imm'):
                    ok = ins.op_str.endswith(hex(struct.unpack('<I', value('f_inv', rate))[0]))
                else:
                    mm = re.search(r'rip ([+-]) (0x[0-9a-f]+|\d+)\]', ins.op_str)
                    t = ins.address + ins.size + int(mm.group(2), 0) * (1 if mm.group(1) == '+' else -1)
                    want = value(NK2KIND[p['what'].split('-> ')[-1]], rate)
                    ok = pe.get_data(t, len(want)) == want and ins.mnemonic == p['what'].split()[0]
                if not ok: bad += 1; print('BAD', rate, tier, fn, p['what'], '->', ins.mnemonic, ins.op_str)
            fails += bad
            print(f'{rate:3} tick  {tier:10} {fn:12} {n} sites checked, {bad} bad')
sys.exit(1 if fails else 0)
