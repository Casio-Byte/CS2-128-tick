import sys, pefile
from capstone import *
pe=pefile.PE(sys.argv[1], fast_load=True)
md=Cs(CS_ARCH_X86,CS_MODE_64)
for a in sys.argv[2:]:
    a,n=(a.split(':')+['40'])[:2]; a=int(a,16); n=int(n)
    print(f"==== {a:#x}")
    for i in md.disasm(pe.get_data(a,n*7),a):
        print(f"{i.address:#x}: {i.bytes.hex():<24} {i.mnemonic} {i.op_str}")
        n-=1
        if n<=0: break
