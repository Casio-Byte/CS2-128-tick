import sys, pefile, struct, re
from capstone import *
path=sys.argv[1]; targets=sys.argv[2:]
pe=pefile.PE(path, fast_load=True)
data=pe.__data__
base=pe.OPTIONAL_HEADER.ImageBase
text=[s for s in pe.sections if s.Name.startswith(b'.text')][0]
tdata=text.get_data(); tva=text.VirtualAddress
def off2rva(o):
    for s in pe.sections:
        if s.PointerToRawData<=o<s.PointerToRawData+s.SizeOfRawData: return o-s.PointerToRawData+s.VirtualAddress
for t in targets:
    for m in re.finditer(b'\x00'+re.escape(t.encode())+b'\x00', data):
        srva=off2rva(m.start()+1)
        print(f"string {t!r} @ rva {srva:#x}")
        # scan for rip-rel disp32 hits
        for i in range(len(tdata)-4):
            d=struct.unpack_from('<i',tdata,i)[0]
            if tva+i+4+d==srva:
                print(f"   ref disp at rva {tva+i:#x}")
