import struct

with open('/Users/aleksandr/workspace/osm-geo-flutter/russia-kaliningrad.bin', 'rb') as f:
    data = f.read()

sp_off = struct.unpack_from('<I', data, 72)[0]
ni_off = struct.unpack_from('<I', data, 76)[0]

print(f'sp_off={sp_off} ni_off={ni_off}')

# Named index: first 5 entries
for i in range(5):
    off = ni_off + i * 9
    b = data[off:off+9]
    name_idx = struct.unpack_from('<H', b, 0)[0]
    translit_idx = struct.unpack_from('<H', b, 2)[0]
    cat = b[4]
    rec_idx = struct.unpack_from('<I', b, 5)[0]

    # Try byte-offset interpretation
    byte_off = sp_off + name_idx
    end = data.find(b'\x00', byte_off)
    if end == -1:
        s = data[byte_off:byte_off+60]
        print(f'  [{i}] name_idx={name_idx} byte_off={byte_off} NO_NULL: {s[:60]}')
    else:
        s = data[byte_off:end]
        try:
            decoded = s.decode('utf-8')
        except:
            decoded = f'<binary:{len(s)}b>'
        print(f'  [{i}] name_idx={name_idx} byte_off={byte_off} len={len(s)}: \"{decoded}\"')

# Also check: is name_idx 0 the first string in the "real" string pool?
# Try finding strings starting after some header
for header_size in [0, 4, 8, 12, 16]:
    print(f'\n--- header_size={header_size} ---')
    p = sp_off + header_size
    for j in range(5):
        if p >= ni_off: break
        end2 = data.find(b'\x00', p)
        if end2 == -1: break
        s = data[p:end2]
        try:
            decoded = s.decode('utf-8')
        except:
            decoded = f'<{len(s)}b>'
        if len(s) > 0:
            print(f'  seq[{j}] off={p-sp_off} len={len(s)}: \"{decoded}\"')
        else:
            print(f'  seq[{j}] off={p-sp_off} EMPTY')
        p = end2 + 1

# Check what the "length-prefixed" strings look like from offset 8:
print('\n--- Length-prefixed from offset 8 ---')
p = sp_off + 8
for j in range(10):
    if p + 2 > ni_off: break
    slen = struct.unpack_from('<H', data, p)[0]
    p += 2
    if p + slen > ni_off: break
    s = data[p:p+slen]
    try:
        decoded = s.decode('utf-8')
    except:
        decoded = f'<{slen}b>'
    print(f'  [{j}] off={p-sp_off} slen={slen}: \"{decoded}\"')
    p += slen
