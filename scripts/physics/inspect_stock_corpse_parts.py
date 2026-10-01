#!/usr/bin/env python3
"""Read-only authored special-LOD parts; never admits joints or a runtime rig."""

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

import inspect_stock_corpse_rig as rig


def components(lod):
    result = []
    for name, selection in sorted(lod["selections"].items()):
        if not name.startswith("component"):
            continue
        indices = sorted(i for i, weight in selection.items() if weight > 0)
        if not indices:
            raise rig.Refused("empty authored component")
        points = lod["points"][indices]
        if not np.isfinite(points).all():
            raise rig.Refused("nonfinite authored component")
        influences = {}
        for bone in rig.STOCK_BONES:
            membership = lod["selections"].get(bone, {})
            count = sum(membership.get(i, 0) > 0 for i in indices)
            if count:
                influences[bone] = count
        full_single = [bone for bone, count in influences.items()
                       if count == len(indices) and
                       all(lod["selections"][bone][i] == 255 for i in indices)]
        result.append(dict(name=name, vertices=len(indices),
                           boundsMin=points.min(axis=0).tolist(),
                           boundsMax=points.max(axis=0).tolist(),
                           positiveBoneVertices=influences,
                           exclusiveFullWeightBone=(full_single[0] if len(influences) == 1 and
                                                    len(full_single) == 1 else None)))
    return result


def inspect(archive, member):
    rows, _ = rig.archive_index(archive)
    row = next((row for row in rows if row["name"].lower() == member.lower()), None)
    if row is None:
        raise rig.Refused("missing requested member")
    with archive.open("rb") as stream:
        payload, stored_sha = rig.read_member(stream, row)
    model = rig.parse_odol7(payload)
    roles = {}
    for role in ("geometry", "fireGeometry", "viewGeometry"):
        index = model["roles"][role]
        roles[role] = dict(lod=index, components=components(model["lods"][index]) if index >= 0 else [])
    return dict(member=row["name"], storedSha256=stored_sha,
                decodedSha256=hashlib.sha256(payload).hexdigest(),
                boundingCenter=model["boundingCenter"], autoCenter=model["autoCenter"],
                lockAutoCenter=model["lockAutoCenter"], roles=roles,
                coordinateSpace="raw ODOL7 before engine recentering; no animation or world transform",
                componentScope="authored named membership only; convexity/inertia/contact fit not admitted",
                jointFrameAdmitted=False, runtimeRigAdmitted=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--member", default="mc vojakw2.p3d")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    encoded = json.dumps(inspect(args.archive, args.member), indent=2) + "\n"
    if args.out:
        args.out.write_text(encoded, encoding="utf-8")
    else:
        print(encoded, end="")


if __name__ == "__main__":
    main()
