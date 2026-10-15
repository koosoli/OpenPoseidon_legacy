#!/usr/bin/env python3
"""Text dump of an Enfusion EBIN file (the binarised .ent/.layer form), following the format
notes in engine/Poseidon/Asset/Formats/Enfusion/EbinWorld.hpp, with GUIDs resolved through
resourceDatabase.rdb (ResourceDatabase.hpp). Read-only; for studying Reforger's own data.

  ebin.py <file.ent> <out.txt> [rdb ...]
"""
import struct
import sys


def guid_text(b):
    return b[::-1].hex().upper()


def load_rdb(path, out):
    data = open(path, 'rb').read()
    size = len(data)

    def rec(at):
        if at + 4 > size:
            return 0, None, None
        n = struct.unpack_from('<I', data, at)[0]
        if n < 2 or n > 1024 or at + 4 + n + 14 > size or data[at + 4 + n - 1] != 0:
            return 0, None, None
        name = data[at + 4:at + 4 + n - 1]
        if any(c < 32 for c in name):
            return 0, None, None
        tail = at + 4 + n
        typ = struct.unpack_from('<H', data, tail)[0]
        ts = 22 if typ >= 6 else 14
        if tail + ts > size:
            return 0, None, None
        return 4 + n + ts, name.decode('utf-8', 'replace'), guid_text(data[tail + 6:tail + 14])

    for start in range(12, min(size, 65536)):
        at, ok = start, 0
        while ok < 64:
            step, _, _ = rec(at)
            if not step:
                break
            at += step
            ok += 1
        if ok < 64:
            continue
        at = start
        tmp = {}
        while at < size:
            step, name, g = rec(at)
            if not step:
                break
            tmp[g] = name
            at += step
        if at == size:
            out.update(tmp)
            return len(tmp)
    return 0


class Dumper:
    def __init__(self, data, rdb, w):
        self.d = data
        self.at = 0
        self.rdb = rdb
        self.w = w

    def u8(self):
        v = self.d[self.at]
        self.at += 1
        return v

    def u16(self):
        v = struct.unpack_from('<H', self.d, self.at)[0]
        self.at += 2
        return v

    def u32(self):
        v = struct.unpack_from('<I', self.d, self.at)[0]
        self.at += 4
        return v

    def f32(self, n):
        v = struct.unpack_from('<%df' % n, self.d, self.at)
        self.at += 4 * n
        return v

    def g(self, guid):
        p = self.rdb.get(guid)
        return '{%s}%s' % (guid, p or '')

    def header(self):
        assert self.d[:4] == b'EBIN'
        self.at = 4
        self.version = self.u32()
        nn, ng = self.u16(), self.u16()
        self.names = []
        for _ in range(nn):
            n = self.u16()
            self.names.append(self.d[self.at:self.at + n].decode('utf-8', 'replace'))
            self.at += n
        self.guids = []
        for _ in range(ng):
            self.guids.append(guid_text(self.d[self.at:self.at + 8]))
            self.at += 8

    def fmt(self, v):
        return ' '.join(('%.6g' % x) for x in v)

    def value(self, tag, end, decl, ind):
        if tag & 0x80:
            et = tag & 0x7F
            cnt = self.u32()
            items = []
            for _ in range(cnt):
                items.append(self.scalar(et, end, decl, ind + 1))
            self.w('%s%s [%s]\n' % (ind * ' ', decl, ', '.join(i for i in items if i is not None)))
            return
        if tag == 0x0E:
            self.block(decl, ind)
            return
        s = self.scalar(tag, end, decl, ind)
        if s is not None:
            self.w('%s%s %s\n' % (ind * ' ', decl, s))

    def scalar(self, tag, end, decl, ind):
        if tag == 0x01:
            return str(struct.unpack('<i', struct.pack('<I', self.u32()))[0])
        if tag == 0x02:
            a, b = self.u32(), self.u32()
            return '0x%x 0x%x' % (a, b)
        if tag == 0x05:
            return self.g(self.guids[self.u16()])
        if tag == 0x06:
            return self.fmt(self.f32(1))
        if tag == 0x07:
            return self.fmt(self.f32(2))
        if tag == 0x08:
            return self.fmt(self.f32(3))
        if tag == 0x09:
            return self.fmt(self.f32(4))
        if tag == 0x0B:
            return str(self.u8())
        if tag == 0x0C:
            n = self.u16()
            s = self.d[self.at:self.at + n].decode('utf-8', 'replace')
            self.at += n
            return '"%s"' % s
        if tag == 0x0E:
            self.block(decl, ind)
            return None
        raise ValueError('tag %02x at %d' % (tag, self.at))

    def block(self, decl, ind):
        start = self.at
        size = self.u32()
        end = start + size
        if self.at >= end:
            self.w('%s%s {}\n' % (ind * ' ', decl))
            return
        first = self.d[self.at]
        if first == 0x00:
            self.at += 3
            self.w('%s%s {\n' % (ind * ' ', decl))
            self.records(end, ind + 1)
            self.w('%s}\n' % (ind * ' '))
        elif first in (0x0D, 0x43, 0x44, 0x45, 0x0E):
            self.w('%s%s {\n' % (ind * ' ', decl))
            self.records(end, ind + 1)
            self.w('%s}\n' % (ind * ' '))
        elif first & 0x80:
            et = self.u8() & 0x7F
            cnt = self.u32()
            self.w('%s%s [%d] {\n' % (ind * ' ', decl, cnt))
            for _ in range(cnt):
                s = self.scalar(et, end, decl, ind + 1)
                if s is not None:
                    self.w('%s%s\n' % ((ind + 1) * ' ', s))
            self.w('%s}\n' % (ind * ' '))
            self.at = end
        else:
            self.w('%s%s <blob %d bytes>\n' % (ind * ' ', decl, end - self.at))
            self.at = end

    def records(self, end, ind):
        parts = []
        while self.at < end:
            tag = self.u8()
            if tag == 0x0D:
                name = self.names[self.u16()]
                if name == ':':
                    op = self.u8()
                    if op == 0x05:
                        parts.append(': ' + self.g(self.guids[self.u16()]))
                    else:
                        parts.append(': ' + str(self.scalar(op, end, ' '.join(parts), ind)))
                    continue
                parts.append(name)
                continue
            if tag == 0x43:
                n = self.u16()
                parts.append('"%s"' % self.d[self.at:self.at + n].decode('utf-8', 'replace'))
                self.at += n
                continue
            if tag == 0x44:
                parts.append(self.g(guid_text(self.d[self.at:self.at + 8])))
                self.at += 8
                continue
            if tag == 0x45:
                parts.append(self.g(self.guids[self.u16()]))
                continue
            self.value(tag, end, ' '.join(parts), ind)
            parts = []


def main():
    src, dst = sys.argv[1], sys.argv[2]
    rdb = {}
    for p in sys.argv[3:]:
        n = load_rdb(p, rdb)
        print('%s: %d records' % (p, n), file=sys.stderr)
    data = open(src, 'rb').read()
    with open(dst, 'w', encoding='utf-8', buffering=1 << 20) as f:
        dmp = Dumper(data, rdb, f.write)
        dmp.header()
        dmp.records(len(data), 0)
    print('done', file=sys.stderr)


if __name__ == '__main__':
    main()
