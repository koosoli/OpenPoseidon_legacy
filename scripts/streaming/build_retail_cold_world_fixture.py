"""Build a private WRP that references the installed retail rock, without asset overrides.

The byte self-check proves only the generated WRP subset. The runtime driver
must establish actual Data3D/Data.pbo source and placement evidence separately.
"""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path


ASSET_MODEL_PATHS = {
    "skala_new": r"data3d\skala_new.p3d",
    "skala2": r"data3d\skala2.p3d",
}
ASSET_PAC_MEMBERS = {
    "skala_new": "skala_piskovec2.pac",
    "skala2": "piskovec.pac",
}
PLACEMENTS = ((1001, 1200.0, 11.5, 1200.0), (1002, 1230.0, 11.5, 1200.0))


def load_world_builder():
    path = Path(__file__).with_name("build_simulation_residency_fixture.py")
    spec = importlib.util.spec_from_file_location("retail_cold_world_builder", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module, path


def installed_bank_evidence(game_root: Path, asset: str):
    # Independent bounded archive/member walk. This is a comparison witness,
    # not proof of the game's live mount; the runtime must still report its
    # actual parser-born source and PAC upload for the same selected asset.
    inspector_path = Path(__file__).resolve().parents[1] / "Inspect-RigidOdol7Candidate.py"
    spec = importlib.util.spec_from_file_location("retail_archive_inspector", inspector_path)
    inspector = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(inspector)
    model_member = ASSET_MODEL_PATHS[asset].split("\\")[-1]
    model_bytes, model_meta = inspector.member_bytes(game_root / "DTA" / "data3d.pbo", model_member)
    if model_meta["compressionMagic"] == "0x43707273":
        decoder = inspector.Reader(model_bytes)
        decoded = decoder.ss(model_meta["decodedBytes"])
        if decoder.pos != len(model_bytes):
            raise ValueError("compressed retail P3D member has trailing bytes")
    elif model_meta["compressionMagic"] == "0x0" and model_meta["decodedBytes"] == len(model_bytes):
        decoded = model_bytes
    else:
        raise ValueError("unsupported retail P3D member encoding")
    pac_bytes, pac_meta = inspector.member_bytes(
        game_root / "DTA" / "data.pbo", ASSET_PAC_MEMBERS[asset])
    if pac_meta["compressionMagic"] != "0x0":
        raise ValueError("retail PAC is not a raw archive member")
    return {
        "sourceEvidenceOnly": True,
        "liveMountedAuthority": False,
        "model": {**model_meta, "decodedSha256": hashlib.sha256(decoded).hexdigest()},
        "texture": {**pac_meta, "rawSha256": hashlib.sha256(pac_bytes).hexdigest()},
    }


def generate(output: Path, asset: str = "skala_new", game_root: Path = None):
    model_path = ASSET_MODEL_PATHS[asset]
    bank_evidence = installed_bank_evidence(game_root.resolve(), asset) if game_root else None
    output = output.resolve()
    if output.exists():
        raise ValueError("Output must be a new private directory")
    builder, helper = load_world_builder()
    # Private module only: retain placements while adding a valid far camera edge.
    # No shared fixture or installed source asset is modified.
    builder.LAND = 64
    world = builder.build_world(model_path=model_path, model_count=1, placements=PLACEMENTS)
    builder.verify_world(world, model_path=model_path, model_count=1, placements=PLACEMENTS)
    if not world or len(world) > 128 * 1024:
        raise ValueError("Private WRP exceeds its byte cap")
    output.mkdir(parents=True)
    wrp = output / "retail-cold.wrp"
    wrp.write_bytes(world)
    manifest = {
        "schema": 1,
        "asset": asset,
        "modelPath": model_path,
        "modelCount": 1,
        "landSide": builder.LAND,
        "cellMeters": builder.CELL,
        "placements": [
            {"id": identity, "modelIndex": 0, "position": [x, y, z]}
            for identity, x, y, z in PLACEMENTS
        ],
        "worldFile": str(wrp),
        "worldBytes": len(world),
        "worldSha256": hashlib.sha256(world).hexdigest(),
        "producerSha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "worldHelperSha256": hashlib.sha256(helper.read_bytes()).hexdigest(),
        "installedBankEvidence": bank_evidence,
        "scope": "One private WRP only; no generated P3D/PAC/PBO, addon root, runtime source, upload, or renderer-return proof.",
    }
    (output / "fixture.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, separators=(",", ":")))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--asset", choices=tuple(ASSET_MODEL_PATHS), default="skala_new")
    parser.add_argument("--game-root", type=Path)
    args = parser.parse_args()
    generate(args.output, args.asset, args.game_root)
