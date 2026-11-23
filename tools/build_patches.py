import os
WORK = os.environ.get("CS2_WORK", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "work"))
import json,pefile,struct,hashlib,sys
FILES={'engine2.dll':'game/bin/win64/engine2.dll','server.dll':'game/csgo/bin/win64/server.dll','client.dll':'game/csgo/bin/win64/client.dll'}
SCALAR_OK={'movss','mulss','addss','subss','divss','comiss','ucomiss','movsd','mulsd','addsd','subsd','divsd','comisd','ucomisd','maxss','minss'}
NEWK={'f1/64':'f1/128','f64':'f128','d1/64':'d1/128','d64':'d128'}
CAVE_VALS=[('f1/128',struct.pack('<f',1/128)),('f128',struct.pack('<f',128.0)),('d1/128',struct.pack('<d',1/128)),('d128',struct.pack('<d',128.0))]
EXTRA_SAFE={'engine2.dll':{0x120cec,0x2116a7}}   # fixed-value check; dedicated-server frame-rate target   # client's "compiled with fixed value" compare
out={'files':{}}
for fn,rel in FILES.items():
    pe=pefile.PE(WORK+'/'+fn,fast_load=True)
    raw=open(WORK+'/'+fn,'rb').read()
    text=[s for s in pe.sections if s.Name.startswith(b'.text')][0]
    tb=text.get_data()
    import re
    m=re.search(rb'\xcc{64,}',tb)
    cave={}; patches=[]
    if m:
        start=(m.start()+8+15)&~15
        cave_rva=text.VirtualAddress+start; off=0
        for k,v in CAVE_VALS:
            a=cave_rva+off; cave[k]=a
            fo=pe.get_offset_from_rva(a)
            assert raw[fo:fo+len(v)]==b'\xcc'*len(v)
            patches.append({'tier':'safe','what':f'cave const {k}','off':fo,'old':raw[fo:fo+len(v)].hex(),'new':v.hex()})
            off+=8
    else:
        cave_rva=None
        dd=json.load(open(f'{WORK}/{fn[:-4]}.json'))
        for a,k in dd['consts'].items():
            if k in ('f1/128','f128','d1/128','d128') and k not in cave: cave[k]=int(a,16)
        # prefer pooled ones near the 1/64 pool
        if fn=='engine2.dll': cave.update({'f1/128':0x620a30,'d1/128':0x620de0,'d128':0x6324f2})
        print(fn,'using existing constants',{k:hex(v) for k,v in cave.items()})
    d=json.load(open(f'{WORK}/{fn[:-4]}.json'))
    TICKFUNCS={x['func'] for x in d['hits'] if (x.get('cls') or '').startswith('tick:') or x.get('cls')=='unknown:inv'}
    stats={}
    for h in d['hits']:
        k=h['kind']; c=h.get('cls')
        if k.startswith('imm '):
            base=k[4:]
            if base!='f1/64': continue
            tier='safe'
            ib=bytes.fromhex(h['bytes']); assert ib[-4:]==struct.pack('<f',1/64)
            fo=pe.get_offset_from_rva(h['addr'])+len(ib)-4
            patches.append({'tier':tier,'what':f"imm {h['mn']} {h['op']} @rva {h['addr']:#x}",'off':fo,'old':ib[-4:].hex(),'new':struct.pack('<f',1/128).hex()})
            stats[tier]=stats.get(tier,0)+1; continue
        if not c or h['mn'] not in SCALAR_OK or h['disp_off'] is None or h['disp_off']<0: continue
        if c.startswith('tick:') or h['addr'] in EXTRA_SAFE.get(fn,()) or c in ('unknown:inv','ambig:inv_then_toint'): tier='safe'
        elif c=='unknown:rate' and h['func'] in TICKFUNCS: tier='aggressive'
        else: continue
        nk=NEWK[k]
        if nk not in cave: print('  SKIP no',nk,hex(h['addr'])); continue
        ins_end=h['addr']+h['size']
        newdisp=cave[nk]-ins_end; olddisp=h['target']-ins_end
        fo=pe.get_offset_from_rva(h['addr'])+h['disp_off']
        assert raw[fo:fo+4]==struct.pack('<i',olddisp)
        patches.append({'tier':tier,'what':f"{h['mn']} {h['op']} @rva {h['addr']:#x} [{c}] -> {nk}",'off':fo,'old':struct.pack('<i',olddisp).hex(),'new':struct.pack('<i',newdisp).hex()})
        stats[tier]=stats.get(tier,0)+1
    out['files'][fn]={'path':rel,'sha256':hashlib.sha256(raw).hexdigest(),'size':len(raw),'cave_rva':hex(cave_rva) if cave_rva else None,'patches':patches}
    print(fn,'cave',cave_rva,stats,'total',len(patches))
json.dump(out,open(WORK+'/patches.json','w'),indent=1)
