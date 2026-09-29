#!/usr/bin/env python3
"""Read-only, bounded CWA source evidence; never a live render/source certificate.

Example (run from combat-audit): python scripts/Inspect-RigidOdol7Candidate.py
Only bounded archive header/member reads and PoseidonTools `pbo show` are used.
No extraction, install mutation, game launch or model/material admission occurs.
The independent wire walk must consume the entire decoded ODOL7 member, checking
every SSCompress checksum. Canonical IR conversion remains a separate gate.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

CAP = 128 * 1024


def sha(data):
    return hashlib.sha256(data).hexdigest()


def cstring(stream):
    value = bytearray()
    for _ in range(256):
        ch = stream.read(1)
        if not ch:
            raise ValueError("truncated archive string")
        if ch == b"\0":
            return value.decode("ascii")
        value += ch
    raise ValueError("archive string cap")


def member_bytes(archive, member):
    with archive.open("rb") as stream:
        size = stream.seek(0, 2)
        stream.seek(0)
        rows = []
        for _ in range(20000):
            if stream.tell() > 8 * 1024 * 1024:
                raise ValueError("archive table cap")
            name = cstring(stream)
            magic, decoded, _, _, stored = struct.unpack("<5I", stream.read(20))
            if not name and magic == 0x56657273:  # Vers header, properties
                for _ in range(128):
                    if not cstring(stream):
                        break
                    cstring(stream)
                else:
                    raise ValueError("archive property cap")
            elif not name and not magic and not stored:
                break
            else:
                rows.append((name, magic, decoded, stored))
        else:
            raise ValueError("archive member cap")
        offset = stream.tell()
        matches = []
        for name, magic, decoded, stored in rows:
            if offset + stored > size:
                raise ValueError("archive range")
            if name.lower() == member.lower():
                if stored <= 0 or stored > CAP or decoded > CAP:
                    raise ValueError("member byte cap")
                matches.append((magic, decoded, stored, offset))
            offset += stored
        if len(matches) != 1:
            raise ValueError("missing or duplicate member")
        magic, decoded, stored, offset = matches[0]
        stream.seek(offset)
        payload = stream.read(stored)
        return payload, dict(archive=str(archive), archiveBytes=size, member=member,
                             compressionMagic=hex(magic), decodedBytes=decoded or stored,
                             storedBytes=stored, offset=offset, storedSha256=sha(payload))


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def take(self, n):
        if n < 0 or self.pos + n > len(self.data):
            raise ValueError("truncated bounded payload")
        value = self.data[self.pos:self.pos+n]
        self.pos += n
        return value

    def value(self, fmt):
        return struct.unpack("<" + fmt, self.take(struct.calcsize("<" + fmt)))

    def u32(self):
        return self.value("I")[0]

    def count(self):
        value = self.u32()
        if value > 8192:
            raise ValueError("array/list count cap")
        return value

    def string(self):
        start = self.pos
        for _ in range(256):
            if self.take(1) == b"\0":
                return self.data[start:self.pos-1].decode("ascii")
        raise ValueError("payload string cap")

    def ss(self, wanted):
        if not 0 < wanted <= CAP:
            raise ValueError("SSCompress decoded cap")
        ring = bytearray(b" " * 4096)
        cursor, flags, result = 4078, 0, bytearray()
        while len(result) < wanted:
            flags >>= 1
            if not flags & 256:
                flags = self.take(1)[0] | 0xff00
            if flags & 1:
                values = self.take(1)
                for ch in values:
                    result.append(ch); ring[cursor] = ch; cursor = (cursor+1) & 4095
            else:
                low, high = self.take(2)
                distance = low | ((high & 0xf0) << 4)
                source = cursor-distance
                for k in range(min((high & 15)+3, wanted-len(result))):
                    ch = ring[(source+k) & 4095]
                    result.append(ch); ring[cursor] = ch; cursor = (cursor+1) & 4095
        if self.u32() != (sum(result) & 0xffffffff):
            raise ValueError("SSCompress checksum")
        return bytes(result)

    def array(self, stride, compressed=False):
        n = self.count()
        byte_count = n*stride
        if byte_count > CAP:
            raise ValueError("array byte cap")
        data = self.ss(byte_count) if compressed and byte_count >= 1024 else self.take(byte_count)
        return n, data


def inspect_odol(data):
    r = Reader(data)
    if r.take(4) != b"ODOL" or r.u32() != 7:
        raise ValueError("requires exact ODOL7")
    lod_count = r.count()
    if not 1 <= lod_count <= 16:
        raise ValueError("LOD cap")
    lods = []
    for i in range(lod_count):
        _, clips = r.array(4, True)
        _, uv = r.array(8, True)
        vertices, positions = r.array(12)
        normals, _ = r.array(12)
        or_hints, and_hints = r.value("II")
        r.take(40)
        textures = [r.string() for _ in range(r.count())]
        r.array(2, True); r.array(2, True)
        faces = r.count(); section_offset = r.u32()
        tri = quad = 0; face_flags = 0; valid_indices = True
        for _ in range(faces):
            flag, texture, count = r.value("IhB")
            if count not in (3, 4):
                raise ValueError("face corner count")
            indices = r.value("H"*count)
            tri += count == 3; quad += count == 4; face_flags |= flag
            valid_indices &= max(indices) < vertices and 0 <= texture < len(textures)
        sections = [dict(zip(("lowerByteOffset", "upperByteOffset", "material", "texture", "special"),
                            r.value("IIihi"))) for _ in range(r.count())]
        selections = []
        for _ in range(r.count()):
            name = r.string()
            counts = [r.array(s, True)[0] for s in (2, 1, 4)]
            r.take(1)
            counts.extend(r.array(s, True)[0] for s in (4, 2, 1))
            selections.append(dict(name=name, elementCounts=counts))
        properties = [[r.string(), r.string()] for _ in range(r.count())]
        frames = r.count()
        for _ in range(frames):
            r.take(4); r.array(12)
        color_top, color, special = r.value("III")
        proxies = r.count()
        for _ in range(proxies):
            r.string(); r.take(56)
        clip_or = 0
        for (flag,) in struct.iter_unpack("<I", clips):
            clip_or |= flag
        lods.append(dict(index=i, vertices=vertices, normals=normals, uvCount=len(uv)//8,
                         triangles=tri, quads=quad, validIndices=valid_indices,
                         clipOr=clip_or, faceFlagsOr=face_flags, orHints=or_hints,
                         andHints=and_hints, textures=textures, sections=sections,
                         selections=selections, properties=properties, frames=frames,
                         proxies=proxies, special=special, sectionOffset=section_offset))
    resolutions = r.value("f"*lod_count)
    for lod, resolution in zip(lods, resolutions):
        lod["resolution"] = resolution
    model_special, _, _, remarks, model_and_hints, model_or_hints = r.value("iffiII")
    r.take(120)
    auto, lock, occlude, occluded, animation, map_type = r.value("5Bb")
    r.array(4, True); r.take(16)
    role_indices = r.value("12b")
    if r.pos != len(data):
        raise ValueError("ODOL trailing bytes")
    return dict(revision=7, fullDecodedBytes=r.pos, allowAnimation=animation,
                mapType=map_type, autoCenter=auto, lockAutoCenter=lock,
                modelSpecial=model_special, modelOrHints=model_or_hints,
                modelAndHints=model_and_hints, remarks=remarks,
                roleLodIndices=role_indices, lods=lods)


def inspect_pac(data):
    r = Reader(data)
    if r.value("H")[0] != 0xff01:
        raise ValueError("requires candidate DXT1")
    tags = []
    while r.data[r.pos:r.pos+4] == b"GGAT":
        r.take(4); name = r.take(4)[::-1].decode("ascii"); n = r.u32()
        if n > 128 or len(tags) >= 16:
            raise ValueError("PAC tag cap")
        payload = r.take(n)
        tags.append(dict(name=name, bytes=n, value=payload.hex() if n <= 4 else None))
    palette = r.value("H")[0]
    if palette != 0:
        raise ValueError("unexpected DXT1 palette")
    mips = []
    for _ in range(16):
        w, h = r.value("HH")
        if not w and not h:
            break
        if w & 0x8000 or not 1 <= w <= 256 or not 1 <= h <= 256:
            raise ValueError("candidate mip dimension/compression")
        count = int.from_bytes(r.take(3), "little")
        blocks = ((w+3)//4)*((h+3)//4)
        if count != blocks*8:
            raise ValueError("DXT1 block bytes")
        payload = r.take(count)
        transparent = sum(c0 <= c1 for c0, c1, _ in struct.iter_unpack("<HHI", payload))
        mips.append(dict(width=w, height=h, storedBytes=count,
                         blocks=blocks, transparentModeBlocks=transparent))
    else:
        raise ValueError("mip cap")
    # Candidate PAC retains an additional zero16 after the zero dimension pair.
    if r.take(len(data)-r.pos) not in (b"", b"\0\0"):
        raise ValueError("PAC nonzero tail")
    return dict(format="DXT1", tags=tags, palette=palette, mips=mips,
                everyMipOpaque=bool(mips) and all(not m["transparentModeBlocks"] for m in mips))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-root", type=Path, default=Path("D:/SteamLibrary/steamapps/common/ARMA Cold War Assault"))
    parser.add_argument("--tools", type=Path, default=Path(__file__).resolve().parents[1]/"build/win-x64-clang-rwdi/apps/tools/Tools/PoseidonTools.exe")
    args = parser.parse_args()
    model_archive = args.game_root/"DTA/data3d.pbo"
    encoded, model_meta = member_bytes(model_archive, "skala_new.p3d")
    if model_meta["compressionMagic"] != "0x43707273":
        raise ValueError("candidate expected Cprs")
    decoder = Reader(encoded)
    decoded = decoder.ss(model_meta["decodedBytes"])
    if decoder.pos != len(encoded):
        raise ValueError("compressed member tail")
    canonical = subprocess.run([str(args.tools), "pbo", "show", str(model_archive), "skala_new.p3d"],
                               capture_output=True, check=True, timeout=30).stdout
    if canonical != decoded:
        raise ValueError("independent decode != Tools exact stdout")
    model_meta.update(decodedSha256=sha(decoded), consumedStoredBytes=decoder.pos,
                      toolsExactBytesMatch=True, wire=inspect_odol(decoded))
    pac, pac_meta = member_bytes(args.game_root/"DTA/data.pbo", "skala_piskovec2.pac")
    if pac_meta["compressionMagic"] != "0x0":
        raise ValueError("candidate PAC expected raw member")
    pac_meta.update(wire=inspect_pac(pac))
    print(json.dumps(dict(evidenceSchema=1, sourceEvidenceOnly=True,
                         liveMountedAuthority=False, canonicalIrVerified=False,
                         model=model_meta, texture=pac_meta), indent=2))


if __name__ == "__main__":
    main()
