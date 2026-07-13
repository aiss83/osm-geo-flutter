import struct
with open('/Users/aleksandr/workspace/osm-geo-flutter/russia-kaliningrad.bin','rb') as f: data=f.read()
sp_off=struct.unpack_from('<I',data,72)[0]
ni_off=struct.unpack_from('<I',data,76)[0]

# Entry 0 in named index
b=data[ni_off:ni_off+9]
n0=struct.unpack_from('<H',b,0)[0]
print(f'Named[0] name_idx={n0}')

# Walk to string pool entry n0
p=sp_off+6
for i in range(n0):
    nlen=struct.unpack_from('<H',data,p)[0]; p+=2
    p+=nlen
    tlen=struct.unpack_from('<H',data,p)[0]; p+=2
    p+=tlen
nlen=struct.unpack_from('<H',data,p)[0]; p+=2
name=data[p:p+nlen]
print(f'SP[{n0}] nlen={nlen} name="{name.decode("utf-8",errors="replace")}"')
