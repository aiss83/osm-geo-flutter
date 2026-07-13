import struct
with open('/Users/aleksandr/workspace/osm-geo-flutter/russia-kaliningrad.bin','rb') as f:d=f.read()
sp_off=struct.unpack_from('<I',d,72)[0]
ai_off=struct.unpack_from('<I',d,80)[0]
for i in range(30):
    ci=struct.unpack_from('<H',d,ai_off+i*10)[0]
    p=sp_off+4
    for j in range(ci):
        nl=struct.unpack_from('<H',d,p)[0];p+=2+nl
    nl=struct.unpack_from('<H',d,p)[0];p+=2
    s=d[p:p+min(nl,60)]
    print(f'Addr[{i}] ci={ci} "{s.decode("utf-8","replace")}"')
