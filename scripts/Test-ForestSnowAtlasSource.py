"""CPU algebra checks of authored atlas rectangles extracted from production.

This does not execute WGSL. The mandatory production-helper GPU probe is separate.
"""
from pathlib import Path
import math
import re

repo = Path(__file__).resolve().parent.parent
source = (repo / "engine/WgpuRenderer/rust/src/shaders/tree_snow.wgsl").read_text()
width = float(re.search(r"let edge = vec2<f32>\(([^)]+)\)", source).group(1))
assert width == 0.004

def smooth(a, b, x):
    t = max(0.0, min(1.0, (x-a)/(b-a)))
    return t*t*(3.0-2.0*t)

def rectangles(kind):
    block = source.split(f"if (category == {kind}u)", 1)[1].split(f"if (category == {kind+1}u)", 1)[0]
    return [tuple(map(float, row)) for row in re.findall(
        r"forest_snow_crown_rect\(uv,vec2<f32>\(([^,]+),([^)]+)\),vec2<f32>\(([^,]+),([^)]+)\)\)", block)]

def cover(rects, u, v):
    return max((smooth(x0,x0+width,u)*smooth(y0,y0+width,v)*
                (1-smooth(x1-width,x1,u))*(1-smooth(y1-width,y1,v))
                for x0,y0,x1,y1 in rects), default=0.0)

negative = {
    3: ((.9,.9), (.65,.94), (.375,.375), (.15,.21), (.9,.65)),
    4: ((.3,.375), (.375,.8), (.1,.47), (.9,.49), (.625,.9)),
}
positive = {3: ((.375,.1), (.625,.1), (.1,.375), (.375,.625)),
            4: ((.625,.25), (.625,.625), (.375,.1), (.375,.625))}
samples = 0
for kind in (3,4):
    rects = rectangles(kind)
    assert len(rects) >= 12, "actual near atlas masks must exist"
    assert all(0 <= x0 < x1 <= 1 and 0 <= y0 < y1 <= 1 for x0,y0,x1,y1 in rects)
    for uv in negative[kind]:
        assert cover(rects,*uv) == 0, (kind, "bark/ground/exposed trunk", uv)
    for uv in positive[kind]:
        assert cover(rects,*uv) == 1, (kind, "authored foliage/top", uv)
    for x in range(257):
        for y in range(257):
            value = cover(rects,x/256,y/256)
            assert math.isfinite(value) and 0 <= value <= 1
            samples += 1

assert "sampler_index & 1u" in source and "sampler_index & 2u" in source
assert "fract(uv.x), clamp(uv.x,0.0,1.0)" in source
assert "fract(uv.y), clamp(uv.y,0.0,1.0)" in source
assert "sampler_index > 7u" in source
assert "weather_map_coverage(world_abs)" in source and "weather_map_reach(world_abs)" in source
print(f"CPU actual-source rectangle algebra PASS: {samples} bounded samples, foliage positives and explicit bark/ground/trunk negatives.")
print("WGSL execution, installed forest appearance, top-view coverage and performance remain separate runtime gates.")
