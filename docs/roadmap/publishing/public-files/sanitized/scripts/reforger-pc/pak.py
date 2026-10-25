#!/usr/bin/env python3
"""Read-only lister/extractor for Enfusion .pak archives (FORM/PAC1, see
design notes and engine/Poseidon/Asset/Formats/Enfusion/PakArchive.cpp).

  pak.py index <addons-dir> <index.tsv>          build a path index over every *.pak
  pak.py find  <index.tsv> <regex> [limit]       list matching member paths
  pak.py cat   <index.tsv> <member-path>          print one member (text) to stdout
  pak.py get   <index.tsv> <regex> <out-dir> [limit]  extract matching members

Used by the Reforger parity session to read Reforger's own data (prefabs, materials,
configs) on the owner's machine. Nothing extracted is committed anywhere.
"""
import os
import re
import struct
import sys
import zlib


def be32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def le32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def read_directory(path):
    with open(path, 'rb') as f:
        head = f.read(12)
        if head[:4] != b'FORM' or head[8:12] != b'PAC1':
            raise ValueError(path + ': not a PAC1 FORM')
        size = os.path.getsize(path)
        at = 12
        file_chunk = None
        while at + 8 <= size:
            f.seek(at)
            ch = f.read(8)
            tag = ch[:4]
            n = be32(ch, 4)
            if tag == b'FILE':
                file_chunk = (at + 8, n)
            at = at + 8 + n
        if not file_chunk:
            raise ValueError(path + ': no FILE chunk')
        f.seek(file_chunk[0])
        blob = f.read(file_chunk[1])
    out = []
    pos = 0

    def entry():
        nonlocal pos
        kind = blob[pos]
        ln = blob[pos + 1]
        name = blob[pos + 2:pos + 2 + ln].decode('utf-8', 'replace')
        pos += 2 + ln
        return kind, name

    kind, name = entry()
    children = le32(blob, pos)
    pos += 4
    stack = [[(name + '/') if name else '', children]]
    while stack:
        if stack[-1][1] == 0:
            stack.pop()
            continue
        stack[-1][1] -= 1
        kind, name = entry()
        if kind == 0:
            children = le32(blob, pos)
            pos += 4
            stack.append([stack[-1][0] + name + '/', children])
        elif kind == 1:
            off, stored, real = le32(blob, pos), le32(blob, pos + 4), le32(blob, pos + 8)
            flags, method = blob[pos + 18], blob[pos + 19]
            pos += 24
            out.append((stack[-1][0] + name, off, stored, real, flags, method))
        else:
            raise ValueError('unknown entry kind %d' % kind)
    return out


def cmd_index(addons, index):
    rows = []
    for root, _dirs, files in os.walk(addons):
        for fn in sorted(files):
            if fn.lower().endswith('.pak'):
                p = os.path.join(root, fn)
                for (member, off, stored, real, flags, method) in read_directory(p):
                    rows.append('\t'.join([member, p, str(off), str(stored), str(real), str(flags), str(method)]))
    with open(index, 'w', encoding='utf-8') as f:
        f.write('\n'.join(rows) + '\n')
    print('%d members indexed' % len(rows))


def load_index(index):
    with open(index, encoding='utf-8') as f:
        for line in f:
            p = line.rstrip('\n').split('\t')
            if len(p) == 7:
                yield p[0], p[1], int(p[2]), int(p[3]), int(p[4]), int(p[5]), int(p[6])


def read_member(row):
    member, pak, off, stored, real, flags, method = row
    with open(pak, 'rb') as f:
        f.seek(off)
        data = f.read(stored)
    if stored != real or method:
        data = zlib.decompress(data)
    return data


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == 'index':
        cmd_index(argv[2], argv[3])
    elif cmd == 'find':
        rx = re.compile(argv[3], re.I)
        limit = int(argv[4]) if len(argv) > 4 else 200
        n = 0
        for row in load_index(argv[2]):
            if rx.search(row[0]):
                print('%s\t%d' % (row[0], row[4]))
                n += 1
                if n >= limit:
                    break
    elif cmd == 'cat':
        want = argv[3].lower()
        for row in load_index(argv[2]):
            if row[0].lower() == want:
                sys.stdout.write(read_member(row).decode('utf-8', 'replace'))
                return 0
        print('not found: ' + argv[3], file=sys.stderr)
        return 1
    elif cmd == 'get':
        rx = re.compile(argv[3], re.I)
        out = argv[4]
        limit = int(argv[5]) if len(argv) > 5 else 50
        n = 0
        for row in load_index(argv[2]):
            if rx.search(row[0]):
                dst = os.path.join(out, row[0])
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with open(dst, 'wb') as f:
                    f.write(read_member(row))
                print(dst)
                n += 1
                if n >= limit:
                    break
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
