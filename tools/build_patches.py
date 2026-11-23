import os
WORK = os.environ.get("CS2_WORK", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "work"))
import json,pefile,struct,hashlib,sys
FILES={'engine2.dll':'game/bin/win64/engine2.dll','server.dll':'game/csgo/bin/win64/server.dll','client.dll':'game/csgo/bin/win64/client.dll'}
SCALAR_OK={'movss','mulss','addss','subss','divss','comiss','ucomiss','movsd','mulsd','addsd','subsd','divsd','comisd','ucomisd','maxss','minss'}
NEWK={'f1/64':'f1/128','f64':'f128','d1/64':'d1/128','d64':'d128'}
CAVE_VALS=[('f1/128',struct.pack('<f',1/128)),('f128',struct.pack('<f',128.0)),('d1/128',struct.pack('<d',1/128)),('d128',struct.pack('<d',128.0))]
EXTRA_SAFE={'engine2.dll':{0x120cec,0x2116a7}}   # BUILD-SPECIFIC: client's "compiled with fixed value" check; dedicated-server frame-rate target
out={'files':{}}
for fn,rel in FILES.items():
    pe=pefile.PE(WORK+'/'+fn,fast_load=True)
    raw=open(WORK+'/'+fn,'rb').read()
    text=[s for s in pe.sections if s.Name.startswith(b'.text')][0]
    tb=text.get_data()
    import re
    # Constant slots: our own 8-aligned slots inside int3 padding between functions (never executed).
    # Their values depend on the chosen tick rate and are written by the app (kind f_inv/f_rate/d_inv/d_rate).
    KINDS=[('f1/128','f_inv',4),('f128','f_rate',4),('d1/128','d_inv',8),('d128','d_rate',8)]
    cave={}; patches=[]; slots=[]
    big=re.search(rb'\xcc{64,}',tb)
    if big:
        start=(big.start()+8+15)&~15
        slots=[text.VirtualAddress+start+8*i for i in range(4)]
    else:
        for m in re.finditer(rb'\xcc{12,}',tb):
            a=(m.start()+2+7)&~7          # 8-aligned, at least 2 int3 kept before
            if a+8 <= m.end()-2: slots.append(text.VirtualAddress+a)   # and 2 after
            if len(slots)==4: break
    assert len(slots)==4, fn
    for (k,kind,n),a in zip(KINDS,slots):
        cave[k]=a; fo=pe.get_offset_from_rva(a)
        assert raw[fo:fo+n]==b'\xcc'*n
        v=struct.pack('<f' if n==4 else '<d', (1/128 if 'inv' in kind else 128.0))
        patches.append({'tier':'safe','kind':kind,'what':f'const slot {kind} @rva {a:#x}','off':fo,'old':raw[fo:fo+n].hex(),'new':v.hex()})
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
            patches.append({'tier':tier,'kind':'f_inv','what':f"imm {h['mn']} {h['op']} @rva {h['addr']:#x}",'off':fo,'old':ib[-4:].hex(),'new':struct.pack('<f',1/128).hex()})
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
        patches.append({'tier':tier,'kind':'fixed','what':f"{h['mn']} {h['op']} @rva {h['addr']:#x} [{c}] -> {nk}",'off':fo,'old':struct.pack('<i',olddisp).hex(),'new':struct.pack('<i',newdisp).hex()})
        stats[tier]=stats.get(tier,0)+1
    out['files'][fn]={'path':rel,'sha256':hashlib.sha256(raw).hexdigest(),'size':len(raw),'slots':[hex(x) for x in slots],'patches':patches}
    print(fn,'slots',[hex(x) for x in slots],stats,'total',len(patches))
json.dump(out,open(WORK+'/patches.json','w'),indent=1)
