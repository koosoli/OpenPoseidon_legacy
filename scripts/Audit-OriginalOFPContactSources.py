"""Read-only original retail map contact census and coarse-source lane candidates.

Uses existing PoseidonTools CPU exporters; no renderer, game or asset rewrite.
Reported source qualification is not installed road/roof/contact/visibility proof.
"""
import argparse
import collections
import fnmatch
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess

MAPS = {"eden": "Everon", "noe": "Nogova", "abel": "Malden", "cain": "Kolgujev"}
REFERENCES = {"eden": {"sand": (6532, 6466), "mud": (6425, 7175)},
              "noe": {"sand": (2675, 5125), "mud": (4975, 4675)},
              "abel": {"sand": (7800, 10100), "mud": (7800, 10100)},
              "cain": {"sand": (6000, 6000), "mud": (6000, 6000)}}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def match(name, pattern):
    # Actual TextureBank.cpp PatternMatchQM6 accepts a final six ? as 6 or 0.
    return fnmatch.fnmatchcase(name, pattern) or (
        len(name) + 6 == len(pattern) and pattern.endswith("??????") and
        fnmatch.fnmatchcase(name, pattern[:-6]))


def surfaces(base, addon):
    merged = dict(base["CfgSurfaces"])
    for name, fields in addon["CfgSurfaces"].items():
        original = next((key for key in merged if key.lower() == name.lower()), None)
        inherited = merged.pop(original) if original else {}
        merged[name] = inherited | fields
    return merged


def quadrants(texture, definitions):
    pure = texture.replace("\\", "/").split("/")[-1].rsplit(".", 1)[0]
    codes = [pure[i:i+2] for i in range(0, 8, 2)] if len(pure) == 8 else [pure] * 4
    fallback = next((key for key in definitions if key.lower() == "default"), None)
    result = []
    for code in codes:
        key = next((key for key, fields in definitions.items() if match(code, fields["files"])), fallback)
        fields = definitions[key]
        result.append(dict(code=code, surfaceClass=key, files=fields["files"],
                           sound=fields["soundEnviron"], character=fields.get("character", "")))
    return result


def eligible(texture, q, kind):
    if kind == "sand":
        return all(v["sound"] == "sand" and not v["character"] and
                   (v["surfaceClass"], v["files"]) in
                   (("Sand", "ps??????"), ("SandAbel", "pi??????"), ("SandDark", "pt??????")) for v in q)
    field = all(v["surfaceClass"] == "Field" and v["files"] == "pol" and
                v["sound"] == "dirt" and not v["character"] for v in q)
    exact = texture.replace("\\", "/").lower().lstrip("/") in ("o/pole1.paa", "o/pole2.paa")
    return field or (exact and all(v["surfaceClass"].lower() == "default" and
                    v["files"] == "default" and v["sound"] == "normalExt" and not v["character"] for v in q))


