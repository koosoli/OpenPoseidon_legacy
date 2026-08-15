import sys, struct, zlib
def conv(src, dst):
    d = open(src,'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]
    w, h = struct.unpack_from('<ii', d, 18)
    bpp = struct.unpack_from('<H', d, 28)[0]
    assert bpp == 24
    row = (w*3+3)&~3
    raw = bytearray()
    for y in range(h-1, -1, -1):
        s = off + y*row
        raw.append(0)
        line = d[s:s+w*3]
        raw.extend(bytes(line[i+2-j] if False else line[i:i+3][::-1][j] for i in range(0,w*3,3) for j in range(3)))
    def chunk(t, data):
        c = t + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 6))
    png += chunk(b'IEND', b'')
    open(dst,'wb').write(png)
for p in sys.argv[1:]:
    conv(p, p[:-4] + '.png')
    print(p[:-4] + '.png')
