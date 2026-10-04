"""OFP OPRW v2 reader (heights only): geography, soundmap, mountains, tex, random, heights (LZSS + 4-byte checksum)."""
import struct, numpy as np
def lzss(d, p, size):
    N, F, TH = 4096, 18, 2; buf = bytearray(b' ' * N); r = N - F; out = bytearray(); flags = 0
    while len(out) < size:
        flags >>= 1
        if not flags & 256: flags = d[p] | 0xff00; p += 1
        if flags & 1:
            ch = d[p]; p += 1; out.append(ch); buf[r] = ch; r = (r + 1) & (N - 1)
        else:
            a = d[p]; b = d[p + 1]; p += 2; a |= (b & 0xf0) << 4; ln = (b & 0x0f) + TH; r0 = r
            for k in range(ln + 1):
                ch = buf[(r0 - a + k) & (N - 1)]; out.append(ch); buf[r] = ch; r = (r + 1) & (N - 1)
    return bytes(out[:size]), p
def comp(d, p, size):
    if size < 1024: return d[p:p + size], p + size
    o, p = lzss(d, p, size); return o, p + 4
def heights(path):
    d = open(path, 'rb').read(); assert d[:4] == b'OPRW'; ver = struct.unpack_from('<i', d, 4)[0]; assert ver == 2
    p = 8; n = 256 * 256
    _, p = comp(d, p, n * 4); _, p = comp(d, p, n)
    cnt = struct.unpack_from('<i', d, p)[0]; p += 4 + cnt * 12
    _, p = comp(d, p, n * 2); _, p = comp(d, p, n * 4)
    h, p = comp(d, p, n * 4)
    return np.frombuffer(h, '<f4').reshape(256, 256)
if __name__ == '__main__':
    H = heights('/mnt/user-data/uploads/DEC-CWA/Worlds/eden.wrp'); print(H.min(), H.max()); np.save('/tmp/edenH.npy', H)

def objects(path):
    d = open(path, 'rb').read(); p = 8; n = 256 * 256
    _, p = comp(d, p, n * 4); _, p = comp(d, p, n)
    cnt = struct.unpack_from('<i', d, p)[0]; p += 4 + cnt * 12
    _, p = comp(d, p, n * 2); _, p = comp(d, p, n * 4); _, p = comp(d, p, n * 4)
    def s():
        nonlocal p
        e = d.index(b'\0', p); v = d[p:e].decode('latin-1'); p = e + 1; return v
    tc = struct.unpack_from('<i', d, p)[0]; p += 4
    for _ in range(tc): s(); p += 1
    nc = struct.unpack_from('<i', d, p)[0]; p += 4
    names = [s() for _ in range(nc)]
    objs = []
    while p + 4 <= len(d):
        oid = struct.unpack_from('<i', d, p)[0]; p += 4
        if oid < 0: break
        ni = struct.unpack_from('<i', d, p)[0]; p += 4
        m = struct.unpack_from('<12f', d, p); p += 48
        objs.append((oid, names[ni] if 0 <= ni < nc else '?', m[9], m[10], m[11]))
    return objs
