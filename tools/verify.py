import os
WORK = os.environ.get("CS2_WORK", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "work"))
import json,pefile,struct,sys,re,shutil
from capstone import *
P=json.load(open(WORK+'/patches.json'))
md=Cs(CS_ARCH_X86,CS_MODE_64)
want={'f1/128':('<f',1/128),'f128':('<f',128.0),'d1/128':('<d',1/128),'d128':('<d',128.0)}
for tier in ('safe','aggressive'):
  for fn,info in P['files'].items():
    raw=bytearray(open(WORK+'/'+fn,'rb').read())
    ps=[p for p in info['patches'] if p['tier']=='safe' or tier=='aggressive']
    for p in ps:
        o=p['off']; old=bytes.fromhex(p['old']); new=bytes.fromhex(p['new'])
        assert raw[o:o+len(old)]==old,(fn,p)
        raw[o:o+len(new)]=new
    open(f'{WORK}/out_{tier}_{fn}','wb').write(raw)
    pe=pefile.PE(data=bytes(raw))
    bad=0
    for p in ps:
        m=re.search(r'@rva (0x[0-9a-f]+)',p['what'])
        if not m: continue
        a=int(m.group(1),16)
        ins=next(md.disasm(pe.get_data(a,16),a))
        if p['what'].startswith('imm'):
            ok=ins.op_str.endswith('0x3c000000')
        else:
            mm=re.search(r'rip ([+-]) (0x[0-9a-f]+|\d+)\]',ins.op_str)
            t=ins.address+ins.size+int(mm.group(2),0)*(1 if mm.group(1)=='+' else -1)
            nk=p['what'].split('-> ')[-1]; f,v=want[nk]
            sec=pe.get_section_by_rva(t)
            ok=abs(struct.unpack(f,pe.get_data(t,struct.calcsize(f)))[0]-v)<1e-12 and not (sec.Characteristics&0x80000000)
            orig=p['what'].split(' @')[0]; ok=ok and ins.mnemonic==orig.split()[0]
        if not ok: bad+=1; print('BAD',fn,p['what'],ins.mnemonic,ins.op_str)
    print(tier,fn,'patched',len(ps),'bad',bad,'pe ok',pe.FILE_HEADER.Machine==0x8664)
