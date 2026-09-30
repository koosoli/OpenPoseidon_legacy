"""CPU source/algebra bounds; deliberately NOT shader/GPU/runtime acceptance.

Read the production WGSL coefficients rather than silently freezing a separate
copy. Actual helper execution is covered by the authored GPU tests after root
obtains its serial test window.
"""
from pathlib import Path
import math
import re

root = Path(__file__).resolve().parents[1]
snow = (root / "engine/WgpuRenderer/rust/src/shaders/snow_material.wgsl").read_text()
tree = (root / "engine/WgpuRenderer/rust/src/shaders/tree_snow.wgsl").read_text()

def numbers(text):
    return [float(x.strip()) for x in text.split(",")]

means = [numbers(v) for v in re.findall(r"(?:return )?vec3<f32>\((0\.\d+, 0\.\d+, 0\.\d+)\)", snow)]
assert len(means) == 3 and all(v == means[0] for v in means), "disabled/far/resolved palettes must agree"
mean = means[0]
terms = re.findall(r"(?:broad|powder|fine|granular) \* (0\.\d+)", snow)
assert len(terms) == 4
amplitude = sum(map(float, terms))
assert amplitude <= 0.0850001
compact = float(re.search(r"clamp\(compacted, 0\.0, 1\.0\) \* (0\.\d+)", snow).group(1))
lo, hi = min(mean) * (1-amplitude) * (1-compact), max(mean) * (1+amplitude)
assert 0.50 < lo < hi < 0.90, "diffuse reflectance bound includes all noise bands and compaction"
assert "h & 0x00ffffffu" in snow and "2.0 / 16777215.0" in snow
# Convex value-noise interpolation therefore lies in [-1,1]; the preceding
# coefficient sum is a global bound, not a handful of favourable sampled sites.
assert "let u = f * f * (3.0 - 2.0 * f)" in snow
tilt = float(re.search(r"length\(tangent\) / (0\.\d+)", snow).group(1))
assert tilt <= 0.16 and "return normalize(base + bounded)" in snow
assert math.degrees(math.atan(tilt)) < 9.1
assert "if (footprint >= 1.5) { return base; }" in snow
assert "footprint >= 1.5" in snow and "snow_band(footprint, 0.035)" in snow
assert "wavelength * 0.18, wavelength * 0.5" in snow
# All positive wavelengths lose their detail before sampling could become
# unresolved point noise. This includes the new close-only granular band.
for wavelength in (3.0, 0.6, 0.12, 0.035):
    assert 0 < 0.18*wavelength < 0.5*wavelength <= 1.5

material = tree.split("fn tree_snow_albedo", 1)[1].split("fn tree_snow_cover_depth", 1)[0]
r0,r1,den = map(float,re.search(r"let relief = (0\.\d+) \+ (0\.\d+) \* lum / \(lum \+ (0\.\d+)\)", material).groups())
edge0,edge1 = map(float,re.search(r"smoothstep\((0\.\d+), (0\.\d+), lum\)", material).groups())
frost = numbers(re.search(r"let frost = vec3<f32>\(([^)]+)\)", material).group(1))
assert frost == mean and r0 > 0 and r1 > 0 and r0+r1 <= 1 and den > 0
assert 0 < edge0 < edge1 < 0.1
assert "snow_band(footprint, 1.8)" in material and "if (footprint < 0.9)" in material
assert not any(word in material for word in ("timeSeconds", "textureSample", "specular", "snow_powder_normal"))
previous = [-1.0]*3
for i in range(4097):
    lum = i/4096
    t = max(0, min(1, (lum-edge0)/(edge1-edge0)))
    recess = t*t*(3-2*t)
    relief = r0+r1*lum/(lum+den)
    rgb = [lum*(1-recess)+v*relief*recess for v in frost]
    assert all(0 <= v <= 1 for v in rgb)
    assert all(a <= b+1e-12 for a,b in zip(previous,rgb)), "photographed contrast remains monotone"
    if lum <= edge0:
        assert all(v == lum for v in rgb), "deep recesses stay authored"
    previous = rgb

print(f"CPU production-source algebra PASS: diffuse[{lo:.6f},{hi:.6f}], tilt<={math.degrees(math.atan(tilt)):.4f}deg, 4097 monotone frost levels.")
print("Actual WGSL GPU execution, composition, installed appearance and motion stability remain pending.")