def read_export(path):
    data = path.read_bytes()
    if data[:4] != b"4WVR" or struct.unpack_from("<ii", data, 4) != (256, 256):
        raise ValueError("Only exact original 256x256 50m CPU exports are supported")
    heights = struct.unpack_from("<65536h", data, 12)
    ids = struct.unpack_from("<65536H", data, 131084)
    offset = 262156
    names = [data[offset+i*32:offset+(i+1)*32].split(b"\0")[0].decode("ascii") for i in range(512)]
    offset += 512*32
    if (len(data)-offset) % 128:
        raise ValueError("Unexpected actual placement record layout")
    buckets = collections.defaultdict(list)
    for at in range(offset, len(data), 128):
        matrix = struct.unpack_from("<12f", data, at)
        ident = struct.unpack_from("<i", data, at+48)[0]
        name = data[at+52:at+128].split(b"\0")[0].decode("ascii")
        if not all(math.isfinite(v) for v in matrix):
            raise ValueError("Nonfinite source placement")
        x, y, z = matrix[9:12]
        buckets[int(x//50), int(z//50)].append(dict(id=ident, model=name, position=[x,y,z]))
    return heights, ids, names, buckets


def census(world, wrp, export, definitions):
    heights, ids, names, objects = read_export(export)
    counts = collections.Counter(ids)
    palette = {}
    for ident, count in counts.items():
        q = quadrants(names[ident], definitions)
        palette[ident] = dict(id=ident, texture=names[ident], cells=count, quadrants=q,
                             pureSand=eligible(names[ident], q, "sand"), pureMud=eligible(names[ident], q, "mud"))
    controls = {}
    for kind in ("sand", "mud", "uniform"):
        candidates = []
        for cz in range(1, 254):
            for cx in range(1, 254):
                p = palette[ids[cz*256+cx]]
                if kind != "uniform" and not p["pure"+kind.title()]:
                    continue
                h = [heights[i]*.045 for i in (cz*256+cx, cz*256+cx+1, (cz+1)*256+cx, (cz+1)*256+cx+1)]
                if min(h) <= 3 or max(h)-min(h) > 2:
                    continue
                x, z = cx*50+25, cz*50+25
                nearby = [item for dx in (-1,0,1) for dz in (-1,0,1) for item in objects[cx+dx,cz+dz]]
                if any(math.hypot(item["position"][0]-x,item["position"][2]-z) < 12 for item in nearby):
                    continue
                nearest = min(nearby, key=lambda item: math.hypot(item["position"][0]-x,item["position"][2]-z), default=None)
                candidates.append(dict(x=x,z=z,cell=[cx,cz],palette=p["id"],texture=p["texture"],
                    quadrants=p["quadrants"],sourceHeight=.5*(h[1]+h[2]),cornerHeights=h,
                    rangePer50m=(max(h)-min(h))/50,nearestSourcePlacement=nearest,
                    placementSearchScope="Owner centres in the surrounding3x3sourcecells, not model bounds/collision proof",
                    sourceLane=dict(start=[x,z],heading=90,forwardMetres=5,supportMarginToCellBoundary=20,
                        scope="5m east candidate; W-key direction may include camera-relative drift; runtime support queries required")))
        reference = REFERENCES[world].get(kind, (6000, 6000))
        candidates.sort(key=lambda v:(math.hypot(v["x"]-reference[0],v["z"]-reference[1]),v["rangePer50m"]))
        controls[kind] = dict(eligiblePaletteCells=sum(p["cells"] for p in palette.values() if kind=="uniform" or p["pure"+kind.title()]),
                             dryLandFlatSourceCandidates=len(candidates), reference=list(reference), candidates=candidates[:5],
                             status="source-qualified-runtime-pending" if candidates else "no-source-qualified-stock-candidate")
        if kind == "uniform":
            controls[kind]["scope"]="Any dry/flat source land candidate for cloth/body tests; never soft contact admission"
    return dict(world=world,displayName=MAPS[world],wrpSha256=sha(wrp),exportSha256=sha(export),grid=[256,256],landGrid=50,
                palette=sorted(palette.values(),key=lambda p:(-p["cells"],p["id"])),controls=controls)


def self_test():
    definitions = {"Default":dict(files="default",soundEnviron="normalExt"),
                   "Sand":dict(files="ps??????",soundEnviron="sand"),
                   "Field":dict(files="pol",soundEnviron="dirt"),
                   "Grass":dict(files="tn??????",soundEnviron="grass")}
    assert match("ps","ps??????") and not match("p","ps??????")
    assert eligible("eden/ps.paa",quadrants("eden/ps.paa",definitions),"sand")
    assert not eligible("eden/pspstntn.paa",quadrants("eden/pspstntn.paa",definitions),"sand")
    assert eligible("eden/pol.paa",quadrants("eden/pol.paa",definitions),"mud")
    assert eligible("o/pole1.paa",quadrants("o/pole1.paa",definitions),"mud")
    assert not eligible("mod/pole1.paa",quadrants("mod/pole1.paa",definitions),"mud")
    assert not eligible("cain/j9.paa",quadrants("cain/j9.paa",definitions),"mud")
    definitions["Field"]["character"]="grass"
    assert not eligible("eden/pol.paa",quadrants("eden/pol.paa",definitions),"mud")
    print("Source-census falsifiers PASS; no game/GPU/assets/profile used")


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--game-dir",type=Path)
    p.add_argument("--tools",type=Path)
    p.add_argument("--out",type=Path)
    p.add_argument("--self-test",action="store_true")
    args=p.parse_args()
    if args.self_test:
        self_test(); return
    if not args.game_dir or not args.tools or not args.out:
        p.error("--game-dir, --tools and --out are required")
    args.out.mkdir(parents=True,exist_ok=True)
    def tool(*values):
        subprocess.run([str(args.tools.resolve()),*map(str,values)],check=True,capture_output=True,text=True)
    base=args.game_dir/"bin/CONFIG.BIN"
    tool("config","tojson",base,"-o",args.out/"installed-config.json")
    tool("pbo","extract",args.game_dir/"AddOns/Noe.pbo",args.out/"noe","--filter","config.bin")
    tool("pbo","extract",args.game_dir/"AddOns/Noe.pbo",args.out/"noe","--filter","noe.wrp")
    addon=args.out/"noe/config.bin"
    tool("config","tojson",addon,"-o",args.out/"noe-config.json")
    definitions=surfaces(json.loads((args.out/"installed-config.json").read_text()),json.loads((args.out/"noe-config.json").read_text()))
    result=dict(status="retail-source-inference-only-runtime-pending",sourceDefinitions=definitions,
                configSha256=sha(base),noeConfigSha256=sha(addon),noeArchiveSha256=sha(args.game_dir/"AddOns/Noe.pbo"),maps={},
                limitations="CPU exporter quantizes heights .045m. Noe CfgSurfaces merged with installed base; other loaded overlays need actual dev_mud/dev_sand runtime metadata proof. Coarse pure quadrant/nearby owner centre tests cannot prove collision-free walking, actual surface slope, source support, road/roof absence, uniforms, body pixels or physical contact. No Cain/unknown alias inferred from colour.")
    for world in MAPS:
        wrp=args.out/"noe/noe.wrp" if world=="noe" else args.game_dir/("Worlds/"+world+".wrp")
        export=args.out/(world+".rvw4")
        tool("terrain","export-rvw4",wrp,export)
        result["maps"][world]=census(world,wrp,export,definitions)
    (args.out/"contact-source-census.json").write_text(json.dumps(result,indent=2)+"\n",encoding="utf-8")
    print(json.dumps({world:{kind:{k:v for k,v in rows.items() if k!="candidates"} | {"firstCandidate":rows["candidates"][:1]} for kind,rows in values["controls"].items()} for world,values in result["maps"].items()},indent=2))


if __name__=="__main__":
    main()
