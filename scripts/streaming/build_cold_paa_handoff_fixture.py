"""Original bounded cold-PAA fixture. Generation/self-check is not runtime proof."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def generate(output, alpha_tagged=False):
    output = output.resolve()
    if output.exists():
        raise ValueError("Output must be a new private directory")
    if not str(output).isascii():
        raise ValueError("Fixture path must be ASCII")
    model_file = output / "wall.p3d"
    if len(str(model_file.with_name("wall_unused_001.p3d"))) >= 128:
        raise ValueError("Engine canonical model path must remain below 128 bytes")
    scripts = Path(__file__).resolve().parents[1]
    world_builder = load(scripts / "streaming/build_simulation_residency_fixture.py", "cold_world")
    showcase = load(scripts / "showcase/build_showcase.py", "cold_showcase")
    prefix = "coldpaa_" + hashlib.sha256(str(output).encode("ascii")).hexdigest()[:12]
    texture = prefix + "\\primary.paa"
    block = struct.pack("<BB6sHHI", 255, 255, bytes(6), 0xf800, 0xf800, 0)
    paa = bytearray(struct.pack("<H", 0xff05))
    if alpha_tagged:
        # Pactext::PacPalette::Load reads little-endian TAGG/FLAG constants:
        # physical bytes GGAT/GALF, size4, PicFlagAlpha1. BC3 alone is not
        # authored alpha eligibility. The original opaque BC blocks stay exact.
        paa += b"GGATGALF" + struct.pack("<II", 4, 1)
    paa += struct.pack("<H", 0)  # no palette, after optional tags
    mips = []
    for side in (64, 32, 16, 8, 4):
        raw = block * ((side // 4) ** 2)
        packed = world_builder.bulk(raw)
        compressed = len(raw) >= 1024
        offset = len(paa)
        paa += struct.pack("<HH", side | (0x8000 if compressed else 0), side)
        paa += len(packed).to_bytes(3, "little") + packed
        mips.append(dict(side=side, headerOffset=offset, storedBytes=len(packed), decodedBytes=len(raw), lzo=compressed))
    paa += bytes(4)
    paa = bytes(paa)
    # Independent byte-level self-check, including literal-only LZO payloads.
    if alpha_tagged:
        assert paa[:2] == struct.pack("<H", 0xff05)
        assert paa[2:10] == b"GGATGALF"
        assert struct.unpack_from("<II", paa, 10) == (4, 1)
        assert struct.unpack_from("<H", paa, 18)[0] == 0
        cursor = 20
    else:
        assert paa[:4] == struct.pack("<HH", 0xff05, 0)
        cursor = 4
    for mip in mips:
        w, h = struct.unpack_from("<HH", paa, cursor)
        size = int.from_bytes(paa[cursor+4:cursor+7], "little")
        assert cursor == mip["headerOffset"] and h == mip["side"]
        assert (w & 0x7fff) == h and bool(w & 0x8000) == mip["lzo"]
        payload = paa[cursor+7:cursor+7+size]
        expected = block * ((h // 4) ** 2)
        if mip["lzo"]:
            index = 1
            length = 0
            assert payload[0] == 0
            while payload[index] == 0:
                length += 255
                index += 1
            length += 18 + payload[index]
            index += 1
            assert length == len(expected) and payload[index:index+length] == expected
            assert payload[index+length:] == b"\x11\0\0"
        else:
            assert payload == expected
        cursor += 7 + size
    assert paa[cursor:] == bytes(4)
    header = b"primary.paa\0" + struct.pack("<5I", 0, 0, 0, 0, len(paa)) + b"\0" + bytes(20)
    pbo = header + paa
    assert pbo[len(header):] == paa and struct.unpack_from("<5I", pbo, 12)[4] == len(paa)
    model = showcase.model((0.5, 3.0, 6.0), texture, "")
    assert model[:12] == b"MLOD" + struct.pack("<II", 257, 4)
    assert model.count(texture.encode("ascii") + b"\0") == 6
    world = world_builder.build_world(str(model_file), 2, reference_all=True)
    world_builder.verify_world(world, str(model_file), 2, reference_all=True)
    files = {"wall.p3d": model, "wall_unused_001.p3d": model, "cold-paa.wrp": world, "addons/" + prefix + ".pbo": pbo}
    if sum(map(len, files.values())) > 131072 or len(paa) > 16 * 1024 * 1024:
        raise ValueError("Fixture byte cap exceeded")
    output.mkdir()
    records = []
    for name, data in files.items():
        path = output / name
        path.parent.mkdir(exist_ok=True)
        path.write_bytes(data)
        records.append(dict(path=str(path), bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
    manifest = dict(schema=1, originalGenerated=True, modelCount=2, placements=[1001,1002],
                    sourceName=texture, archivePrefix=prefix, archiveMember="primary.paa",
                    memberOffset=len(header), memberBytes=len(paa), paaSha256=hashlib.sha256(paa).hexdigest(),
                    alphaTagged=bool(alpha_tagged), alphaFlag=1 if alpha_tagged else 0,
                    alphaTagBytes=16 if alpha_tagged else 0,
                    alphaScope="Authored FLAG eligibility only; BC3 alpha samples remain255 and classification/peek require actual engine evidence.",
                    chainBytes=sum(m["decodedBytes"] for m in mips),
                    selectedMipCount=4, selectedChainBytes=sum(m["decodedBytes"] for m in mips if m["side"] > 4),
                    mipSelectionScope="Existing TextureSourcePac::Init MIN_MIP_SIZE=4 excludes final 4x4 from _mipmaps; archive/worker retains all five levels.",
                    mips=mips, files=records,
                    producerSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                    worldHelperSha256=hashlib.sha256((scripts / "streaming/build_simulation_residency_fixture.py").read_bytes()).hexdigest(),
                    modelHelperSha256=hashlib.sha256((scripts / "showcase/build_showcase.py").read_bytes()).hexdigest(),
                    scope="Two original MLOD model paths, two placements, one primary BC3 PAA. Byte self-check is not engine parsing/upload/pixel proof.")
    (output / "fixture.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, separators=(",", ":")))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--alpha-tagged", action="store_true", help="Author FLAG PicFlagAlpha=1 without changing any BC3 block")
    args = parser.parse_args()
    generate(args.output, args.alpha_tagged)
