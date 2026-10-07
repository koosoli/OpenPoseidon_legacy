#!/usr/bin/env python3
"""Regenerate the inactive inventory vector draft from original primitives.

No source game textures, screenshots, trademarks or donor artwork are used.
The same scene commands produce editable SVG sources and antialiased RGBA PNGs.
These vectors are historical draft art, NOT the default inventory icons.
Outputs are confined to content/inventory-icons/vector-draft; active realistic art
in assets/inventory is never written. Requires Pillow; --check verifies the draft.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "content/inventory-icons/vector-draft"
OUTPUT = SOURCE / "png"
INK = "#20272b"
STEEL = "#69787d"
LIGHT = "#bac8c9"
DARK = "#3d4b50"
OLIVE = "#6b7652"
OLIVE_LIGHT = "#a8b28a"
WOOD = "#99704f"
Scene = list[dict]


def polygon(scene: Scene, points, fill, stroke=INK, width=2):
    scene.append(dict(kind="polygon", points=points, fill=fill, stroke=stroke, width=width))


def rect(scene: Scene, box, fill, stroke=INK, width=2, radius=0):
    scene.append(dict(kind="rect", box=box, fill=fill, stroke=stroke, width=width, radius=radius))


def ellipse(scene: Scene, box, fill, stroke=INK, width=2):
    scene.append(dict(kind="ellipse", box=box, fill=fill, stroke=stroke, width=width))


def line(scene: Scene, points, stroke=LIGHT, width=2):
    scene.append(dict(kind="line", points=points, fill=None, stroke=stroke, width=width))


def gun(kind: str) -> Scene:
    s: Scene = []
    ak = kind == "ak_rifle"
    smg = kind == "smg"
    mg = kind == "machinegun"
    scope = kind == "scoped_rifle"
    wood = WOOD if ak else DARK
    # Independent generic engineering silhouette, not a real manufacturer's logo/model.
    polygon(s, [(20, 114), (63, 111), (83, 120), (74, 137), (54, 135), (21, 148)], wood)
    line(s, [(26, 121), (52, 120), (63, 123)], "#b3a28c" if ak else STEEL, 3)
    polygon(s, [(66, 137), (76, 136), (73, 145), (64, 146)], INK)
    polygon(s, [(81, 128), (95, 130), (87, 154), (77, 152)], wood)
    polygon(s, [(91, 133), (107, 133), (108, 143), (95, 143)], STEEL)
    rect(s, (96, 134, 104, 139), INK, INK, 1, 2)
    rect(s, (66, 111, 148 if not smg else 139, 130), STEEL, radius=3)
    polygon(s, [(73, 107), (117, 107), (125, 112), (70, 112)], DARK)
    line(s, [(74, 115), (133, 115)], LIGHT, 3)
    rect(s, (103, 119, 128, 124), DARK, width=1, radius=2)
    ellipse(s, (83, 118, 88, 123), LIGHT, width=1)
    fore_end = 175 if smg else 194
    rect(s, (144 if not smg else 135, 113, fore_end, 128), wood, radius=3)
    for x in range(150 if not smg else 142, fore_end - 5, 8):
        line(s, [(x, 116), (x, 124)], STEEL if not ak else "#c19b74", 2)
    rect(s, (fore_end, 117, 233 if not smg else 215, 122), STEEL, width=1)
    rect(s, (230 if not smg else 212, 115, 242 if not smg else 225, 124), DARK, width=1)
    rect(s, (182 if not smg else 166, 105, 187 if not smg else 171, 118), DARK, width=1)
    polygon(s, [(176 if not smg else 160, 106), (194 if not smg else 178, 106), (186 if not smg else 170, 102)], STEEL, width=1)
    if ak:
        polygon(s, [(113, 129), (129, 129), (136, 151), (144, 161), (135, 168), (122, 156)], DARK)
        line(s, [(121, 135), (127, 151), (136, 160)], STEEL, 3)
        line(s, [(131, 104), (176, 108)], STEEL, 4)
    elif mg:
        rect(s, (112, 129, 140, 158), OLIVE, radius=3)
        line(s, [(117, 136), (135, 136)], OLIVE_LIGHT, 3)
        line(s, [(185, 128), (178, 155)], STEEL, 3)
        line(s, [(187, 128), (199, 154)], STEEL, 3)
        rect(s, (91, 101, 103, 110), DARK, radius=2)
    else:
        polygon(s, [(113, 129), (128, 129), (124, 158 if not smg else 165), (110, 156 if not smg else 164)], DARK)
        line(s, [(117, 137), (115, 151)], STEEL, 3)
    if scope:
        rect(s, (99, 96, 105, 108), DARK, width=1)
        rect(s, (135, 96, 141, 108), DARK, width=1)
        rect(s, (87, 87, 152, 99), STEEL, radius=5)
        rect(s, (85, 84, 98, 102), DARK, radius=4)
        rect(s, (143, 83, 159, 103), DARK, radius=5)
        line(s, [(103, 90), (137, 90)], LIGHT, 2)
        rect(s, (118, 82, 126, 89), STEEL, radius=2)
    return s


def scenes() -> dict[str, Scene]:
    result = {k: gun(k) for k in ("rifle", "ak_rifle", "scoped_rifle", "smg", "machinegun")}
    s: Scene = []
    polygon(s, [(70, 137), (90, 137), (81, 162), (68, 159)], DARK)
    polygon(s, [(111, 139), (128, 139), (127, 151), (114, 151)], STEEL)
    rect(s, (18, 108, 235, 137), OLIVE, radius=12)
    rect(s, (20, 104, 43, 141), DARK, radius=7)
    rect(s, (211, 104, 237, 141), DARK, radius=6)
    line(s, [(50, 114), (203, 114)], OLIVE_LIGHT, 4)
    line(s, [(53, 132), (202, 132)], "#455237", 3)
    rect(s, (116, 97, 128, 109), STEEL, radius=2)
    rect(s, (115, 91, 133, 99), DARK, radius=2)
    for x in [52, 191]: rect(s, (x, 107, x + 5, 138), STEEL, width=1)
    result["launcher"] = s
    s = []
    polygon(s, [(91, 119), (133, 119), (122, 176), (91, 174), (100, 138), (77, 134)], DARK)
    polygon(s, [(102, 142), (123, 143), (116, 168), (97, 167)], OLIVE)
    for y in [149, 156, 163]: line(s, [(102, y), (117, y)], STEEL, 2)
    polygon(s, [(82, 96), (175, 96), (177, 114), (139, 116), (131, 126), (84, 126)], STEEL)
    rect(s, (88, 93, 175, 108), DARK, radius=3)
    line(s, [(96, 97), (167, 97)], LIGHT, 3)
    for x in [94, 100, 106]: line(s, [(x, 102), (x, 113)], STEEL, 2)
    rect(s, (159, 88, 168, 96), STEEL, width=1, radius=1)
    polygon(s, [(133, 121), (152, 121), (150, 140), (126, 140)], STEEL)
    rect(s, (135, 125, 146, 135), INK, radius=3)
    result["pistol"] = s
    s = []
    for x in [61, 139]:
        polygon(s, [(x+9, 79), (x+41, 79), (x+47, 145), (x+42, 162), (x, 162), (x-5, 145)], OLIVE)
        rect(s, (x+6, 72, x+43, 90), DARK, radius=7)
        ellipse(s, (x-4, 140, x+46, 175), DARK)
        ellipse(s, (x+2, 146, x+40, 169), "#425e67", LIGHT, 2)
        line(s, [(x+10, 153), (x+29, 153)], "#89b4c0", 3)
        line(s, [(x+7, 98), (x+9, 132)], OLIVE_LIGHT, 4)
    rect(s, (106, 101, 139, 120), DARK, radius=4)
    rect(s, (119, 93, 129, 128), STEEL, radius=3)
    result["binoculars"] = s
    s = []
    polygon(s, [(48, 94), (76, 71), (179, 71), (208, 96), (201, 128), (55, 128)], DARK)
    rect(s, (90, 64, 166, 82), OLIVE, radius=5)
    line(s, [(99, 69), (155, 69)], OLIVE_LIGHT, 3)
    for x in [58, 141]:
        rect(s, (x, 92, x+58, 163), OLIVE, radius=10)
        ellipse(s, (x+1, 136, x+57, 177), DARK)
        ellipse(s, (x+9, 144, x+49, 169), "#315b4c", LIGHT, 2)
        ellipse(s, (x+19, 149, x+39, 163), "#77a88b", width=1)
        line(s, [(x+11, 104), (x+43, 104)], OLIVE_LIGHT, 3)
    result["nvg"] = s
    s = []
    polygon(s, [(98, 56), (160, 59), (153, 198), (92, 194)], STEEL)
    polygon(s, [(99, 61), (157, 64), (155, 79), (98, 76)], LIGHT)
    polygon(s, [(103, 85), (146, 87), (141, 179), (99, 177)], DARK)
    for x in [110, 125, 140]: line(s, [(x, 94), (x-4, 169)], STEEL, 4)
    line(s, [(98, 186), (147, 189)], LIGHT, 3)
    result["magazine"] = s
    s = []
    polygon(s, [(95, 51), (143, 53), (148, 91), (162, 128), (187, 163), (160, 197), (131, 169), (110, 127), (99, 88)], DARK)
    polygon(s, [(97, 54), (141, 56), (143, 73), (99, 72)], STEEL)
    for dx in [8, 20, 32]:
        line(s, [(99+dx, 84), (107+dx, 122), (123+dx, 156), (149+dx, 182)], STEEL, 3)
    line(s, [(163, 184), (179, 165)], LIGHT, 3)
    result["curved_magazine"] = s
    s = []
    ellipse(s, (89, 89, 169, 191), OLIVE)
    rect(s, (112, 70, 146, 100), DARK, radius=6)
    polygon(s, [(134, 75), (168, 84), (176, 155), (165, 157), (157, 94), (132, 87)], STEEL)
    ellipse(s, (91, 66, 121, 89), None, LIGHT, 4)
    for y, x in [(113,96), (134,92), (155,95), (175,104)]: line(s, [(x,y), (154,y)], "#46533a", 4)
    line(s, [(116, 100), (111, 181)], OLIVE_LIGHT, 4)
    result["grenade"] = s
    s = []
    rect(s, (88, 93, 167, 192), "#859184", radius=12)
    rect(s, (99, 77, 158, 98), DARK, radius=5)
    rect(s, (93, 121, 162, 155), "#d1d5c6", radius=1)
    line(s, [(105,131), (150,131)], DARK, 5)
    line(s, [(105,144), (138,144)], DARK, 4)
    polygon(s, [(140, 78), (175, 89), (179, 151), (168, 151), (162, 99), (137, 90)], STEEL)
    ellipse(s, (104, 67, 129, 84), None, LIGHT, 3)
    line(s, [(98, 105), (98, 114)], LIGHT, 3)
    result["smoke"] = s
    s = []
    rect(s, (62, 87, 194, 177), OLIVE, radius=11)
    rect(s, (106, 69, 153, 92), DARK, radius=6)
    rect(s, (114, 76, 145, 88), None, OLIVE_LIGHT, 3, 2)
    polygon(s, [(65, 93), (190, 93), (180, 116), (77, 116)], "#89916c")
    for x in [82, 161]:
        rect(s, (x, 94, x+12, 174), DARK, width=1)
        rect(s, (x-2, 125, x+14, 144), STEEL, width=2, radius=2)
        rect(s, (x+2, 129, x+10, 140), OLIVE, width=1)
    line(s, [(72, 166), (185, 166)], OLIVE_LIGHT, 3)
    result["satchel"] = s
    s = []
    ellipse(s, (53, 112, 203, 176), DARK)
    rect(s, (54, 106, 202, 145), OLIVE, radius=15)
    ellipse(s, (54, 86, 202, 148), OLIVE)
    ellipse(s, (75, 96, 181, 137), "#7e8860", width=2)
    ellipse(s, (111, 106, 145, 127), STEEL, width=2)
    line(s, [(67, 124), (83, 132)], OLIVE_LIGHT, 3)
    line(s, [(62, 148), (81, 158)], LIGHT, 3)
    result["mine"] = s
    s = []
    polygon(s, [(44, 109), (73, 98), (119, 103), (137, 113), (217, 116), (239, 125), (217, 134), (137, 137), (119, 147), (73, 152), (44, 141)], OLIVE)
    polygon(s, [(44, 109), (73, 98), (119, 103), (137, 113), (137, 125), (55, 125)], OLIVE_LIGHT)
    rect(s, (141, 117, 220, 132), STEEL, width=1, radius=2)
    polygon(s, [(186, 117), (207, 96), (216, 117)], DARK)
    polygon(s, [(187, 132), (208, 153), (216, 132)], DARK)
    line(s, [(75, 109), (117, 112)], "#cad2ab", 3)
    result["rocket"] = s
    return result


def svg(scene: Scene) -> str:
    rows = ['<svg xmlns="http://www.w3.org/2000/svg" width="512" height="512" viewBox="0 0 256 256">',
            '<title>Original Open Poseidon Engine inventory icon</title>',
            '<metadata>GPL-3.0-or-later; original vector geometry, no game-derived artwork.</metadata>']
    for c in scene:
        fill = c.get("fill") or "none"
        style = f'fill="{fill}" stroke="{c["stroke"]}" stroke-width="{c["width"]}" stroke-linejoin="round" stroke-linecap="round"'
        if c["kind"] in ("polygon", "line"):
            points = " ".join(f"{x},{y}" for x,y in c["points"])
            tag = "polygon" if c["kind"] == "polygon" else "polyline"
            rows.append(f'<{tag} points="{points}" {style}/>')
        else:
            x0,y0,x1,y1 = c["box"]
            if c["kind"] == "ellipse":
                rows.append(f'<ellipse cx="{(x0+x1)/2}" cy="{(y0+y1)/2}" rx="{(x1-x0)/2}" ry="{(y1-y0)/2}" {style}/>')
            else:
                rows.append(f'<rect x="{x0}" y="{y0}" width="{x1-x0}" height="{y1-y0}" rx="{c.get("radius",0)}" {style}/>')
    return "\n".join(rows)+"\n</svg>\n"


def png(scene: Scene) -> bytes:
    import io
    scale = 8
    image = Image.new("RGBA", (256*scale,256*scale))
    d = ImageDraw.Draw(image)
    for c in scene:
        width = c["width"]*scale
        fill = c.get("fill")
        if c["kind"] in ("polygon", "line"):
            points = [(x*scale,y*scale) for x,y in c["points"]]
            if c["kind"] == "polygon":
                d.polygon(points, fill=fill)
                d.line(points+[points[0]], fill=c["stroke"], width=width, joint="curve")
            else:
                d.line(points, fill=c["stroke"], width=width, joint="curve")
        else:
            box = tuple(v*scale for v in c["box"])
            if c["kind"] == "ellipse": d.ellipse(box, fill=fill, outline=c["stroke"], width=width)
            else: d.rounded_rectangle(box, radius=c.get("radius",0)*scale, fill=fill, outline=c["stroke"], width=width)
    image = image.resize((512,512),Image.Resampling.LANCZOS)
    output = io.BytesIO()
    image.save(output,format="PNG", optimize=False, compress_level=9)
    return output.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    definitions = scenes()
    pending = {}
    entries = []
    for name, commands in definitions.items():
        raw_png = png(commands)
        pending[SOURCE/f"{name}.svg"] = svg(commands).encode()
        pending[OUTPUT/f"{name}.png"] = raw_png
        entries.append(dict(category=name, svg=f"{name}.svg", png=f"content/inventory-icons/vector-draft/png/{name}.png",
                            sha256=hashlib.sha256(raw_png).hexdigest(), dimensions=[512,512], transparency="RGBA"))
    manifest = dict(name="Open Poseidon Engine inactive vector inventory draft", version=1, active=False,
                    license="GPL-3.0-or-later", additional_terms="Project LICENSE Section 7 terms apply.",
                    provenance="Original geometry authored for Open Poseidon Engine; no external source image or game texture inputs.",
                    generator="scripts/default-content/generate_inventory_icons.py", icons=entries)
    pending[SOURCE/"manifest.json"] = (json.dumps(manifest,indent=2)+"\n").encode()
    if args.check:
        mismatches=[str(path.relative_to(ROOT)) for path,data in pending.items() if not path.exists() or path.read_bytes()!=data]
        if mismatches: raise SystemExit("Missing/stale generated inventory files: "+", ".join(mismatches))
        print(f"Verified {len(entries)} inactive draft icons and manifest; active assets untouched.")
    else:
        for path,data in pending.items():
            path.parent.mkdir(parents=True,exist_ok=True)
            path.write_bytes(data)
        print(f"Generated {len(entries)} inactive draft SVG/PNG icons; active assets untouched.")


if __name__ == "__main__": main()
