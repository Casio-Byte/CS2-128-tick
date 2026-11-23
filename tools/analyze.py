import sys,pefile,json,re,struct,collections
from capstone import *
path,out=sys.argv[1],sys.argv[2]
pe=pefile.PE(path,fast_load=True)
pats={'f1/64':struct.pack('<f',1/64),'f64':struct.pack('<f',64.0),'d1/64':struct.pack('<d',1/64),'d64':struct.pack('<d',64.0),
      'f1/128':struct.pack('<f',1/128),'f128':struct.pack('<f',128.0),'d1/128':struct.pack('<d',1/128),'d128':struct.pack('<d',128.0)}
consts={}
for s in pe.sections:
    if s.Name.startswith(b'.text') or not (s.Characteristics & 0x40000000): continue  # readable data
    if s.Characteristics & 0x80000000: continue  # skip writable (.data)
    b=s.get_data()
    for k,p in pats.items():
        al=len(p); i=b.find(p)
        while i!=-1:
            if i%al==0: consts[s.VirtualAddress+i]=k
            i=b.find(p,i+1)
text=[s for s in pe.sections if s.Name.startswith(b'.text')][0]
code=text.get_data(); tva=text.VirtualAddress
md=Cs(CS_ARCH_X86,CS_MODE_64); md.skipdata=True
rx=re.compile(r'rip ([+-]) (0x[0-9a-f]+|\d+)\]')
window=collections.deque(maxlen=6); pending=[]; hits=[]; fs=tva; p3=False
IMM={'0x3c800000':'f1/64','0x42800000':'f64'}
for i in md.disasm(code,tva):
    rec=(i.address,i.size,i.mnemonic,i.op_str)
    for h in pending:
        h['after'].append(rec)
    pending=[h for h in pending if len(h['after'])<40]
    if i.mnemonic=='int3': p3=True
    elif p3: fs=i.address; p3=False
    k=None; disp_off=None
    if 'rip' in i.op_str:
        m=rx.search(i.op_str)
        if m:
            d=int(m.group(2),0)*(1 if m.group(1)=='+' else -1)
            t=i.address+i.size+d
            if t in consts:
                k=consts[t]; disp_off=bytes(i.bytes).find(struct.pack('<i',d))
    elif i.mnemonic=='mov':
        v=i.op_str.split(', ')[-1]
        if v in IMM: k='imm '+IMM[v]
    if k:
        h={'addr':i.address,'size':i.size,'mn':i.mnemonic,'op':i.op_str,'kind':k,'func':fs,'disp_off':disp_off,
           'target':(i.address+i.size+d) if disp_off is not None else None,
           'bytes':i.bytes.hex(),'before':list(window),'after':[]}
        hits.append(h); pending.append(h)
    window.append(rec)
json.dump({'consts':{hex(a):k for a,k in consts.items()},'hits':hits},open(out,'w'))
c=collections.Counter(h['kind'] for h in hits); print(path,dict(c))
print('const locations',{k:[hex(a) for a,v in consts.items() if v==k] for k in set(consts.values())})
