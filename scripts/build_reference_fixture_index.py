#!/usr/bin/env python3
"""AST-007 -- index the owner-provided Real Virtuality reference corpora.

    python scripts/build_reference_fixture_index.py            # index and write
    python scripts/build_reference_fixture_index.py --summary  # print, do not write

Reads root locations from `.reference-corpora.json` (gitignored, see the
`.example` beside it) or from `$POSEIDON_REFERENCE_CORPORA`, so no contributor's
absolute path is baked into anything tracked.  Every root is optional: a machine
without them -- CI, another contributor -- gets an index that says so rather than
an error, because AST-007 must not become a build dependency on one workstation.

The index records what a compatibility ticket actually needs to decide something:
the container family and per-LOD signature of each P3D, which special LODs it
carries, how many UV sets, which virtual resource prefixes its material and
texture references use, the shader IDs its RVMATs name, and which PAA storage
tags each texture actually exercises.  A directory listing would not answer any
of those, and answering them by hand for 1600 models is how a fixture gets picked
because it was convenient rather than because it exercises the capability.

Deliberately bounded, per AST-007: full parsing is on by default only for roots
the config marks `deep`.  Everything else is inventoried by signature and header.
Nothing here decodes pixels or builds geometry -- that is AST-011/AST-013 work.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import struct
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
CONFIG = ROOT / ".reference-corpora.json"
OUTPUT = ROOT / "docs/roadmap/evidence/compat-c1-fixture-index.json"

# MLOD LOD resolution encodes the LOD's role. Values are exact in the files but
# stored as float32, so they are matched with a relative tolerance rather than
# by equality -- an exact compare silently reclassifies every special LOD as a
# visual one at some absurd resolution, which is the kind of error that only
# shows up as a missing shadow three tickets later.
LOD_ROLES = [
    (1.0e3, "ViewGunner"), (1.1e3, "ViewPilot"), (1.2e3, "ViewCargo"),
    (1.0e4, "ShadowVolume"), (1.1e4, "ShadowVolume2"),
    (1.0e13, "Geometry"), (2.0e13, "Memory"), (3.0e13, "LandContact"),
    (4.0e13, "Roadway"), (5.0e13, "Paths"), (6.0e13, "HitPoints"),
    (7.0e13, "ViewGeometry"), (8.0e13, "FireGeometry"),
    (9.0e13, "ViewCargoGeometry"), (1.0e15, "ViewCargoFireGeometry"),
    (1.1e15, "ViewCommander"), (1.2e15, "ViewCommanderGeometry"),
    (1.3e15, "ViewCommanderFireGeometry"), (1.4e15, "ViewPilotGeometry"),
    (1.5e15, "ViewPilotFireGeometry"), (1.6e15, "ViewGunnerGeometry"),
    (1.7e15, "ViewGunnerFireGeometry"), (1.8e15, "SubParts"),
    (1.9e15, "ShadowVolumeViewCargo"), (2.0e15, "ShadowVolumeViewPilot"),
    (2.1e15, "ShadowVolumeViewGunner"), (2.2e15, "Wreck"),
]

PAA_TYPES = {
    0xFF01: "DXT1", 0xFF02: "DXT2", 0xFF03: "DXT3", 0xFF04: "DXT4",
    0xFF05: "DXT5", 0x4444: "ARGB4444", 0x1555: "ARGB1555", 0x8888: "ARGB8888",
    0x8080: "AI88", 0x8000: "P8",
}

# Suffix carries the intended semantic in Real Virtuality, and getting it wrong
# is exactly the failure MAT-020 warns about (SMDI is not roughness).
TEXTURE_SUFFIXES = {
    "co": "BaseColour", "ca": "BaseColourAlpha", "nohq": "NormalMap",
    "no": "NormalMap", "novhq": "NormalMapVertexHigh", "smdi": "SpecularMapDetail",
    "as": "AmbientShadow", "dt": "DetailTexture", "mc": "MacroTexture",
    "mco": "MultiplyColour", "sm": "SpecularMap", "adshq": "AmbientShadowHigh",
    "ti": "ThermalImaging", "tiCA": "ThermalImagingAlpha", "mask": "Mask",
    "pr": "Parallax", "nopx": "NormalParallax", "nofhq": "NormalMapFaded",
    "sy": "SkySpecular", "gs": "GlossSpecular", "lco": "LayerColour",
}

VIRTUAL_PREFIXES = ("a3\\", "ca\\", "a2\\", "arma2\\", "dta\\", "cwr\\")


class Truncated(Exception):
    """A file ended mid-structure; recorded as a parse limit, never guessed past."""


def asciiz(data: bytes, offset: int) -> tuple[str, int]:
    end = data.find(b"\0", offset)
    if end < 0:
        raise Truncated("unterminated string")
    return data[offset:end].decode("latin1"), end + 1


def u32(data: bytes, offset: int) -> int:
    if offset + 4 > len(data):
        raise Truncated("eof reading u32")
    return struct.unpack_from("<I", data, offset)[0]


def lod_role(resolution: float) -> str:
    for value, name in LOD_ROLES:
        if abs(resolution - value) <= abs(value) * 1e-4:
            return name
    if resolution < 1.0e3:
        return f"VisualLod({resolution:g})"
    return f"Unknown({resolution:g})"


def virtual_prefix(path: str) -> str | None:
    lowered = path.replace("/", "\\").lower().lstrip("\\")
    for prefix in VIRTUAL_PREFIXES:
        if lowered.startswith(prefix):
            return prefix.rstrip("\\")
    return None


def texture_role(path: str) -> str | None:
    stem = pathlib.PurePath(path.replace("\\", "/")).stem
    suffix = stem.rsplit("_", 1)[-1] if "_" in stem else ""
    return TEXTURE_SUFFIXES.get(suffix) or TEXTURE_SUFFIXES.get(suffix.lower())


def parse_p3dm_lod(data: bytes, offset: int, deep: bool) -> tuple[dict, int]:
    """Parse one MLOD LOD. Returns the record and the offset of the next LOD."""
    signature = data[offset:offset + 4].decode("latin1")
    major, minor, points, normals, faces, flags = struct.unpack_from("<IIIIII", data, offset + 4)
    record = {
        "signature": signature, "major": major, "minor": minor,
        "points": points, "normals": normals, "faces": faces, "flags": flags,
    }
    cursor = offset + 28 + points * 16 + normals * 12

    textures: set[str] = set()
    materials: set[str] = set()
    for _ in range(faces):
        cursor += 4 + 64 + 4  # vertex count, four 16-byte vertices, face flags
        texture, cursor = asciiz(data, cursor)
        material, cursor = asciiz(data, cursor)
        if texture:
            textures.add(texture)
        if material:
            materials.add(material)

    if data[cursor:cursor + 4] != b"TAGG":
        raise Truncated(f"expected TAGG at {cursor}, found {data[cursor:cursor + 4]!r}")
    cursor += 4

    taggs: dict[str, int] = {}
    selections: list[str] = []
    properties: dict[str, str] = {}
    while True:
        cursor += 1  # active flag
        name, cursor = asciiz(data, cursor)
        length = u32(data, cursor)
        cursor += 4
        payload = data[cursor:cursor + length]
        cursor += length
        taggs[name] = taggs.get(name, 0) + 1
        if name == "#Property#" and length >= 128:
            key = payload[:64].split(b"\0", 1)[0].decode("latin1")
            val = payload[64:128].split(b"\0", 1)[0].decode("latin1")
            properties[key] = val
        elif not name.startswith("#"):
            selections.append(name)
        if name == "#EndOfFile#":
            break

    resolution = struct.unpack_from("<f", data, cursor)[0]
    cursor += 4

    record["resolution"] = resolution
    record["lod_role"] = lod_role(resolution)
    record["uv_sets"] = taggs.get("#UVSet#", 0)
    record["tagg_counts"] = dict(sorted(taggs.items()))
    record["named_selections"] = sorted(selections)
    record["properties"] = dict(sorted(properties.items()))
    if deep:
        record["textures"] = sorted(textures)
        record["materials"] = sorted(materials)
    else:
        record["texture_count"] = len(textures)
        record["material_count"] = len(materials)
    record["_textures"] = textures
    record["_materials"] = materials
    return record, cursor


def parse_p3d(path: pathlib.Path, deep: bool) -> dict:
    data = path.read_bytes()
    signature = data[:4].decode("latin1", "replace")
    record: dict = {"container": signature}

    if signature == "ODOL":
        # AST-012A dispatch input only. Reading the revision is the whole point;
        # parsing an ODOL body is a separate, fixture-led ticket.
        record["odol_version"] = u32(data, 4)
        record["lods"] = []
        return record
    if signature != "MLOD":
        record["error"] = f"unknown container signature {signature!r}"
        return record

    record["mlod_version"] = u32(data, 4)
    lod_count = u32(data, 8)
    record["lod_count"] = lod_count

    lods: list[dict] = []
    textures: set[str] = set()
    materials: set[str] = set()
    cursor = 12
    for index in range(lod_count):
        lod_signature = data[cursor:cursor + 4].decode("latin1", "replace")
        if lod_signature == "P3DM":
            lod, cursor = parse_p3dm_lod(data, cursor, deep)
        elif lod_signature == "SP3X":
            # Not parsed here: SP3X already has a production reader, and walking
            # it with the P3DM layout would silently produce wrong offsets.
            lods.append({"index": index, "signature": "SP3X", "parsed": False})
            record["lods"] = lods
            record["parse_limit"] = "SP3X LOD encountered; remaining LODs not walked"
            return record
        else:
            raise Truncated(f"unknown LOD signature {lod_signature!r} at {cursor}")
        textures |= lod.pop("_textures")
        materials |= lod.pop("_materials")
        lod["index"] = index
        lods.append(lod)

    record["lods"] = lods
    record["lod_signatures"] = sorted({lod["signature"] for lod in lods})
    record["lod_roles"] = sorted({lod.get("lod_role", "?") for lod in lods})
    record["max_uv_sets"] = max((lod.get("uv_sets", 0) for lod in lods), default=0)
    record["references"] = summarise_references(textures, materials)
    return record


def summarise_references(textures: set[str], materials: set[str]) -> dict:
    prefixes = {virtual_prefix(ref) for ref in textures | materials}
    roles = {texture_role(ref) for ref in textures}
    return {
        "texture_count": len(textures),
        "material_count": len(materials),
        "virtual_prefixes": sorted(p for p in prefixes if p),
        "unprefixed_references": sorted(ref for ref in textures | materials if not virtual_prefix(ref))[:8],
        "texture_roles": sorted(r for r in roles if r),
        "textures": sorted(textures),
        "materials": sorted(materials),
    }


RVMAT_SHADER = re.compile(r'(?:pixelShaderID|vertexShaderID)\s*=\s*"([^"]+)"', re.IGNORECASE)
RVMAT_STAGE = re.compile(r"class\s+(Stage\w+)", re.IGNORECASE)
RVMAT_TEXTURE = re.compile(r'texture\s*=\s*"([^"]*)"', re.IGNORECASE)
RVMAT_UVSOURCE = re.compile(r'uvSource\s*=\s*"([^"]+)"', re.IGNORECASE)


def parse_rvmat(path: pathlib.Path) -> dict:
    data = path.read_bytes()
    if data[:4] == b"\0raP":
        # Binarized ParamFile. Decoding it belongs to AST-014's material-source
        # unification, not to an inventory pass that would have to half-implement
        # the config reader to guess at it.
        return {"binarized": True}
    text = data.decode("latin1", "replace")
    textures = [t for t in RVMAT_TEXTURE.findall(text) if t]
    return {
        "binarized": False,
        "shader_ids": sorted(set(RVMAT_SHADER.findall(text))),
        "stages": sorted(set(RVMAT_STAGE.findall(text))),
        "uv_sources": sorted(set(RVMAT_UVSOURCE.findall(text))),
        "textures": sorted(set(textures)),
        "texture_roles": sorted({r for r in (texture_role(t) for t in textures) if r}),
        "virtual_prefixes": sorted({p for p in (virtual_prefix(t) for t in textures) if p}),
    }


def parse_paa(path: pathlib.Path) -> dict:
    data = path.read_bytes()
    if len(data) < 16:
        return {"error": "too small"}
    type_tag = struct.unpack_from("<H", data, 0)[0]
    record = {"type": PAA_TYPES.get(type_tag, f"0x{type_tag:04X}"), "bytes": len(data)}

    # TAGGs are the decode-critical part for AST-013: SWIZ and FLAG change what
    # the stored channels mean, so which ones a fixture actually carries decides
    # whether it can prove anything about the decoder.
    tags: list[str] = []
    cursor = 2
    while cursor + 12 <= len(data) and data[cursor:cursor + 4] == b"GGAT":
        name = data[cursor + 4:cursor + 8][::-1].decode("latin1", "replace")
        length = struct.unpack_from("<I", data, cursor + 8)[0]
        tags.append(name)
        cursor += 12 + length
    record["taggs"] = tags
    record["role"] = texture_role(path.name)
    return record


def parse_pbo(path: pathlib.Path) -> dict:
    """Index a PBO's header without decompressing anything.

    The binarized models live only inside these archives, so a walk of loose
    files reports zero ODOL fixtures and the ODOL tickets look unservable by the
    local corpus -- which is wrong. The entry table alone gives the addon prefix
    AST-015 needs and, by seeking to each `.p3d` block, the exact ODOL revision
    AST-012A must dispatch on.
    """
    # Streamed, not `read_bytes()`: a game install's PBOs run to gigabytes and
    # only the entry table plus a few small seeks are needed. Reading them whole
    # would make indexing a full Arma 3 install impossible rather than merely slow.
    properties: dict[str, str] = {}
    entries: list[tuple[str, int, int]] = []
    with path.open("rb") as handle:
        header = handle.read(4 * 1024 * 1024)
        cursor = 0
        while True:
            if cursor + 4096 > len(header):
                more = handle.read(4 * 1024 * 1024)
                if not more:
                    raise Truncated("PBO entry table did not terminate")
                header += more
            name, cursor = asciiz(header, cursor)
            method, _original, _reserved, _stamp, stored = struct.unpack_from("<IIIII", header, cursor)
            cursor += 20
            if name == "" and method == 0x56657273:  # "Vers" -- the property block
                while True:
                    key, cursor = asciiz(header, cursor)
                    if not key:
                        break
                    properties[key], cursor = asciiz(header, cursor)
                continue
            if name == "" and stored == 0 and method == 0:
                break
            entries.append((name, method, stored))

        containers: dict[str, int] = {}
        suffixes: dict[str, int] = {}
        offset = cursor
        for name, method, stored in entries:
            suffix = pathlib.PurePath(name.replace("\\", "/")).suffix.lower()
            suffixes[suffix or "<none>"] = suffixes.get(suffix or "<none>", 0) + 1
            if suffix == ".p3d":
                if method == 0:
                    handle.seek(offset)
                    head = handle.read(8)
                    if len(head) == 8:
                        signature = head[:4].decode("latin1", "replace")
                        key = f"{signature}_{struct.unpack_from('<I', head, 4)[0]}"
                    else:
                        key = "TRUNCATED"
                else:
                    # A compressed block's first bytes are not the header, so it
                    # is recorded as unread rather than misreported as a revision.
                    key = "COMPRESSED_UNREAD"
                containers[key] = containers.get(key, 0) + 1
            offset += stored

    return {
        "prefix": properties.get("prefix", ""),
        "product": properties.get("product", ""),
        "entries": len(entries),
        "suffix_counts": dict(sorted(suffixes.items(), key=lambda kv: (-kv[1], kv[0]))[:12]),
        "model_containers": dict(sorted(containers.items())),
    }


SIGNATURE_PARSERS = {
    ".p3d": ("model", parse_p3d),
    ".rvmat": ("material", parse_rvmat),
    ".paa": ("texture", parse_paa),
    ".pbo": ("archive", parse_pbo),
}


# Hashing a 2 GiB PBO in full costs minutes and buys nothing an identity needs,
# so large files are identified by size plus their leading megabytes. The scope is
# recorded in the digest itself, so a partial hash can never be mistaken for a
# whole-file one.
PARTIAL_HASH_THRESHOLD = 64 * 1024 * 1024


def digest_of(path: pathlib.Path) -> tuple[str, int]:
    size = path.stat().st_size
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        if size <= PARTIAL_HASH_THRESHOLD:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(chunk)
            return digest.hexdigest(), size
        digest.update(str(size).encode())
        digest.update(handle.read(8 * 1024 * 1024))
    return "partial:" + digest.hexdigest(), size


def fixture_id(root_alias: str, relative: str, digest: str) -> str:
    """Stable across machines: alias + POSIX relative path + content hash."""
    return f"{root_alias}:{relative}@{digest[:16]}"


def index_root(alias: str, spec: dict, args) -> dict:
    raw_path = os.path.expandvars(spec["path"])
    base = pathlib.Path(raw_path)
    entry: dict = {
        "alias": alias,
        "present": base.is_dir(),
        "deep": bool(spec.get("deep")),
        "note": spec.get("note", ""),
        "generation": spec.get("generation", "unknown"),
    }
    if not entry["present"]:
        # Absent is a normal state, not a failure: CI has none of these roots.
        entry["fixtures"] = []
        entry["counts"] = {}
        return entry

    limit = spec.get("limit", args.limit)
    # A shallow root is a game install: parsing every loose file there would walk
    # tens of gigabytes for no compatibility fact that the PBO entry tables do not
    # already give. AST-007 says inventory first, parse selectively.
    parse_suffixes = set(spec.get("parse_suffixes") or (
        SIGNATURE_PARSERS if (entry["deep"] or args.deep) else {".pbo"}
    ))
    entry["parse_suffixes"] = sorted(parse_suffixes)
    counts: dict[str, int] = {}
    fixtures: list[dict] = []
    skipped = 0

    for path in sorted(base.rglob("*")):
        if not path.is_file():
            continue
        suffix = path.suffix.lower()
        counts[suffix or "<none>"] = counts.get(suffix or "<none>", 0) + 1
        if suffix not in SIGNATURE_PARSERS:
            continue
        if limit and len(fixtures) >= limit:
            skipped += 1
            continue
        kind, parser = SIGNATURE_PARSERS[suffix]
        if suffix not in parse_suffixes:
            skipped += 1
            continue
        digest, size = digest_of(path)
        relative = path.relative_to(base).as_posix()
        record = {
            "fixture_id": fixture_id(alias, relative, digest),
            "kind": kind,
            "path": relative,
            "package": relative.split("/")[1] if relative.count("/") >= 1 else relative.split("/")[0],
            "sha256": digest,
            "bytes": size,
        }
        try:
            record["detail"] = parser(path, args.deep or entry["deep"]) if kind == "model" else parser(path)
        except (Truncated, struct.error, ValueError, IndexError) as exc:
            record["detail"] = {"error": f"{type(exc).__name__}: {exc}"}
        record["capabilities"] = capabilities(record)
        record["headline"] = headline(record)
        fixtures.append(record)

    entry["counts"] = dict(sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])))
    entry["fixtures"] = fixtures
    entry["skipped_over_limit"] = skipped
    entry["companion_files"] = {
        name: counts.get(name, 0) for name in (".cfg", ".cpp", ".hpp", ".rtm", ".pbo", ".bisurf", ".wrp")
    }
    return entry


def headline(record: dict) -> dict:
    """The few numbers worth carrying in the tracked summary."""
    detail = record.get("detail", {})
    if "error" in detail:
        return {"error": detail["error"]}
    if record["kind"] == "model":
        if detail.get("container") == "ODOL":
            return {"container": "ODOL", "version": detail.get("odol_version")}
        return {
            "container": detail.get("container"),
            "lods": detail.get("lod_count"),
            "max_uv_sets": detail.get("max_uv_sets"),
            "faces": sum(lod.get("faces", 0) for lod in detail.get("lods", [])),
            "materials": detail.get("references", {}).get("material_count"),
        }
    if record["kind"] == "material":
        return {"shaders": detail.get("shader_ids", []), "stages": len(detail.get("stages", []))}
    if record["kind"] == "texture":
        return {"type": detail.get("type"), "taggs": detail.get("taggs", [])}
    if record["kind"] == "archive":
        return {
            "prefix": detail.get("prefix"),
            "entries": detail.get("entries"),
            "model_containers": detail.get("model_containers", {}),
        }
    return {}


def capabilities(record: dict) -> list[str]:
    """Capability tags the fixture can actually be used to prove."""
    detail = record.get("detail", {})
    if "error" in detail:
        return []
    tags: list[str] = []
    if record["kind"] == "model":
        container = detail.get("container")
        if container == "ODOL":
            tags.append(f"ODOL_REVISION_{detail.get('odol_version')}")
            return tags
        signatures = detail.get("lod_signatures", [])
        if "P3DM" in signatures:
            tags.append("P3DM_LOD")
        if "SP3X" in signatures or detail.get("parse_limit"):
            tags.append("SP3X_LOD")
        if detail.get("max_uv_sets", 0) >= 2:
            tags.append("MULTI_UV")
        references = detail.get("references", {})
        for prefix in references.get("virtual_prefixes", []):
            tags.append(f"VIRTUAL_PATH_{prefix.upper()}")
        for role in detail.get("lod_roles", []):
            if not role.startswith("VisualLod") and not role.startswith("Unknown"):
                tags.append(f"SPECIAL_LOD_{role.upper()}")
        if any(lod.get("named_selections") for lod in detail.get("lods", [])):
            tags.append("NAMED_SELECTIONS")
        if any("#Property#" in lod.get("tagg_counts", {}) for lod in detail.get("lods", [])):
            tags.append("NAMED_PROPERTIES")
        if any("#SharpEdges#" in lod.get("tagg_counts", {}) for lod in detail.get("lods", [])):
            tags.append("SHARP_EDGES")
    elif record["kind"] == "material":
        if detail.get("binarized"):
            return ["RVMAT_BINARIZED"]
        for shader in detail.get("shader_ids", []):
            tags.append(f"SHADER_{shader.upper()}")
        if len(detail.get("stages", [])) > 1:
            tags.append("MULTISTAGE_MATERIAL")
        if detail.get("uv_sources"):
            tags.append("TEXGEN_UVSOURCE")
        tags.extend(f"TEXROLE_{role.upper()}" for role in detail.get("texture_roles", []))
    elif record["kind"] == "texture":
        if detail.get("type"):
            tags.append(f"PAA_{detail['type']}")
        tags.extend(f"PAA_TAG_{tag}" for tag in detail.get("taggs", []))
        if detail.get("role"):
            tags.append(f"TEXROLE_{detail['role'].upper()}")
    elif record["kind"] == "archive":
        if detail.get("prefix"):
            tags.append("PBO_ADDON_PREFIX")
        tags.extend(f"CONTAINER_{key}" for key in detail.get("model_containers", {}))
    return sorted(set(tags))


def build(args) -> dict:
    raw = os.environ.get("POSEIDON_REFERENCE_CORPORA")
    if raw:
        config = json.loads(raw)
    elif CONFIG.is_file():
        config = json.loads(CONFIG.read_text(encoding="utf-8"))
    else:
        print(f"no corpus config: create {CONFIG.name} from {CONFIG.name}.example", file=sys.stderr)
        return {"roots": [], "generated": None}

    started = time.time()
    roots = [index_root(alias, spec, args) for alias, spec in sorted(config.get("roots", {}).items())]
    capability_index: dict[str, list[str]] = {}
    for root in roots:
        for fixture in root["fixtures"]:
            for tag in fixture["capabilities"]:
                capability_index.setdefault(tag, []).append(fixture["fixture_id"])

    return {
        "schema_version": 1,
        "ticket": "AST-007",
        "overlay": "COMPAT-C1-EXEC-001",
        "generator": "scripts/build_reference_fixture_index.py",
        "elapsed_seconds": round(time.time() - started, 1),
        "roots": roots,
        "capability_index": {
            tag: sorted(ids) for tag, ids in sorted(capability_index.items())
        },
    }


def compact(index: dict, exemplars: int) -> dict:
    """The tracked form of the index.

    The full parse is ~52 MB, which has no business in git and would be reviewed
    by nobody. What a compatibility ticket needs from a tracked artefact is the
    fixture identity, what each fixture can prove, and where each capability can
    be found -- not every named selection of 1600 models. The full detail is one
    command away and deterministic, so nothing is lost by regenerating it.
    """
    trimmed = json.loads(json.dumps({k: v for k, v in index.items() if k != "roots"}))
    trimmed["form"] = "COMPACT"
    trimmed["full_index_command"] = "python scripts/build_reference_fixture_index.py --full <path>"
    trimmed["capability_index"] = {
        tag: {"fixtures": len(ids), "exemplars": ids[:exemplars]}
        for tag, ids in index["capability_index"].items()
    }
    trimmed["roots"] = []
    for root in index["roots"]:
        entry = {k: v for k, v in root.items() if k != "fixtures"}
        entry["fixture_count"] = len(root["fixtures"])
        entry["fixtures"] = [
            {
                "fixture_id": f["fixture_id"], "kind": f["kind"], "path": f["path"],
                "sha256": f["sha256"], "bytes": f["bytes"],
                "headline": f["headline"], "capabilities": f["capabilities"],
            }
            for f in root["fixtures"]
        ]
        trimmed["roots"].append(entry)
    return trimmed


def summarise(index: dict) -> None:
    for root in index["roots"]:
        state = "present" if root["present"] else "ABSENT"
        print(f"\n{root['alias']}  [{state}]  generation={root['generation']}  deep={root['deep']}")
        if not root["present"]:
            continue
        print(f"  files by suffix: {dict(list(root['counts'].items())[:10])}")
        print(f"  fixtures parsed: {len(root['fixtures'])} (skipped over limit: {root.get('skipped_over_limit', 0)})")
    print(f"\ncapabilities observed: {len(index['capability_index'])}")
    for tag, fixtures in index["capability_index"].items():
        print(f"  {tag:44} {len(fixtures)}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--summary", action="store_true", help="print only, do not write the index")
    parser.add_argument("--deep", action="store_true", help="full parse for every root, not only `deep` ones")
    parser.add_argument("--limit", type=int, default=0, help="max parsed fixtures per root (0 = no limit)")
    parser.add_argument("--exemplars", type=int, default=6, help="fixture IDs kept per capability in the tracked index")
    parser.add_argument("--full", type=pathlib.Path, help="also write the untrimmed index here (not tracked)")
    args = parser.parse_args()

    index = build(args)
    summarise(index)
    if args.summary:
        return 0

    if args.full:
        args.full.parent.mkdir(parents=True, exist_ok=True)
        args.full.write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")
        print(f"\nwrote {args.full} (full)")

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(json.dumps(compact(index, args.exemplars), indent=2) + "\n", encoding="utf-8")
    size = OUTPUT.stat().st_size / 1024 / 1024
    print(f"wrote {OUTPUT.relative_to(ROOT).as_posix()} ({size:.1f} MiB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
