"""Read-only retail Noe church/chapel placement and coarse terrain sightline audit.

Input RVW4 and FMD1 are PoseidonTools export-rvw4 output. Runtime IDs are
recovered from the original OPRW records, never confused with exporter ordinals.
No renderer, game, placement rewrite or runtime-visibility assertion.
"""
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import struct

import numpy as np


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def height_at(grid, x, z):
    gx, gz = x / 50, z / 50
    ix, iz = math.floor(gx), math.floor(gz)
    if not (0 <= ix < 255 and 0 <= iz < 255):
        raise ValueError("sample outside original height grid")
    u, v = gx - ix, gz - iz
    a, b, c, d = grid[iz, ix], grid[iz, ix+1], grid[iz+1, ix], grid[iz+1, ix+1]
    return float(a + u*(b-a) + v*(c-a) if u+v <= 1 else
                 d + (1-u)*(c-d) + (1-v)*(b-d))


def angles(camera, point):
    dx, dy, dz = np.asarray(point) - camera
    return math.degrees(math.atan2(dx, dz)) % 360, math.degrees(math.atan2(dy, math.hypot(dx, dz)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--export", type=Path, required=True)
    parser.add_argument("--wrp", type=Path, required=True)
    parser.add_argument("--install", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location("stock_odol", repo / "scripts/physics/inspect_stock_corpse_rig.py")
    stock = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(stock)
    exported, raw = args.export.read_bytes(), args.wrp.read_bytes()
    if exported[:4] != b"4WVR" or struct.unpack_from("<II", exported, 4) != (256, 256):
        raise ValueError("exact original 256-square RVW4 required")
    if raw[:8] != b"OPRW\x03\x00\x00\x00":
        raise ValueError("exact OPRW3 required")
    meta_path = Path(str(args.export) + ".meta")
    meta = meta_path.read_bytes()
    if meta[:4] != b"FMD1":
        raise ValueError("exact float source metadata required")
    grid = np.frombuffer(meta, dtype="<f4", count=65536, offset=4+65536*9).reshape(256,256)
    if not np.isfinite(grid).all():
        raise ValueError("nonfinite authored terrain")
    camera = np.array([2592.94, 75.80, 5422.69])
    objects_start = 12+65536*4+512*32
    count = (len(exported)-objects_start)//128
    original_start = raw.find(exported[objects_start:objects_start+48])-8
    if original_start < 0 or raw[original_start+count*56:] != b"\xff\xff\xff\xff":
        raise ValueError("original placement tail lacks exact terminator")
    source_names = {}
    for i in range(count):
        offset = objects_start+i*128
        at = original_start+i*56
        if raw[at+8:at+56] != exported[offset:offset+48]:
            raise ValueError("original/export full object tail mismatch")
        actual_id, name_index = struct.unpack_from("<ii",raw,at)
        name = exported[offset+52:offset+128].split(b"\0",1)[0].decode("ascii").lower()
        if actual_id < 0 or name_index < 0 or source_names.setdefault(name_index,name) != name:
            raise ValueError("original ID/name table mapping invalid")
    caches, rows = {}, []
    for offset in range(objects_start, len(exported), 128):
        name = exported[offset+52:offset+128].split(b"\0",1)[0].decode("ascii").lower()
        if not any(token in name for token in ("kostel", "kapl", "church")):
            continue
        encoded = exported[offset:offset+48]
        location = original_start+((offset-objects_start)//128)*56+8
        actual_id, name_index = struct.unpack_from("<ii", raw, location-8)
        if actual_id < 0 or name_index < 0:
            raise ValueError("invalid original placement record")
        frame = np.array(struct.unpack("<12f", encoded)).reshape(4,3)
        if name not in caches:
            archive = args.install / ("Dta/Data3D.pbo" if name.startswith("data3d\\") else "AddOns/O.pbo")
            member = name.split("\\",1)[1]
            index, _ = stock.archive_index(archive)
            source = next(row for row in index if row["name"].lower() == member)
            with archive.open("rb") as stream:
                payload, stored_hash = stock.read_member(stream, source)
            model = stock.parse_odol7(payload)
            visual = [lod for lod in model["lods"] if lod["resolution"] < 900]
            if not visual:
                raise ValueError("model has no actual visual LOD")
            caches[name] = (model, dict(archive=str(archive), archiveSha256=sha(archive),
                member=member, decodedSha256=hashlib.sha256(payload).hexdigest(), storedSha256=stored_hash))
        model, source = caches[name]
        pos = frame[3].copy()
        anchor = pos - np.array(model["boundingCenter"]) @ frame[:3]
        lowering = max(0, anchor[1] - height_at(grid, anchor[0], anchor[2]))
        pos[1] -= lowering
        visual = [lod for lod in model["lods"] if lod["resolution"] < 900]
        all_world = np.concatenate([lod["points"] @ frame[:3] + pos for lod in visual])
        foundation = []
        for lod in visual:
            points = lod["points"]
            bottom = points[points[:,1] <= points[:,1].min()+.1] @ frame[:3] + pos
            gaps = [float(p[1]-height_at(grid,p[0],p[2])) for p in bottom]
            foundation.append(dict(resolution=lod["resolution"], bottomVertices=len(bottom),
                minimumTerrainGap=min(gaps), maximumTerrainGap=max(gaps)))
        target = np.array([pos[0], all_world[:,1].max(), pos[2]])
        delta = target-camera
        planar = math.hypot(delta[0], delta[2])
        samples = []
        for t in np.linspace(.01,.99,max(2,math.ceil(planar/5))):
            p = camera+t*delta
            samples.append((height_at(grid,p[0],p[2])-p[1], t, p))
        obstruction = max(samples, key=lambda row:row[0])
        yaw, elevation = angles(camera, target)
        yaw_delta = (yaw-100+180)%360-180
        # Useful follow-up poses, not proof of loaded scene visibility.
        near = pos + np.array([-60., 0, 0])
        near[1] = height_at(grid,near[0],near[2])+4
        near_target = pos.copy()
        near_target[1] = all_world[:,1].min()+3
        near_yaw, near_el = angles(near, near_target)
        rows.append(dict(id=actual_id, exporterOrdinal=struct.unpack_from("<i",exported,offset+48)[0],
            originalRecordOffset=location-8, originalNameIndex=name_index, model=name, source=source,
            wrpFrame=frame.tolist(), coarsePredictedPosition=pos.tolist(), legacyLowering=lowering,
            modelBoundingCentre=model["boundingCenter"], autoCenter=model["autoCenter"],
            lockAutoCenter=model["lockAutoCenter"], planarDistance=planar,
            centreTerrain=height_at(grid,pos[0],pos[2]), visualTopY=float(target[1]),
            topAzimuth=yaw, topElevation=elevation, azimuthOffset=yaw_delta,
            elevationOffset=elevation-6.2, foundationLods=foundation,
            coarseTopRayMaximumTerrainOverRay=float(obstruction[0]),
            coarseTopRayWorstPoint=obstruction[2].tolist(),
            candidateNearPose=[float(near[0]),float(near[2]),float(near[1]),near_yaw,near_el],
            candidateReportedPositionTargetedPose=[camera[0],camera[2],camera[1],yaw,elevation]))
    rows.sort(key=lambda row:row["planarDistance"])
    result = dict(status="source-only-identification-unresolved", cameraXZYaEl=[2592.94,5422.69,75.8,100,6.2],
        wrpSha256=sha(args.wrp), exportSha256=sha(args.export), metadataSha256=sha(meta_path),
        sourceGridSpacing=50, originalObjectCount=count, originalObjectTailOffset=original_start,
        originalFullObjectTailVerified=True,
        limitations=["Original coarse grid, not installed subdivided height/cdlod raster",
            "No object occluders in ray audit", "No screen-FOV assumption or pixel identification",
            "Predicted legacy placement must be checked against actual stream_identity_probe"], objects=rows)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps(dict(objects=len(rows), near=rows[0]["id"], sourceSha=result["wrpSha256"])))


if __name__ == "__main__":
    main()
