import json,sys,collections,re
INT2F=('cvtsi2ss','cvtdq2ps','cvtsi2sd','cvtdq2pd')
F2INT=('cvttss2si','cvtss2si','cvttsd2si','cvtsd2si')
XR=re.compile(r'\bxmm\d+\b')
def dest(op): return op.split(',')[0].strip()
def regs(op): return XR.findall(op)
def flows_to_int(start_reg, after, limit=30):
    """follow value in start_reg through mov/add/sub/mul copies until converted to int"""
    live={start_reg}
    for a in after[:limit]:
        mn,op=a[2],a[3]
        rs=regs(op)
        if mn in F2INT and rs and rs[-1] in live: return True
        if mn=='ret' or mn.startswith('j') and mn!='jmp': pass
        if mn=='ret': return False
        if not rs: continue
        d=dest(op)
        srcs=rs[1:] if d.startswith('xmm') else rs
        if mn in('movss','movsd','movaps','movups','movd','movq') and d.startswith('xmm'):
            if any(s in live for s in srcs): live.add(d)
            elif d in live: live.discard(d)
        elif mn in('addss','subss','mulss','addsd','subsd','mulsd','roundss','roundsd') and d.startswith('xmm'):
            if any(s in live for s in srcs): live.add(d)
        if not live: return False
    return False
def uses_reg_then_int(reg, after, limit=40):
    """constant loaded into reg; any later mul X,reg whose X flows to int?"""
    for i,a in enumerate(after[:limit]):
        mn,op=a[2],a[3]; rs=regs(op)
        if mn in('mulss','mulsd') and len(rs)>=2 and rs[1]==reg:
            if flows_to_int(rs[0], after[i+1:]): return True
        if mn.startswith('mov') and rs and dest(op)==reg: return False
    return False
def cls(h):
    k=h['kind'].replace('imm ','')
    inv=k in('f1/64','d1/64'); rate=k in('f64','d64')
    if not(inv or rate): return None
    before=[b[2] for b in h['before']]; after=h['after']; am=[a[2] for a in after]
    d=dest(h['op'])
    if h['mn'] in('movss','movsd') and d=='xmm0' and am[:1]==['ret']: return 'tick:getter'
    if inv and h['mn'].startswith('mul') and any(m in INT2F for m in before[-4:]): return 'tick:ticks_to_time'
    if rate and h['mn'].startswith('mul') and d.startswith('xmm') and flows_to_int(d,after): return 'tick:time_to_ticks'
    if rate and h['mn'] in('movss','movsd') and d.startswith('xmm') and uses_reg_then_int(d,after): return 'tick:time_to_ticks_reg'
    if inv and h['mn'].startswith('mul') and d.startswith('xmm') and flows_to_int(d,after): return 'ambig:inv_then_toint'
    if rate and any(m in INT2F for m in before[-4:]) and h['mn'].startswith('mul'): return 'ambig:int_times_64'
    return 'unknown:'+('inv' if inv else 'rate')
for f in sys.argv[1:]:
    D=json.load(open(f)); c=collections.Counter()
    for h in D['hits']:
        r=cls(h); h['cls']=r
        if r: c[r]+=1
    json.dump(D,open(f,'w')); print(f,dict(sorted(c.items())))
