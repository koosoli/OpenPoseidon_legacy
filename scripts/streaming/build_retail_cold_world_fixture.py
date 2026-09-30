"""Build a private WRP that references the installed retail rock, without asset overrides.

The byte self-check proves only the generated WRP subset. The runtime driver
must establish actual Data3D/Data.pbo source and placement evidence separately.
"""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path


MODEL_PATH = r"data3d\skala_new.p3d"
PLACEMENTS = ((1001, 1200.0, 11.5, 1200.0), (1002, 1230.0, 11.5, 1200.0))


def load_world_builder():
    path = Path(__file__).with_name("build_simulation_residency_fixture.py")
    spec = importlib.util.spec_from_file_location("retail_cold_world_builder", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module, path


def generate(output: Path):
    output = output.resolve()
    if output.exists():
        raise ValueError("Output must be a new private directory")
    builder, helper = load_world_builder()
    world = builder.build_world(model_path=MODEL_PATH, model_count=1, placements=PLACEMENTS)
    builder.verify_world(world, model_path=MODEL_PATH, model_count=1, placements=PLACEMENTS)
    if not world or len(world) > 128 * 1024:
        raise ValueError("Private WRP exceeds its byte cap")
    output.mkdir(parents=True)
    wrp = output / "retail-cold.wrp"
    wrp.write_bytes(world)
    manifest = {
        "schema": 1,
        "modelPath": MODEL_PATH,
        "modelCount": 1,
        "placements": [
            {"id": identity, "modelIndex": 0, "position": [x, y, z]}
            for identity, x, y, z in PLACEMENTS
        ],
        "worldFile": str(wrp),
        "worldBytes": len(world),
        "worldSha256": hashlib.sha256(world).hexdigest(),
        "producerSha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "worldHelperSha256": hashlib.sha256(helper.read_bytes()).hexdigest(),
        "scope": "One private WRP only; no generated P3D/PAC/PBO, addon root, runtime source, upload, or renderer-return proof.",
    }
    (output / "fixture.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, separators=(",", ":")))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    generate(parser.parse_args().output)
