#!/usr/bin/env python3
"""Generate PAA TAGG fixtures (AST-013).

Authored, not copied: the corpora AST-007 indexed are licensed Bohemia data. These
carry the same header structure as a real Arma texture -- format magic, the TAGG
block with its reversed names, the palette count, and a mip chain -- with pixel
data invented here.

Tag values mirror what was measured across 3,283 archived Arma 3 textures:
AVGC/MAXC/OFFS on every one, FLAG on 1,345 (values 1 and 2), SWIZ on 1,146
(`05040203` and `08080203` being the two most common).

Regenerate:  python tests/fixtures/paa/make_paa_tagg_fixtures.py
"""
import pathlib
import struct

HERE = pathlib.Path(__file__).resolve().parent
DXT5 = 0xFF05
DXT1 = 0xFF01


def tagg(name: str, payload: bytes) -> bytes:
    # Both the marker and the tag name are stored reversed.
    return b"GGAT" + name[::-1].encode("ascii") + struct.pack("<I", len(payload)) + payload


def mip(width: int, height: int, data: bytes, compressed: bool = False) -> bytes:
    stored = width | 0x8000 if compressed else width
    return struct.pack("<HH", stored, height) + len(data).to_bytes(3, "little") + data


def paa(magic: int, taggs: bytes, mips: bytes) -> bytes:
    # palette count 0, then the mip chain, terminated by a 0x0 entry
    return struct.pack("<H", magic) + taggs + struct.pack("<H", 0) + mips + struct.pack("<HH", 0, 0)


def main() -> None:
    # One DXT5 block covers 4x4 texels and is 16 bytes.
    block5 = bytes(16)
    block1 = bytes(8)

    full = (
        tagg("AVGC", bytes.fromhex("fe8080ff"))
        + tagg("MAXC", bytes.fromhex("ffffffff"))
        + tagg("SWIZ", bytes.fromhex("05040203"))
        + tagg("FLAG", struct.pack("<I", 1))
        + tagg("OFFS", bytes(64))
    )
    (HERE / "taggs_full.paa").write_bytes(paa(DXT5, full, mip(4, 4, block5)))

    # An unrecognised tag must be captured by name and stepped over by its declared
    # size, not dropped silently and not allowed to desynchronise the mip chain.
    with_unknown = tagg("AVGC", bytes.fromhex("fe8080ff")) + tagg("ZZZZ", b"\xde\xad\xbe\xef\x00\x11")
    (HERE / "taggs_unknown.paa").write_bytes(paa(DXT1, with_unknown, mip(4, 4, block1)))

    # No tags at all: valid, and the common shape for older content.
    (HERE / "taggs_none.paa").write_bytes(paa(DXT1, b"", mip(4, 4, block1)))

    # Bit 15 set on the stored width. Unmasked this reads as 32772 and gets blamed
    # on the texture size rather than on the compression.
    (HERE / "taggs_compressed_mip.paa").write_bytes(paa(DXT5, full, mip(4, 4, block5, compressed=True)))

    for name in ("taggs_full.paa", "taggs_unknown.paa", "taggs_none.paa", "taggs_compressed_mip.paa"):
        print(f"wrote {name}: {(HERE / name).stat().st_size} bytes")


if __name__ == "__main__":
    main()
