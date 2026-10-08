"""CPU evaluation of the original FX helper's actual return expressions; no GPU."""
from pathlib import Path
import math
import random
import re
import unittest

SOURCE = (Path(__file__).parents[2] / "tools/reshade-depth/OpenPoseidonSceneDepth.fxh").read_text()


class Vector(tuple):
    def __new__(cls, *values):
        return tuple.__new__(cls, [x for v in values for x in (v if isinstance(v, tuple) else (v,))])
    def __getattr__(self, name):
        return self["xyzw".index(name)]
    def apply(self, other, operation):
        return Vector(*(operation(a, b) for a, b in zip(self, other if isinstance(other, tuple) else (other,) * len(self))))
    def __add__(self, other): return self.apply(other, lambda a, b: a + b)
    def __sub__(self, other): return self.apply(other, lambda a, b: a - b)
    def __mul__(self, other): return self.apply(other, lambda a, b: a * b)
    def __truediv__(self, other): return self.apply(other, lambda a, b: a / b)


def execute(name, **values):
    # These helpers are straight-line scalar/vector arithmetic. Evaluate the
    # actual shader expressions, including its branch, rather than repeat math.
    body = re.search(r"float[23]? " + name + r"\([^)]*\)\s*\{(.*?)\n\}", SOURCE, re.S).group(1)
    body = re.sub(r"//[^\n]*", "", body)
    scope = dict(values, float2=Vector, float3=Vector)
    def expression(text):
        text = text.strip().replace("&&", " and ")
        if "?" in text:
            condition, branches = text.split("?", 1)
            yes, no = branches.split(":", 1)
            text = f"({yes} if {condition} else {no})"
        return eval(text, {"__builtins__": {}}, scope)
    for statement in body.split(";"):
        statement = statement.strip()
        if statement.startswith("return "):
            return expression(statement[7:])
        if statement:
            variable, value = statement.split("=", 1)
            variable = variable.split()[-1]
            if "tex2Dlod" not in value:  # caller supplies the sampled raw scalar
                scope[variable] = expression(value)
    raise AssertionError("helper returned nothing")


class Reconstruction(unittest.TestCase):
    def inputs(self, scale=1000, offset=(0, 0)):
        return dict(OpSceneProjection=Vector(1.0714285, 1.904762, .0957142934, 0),
                    OpSceneOffset=Vector(*offset), OpSceneRange=scale, OpSceneReady=True)

    def test_actual_forward_reverse_pipeline_recovers_metric_position(self):
        randomizer = random.Random(42)
        for offset in [(0, 0), (.025, -.04)]:
            inputs = self.inputs(offset=offset)
            px, py, near, _ = inputs["OpSceneProjection"]
            for z in [.1, .77, 2, 10, 95, 500, 999]:
                for _ in range(12):
                    x, y = randomizer.uniform(-.7, .7) * z, randomizer.uniform(-.4, .4) * z
                    # Actual vertex path: forward infinite, z = w - z, divide.
                    clip = Vector(px*x + offset[0]*z, py*y + offset[1]*z, z-near, z)
                    raw = (clip.w - clip.z) / clip.w
                    uv = Vector((clip.x/clip.w+1)/2, (1-clip.y/clip.w)/2)
                    depth = execute("OpSceneLinearDepth", uv=uv, raw=raw, **inputs)
                    position = execute("OpSceneViewPosition", uv=uv, normalizedDepth=depth, **inputs)
                    for got, expected in zip(position, (x, -y, z)):
                        self.assertAlmostEqual(got, expected, delta=max(1e-7, z*1e-10))
                    screen = execute("OpSceneScreenPosition", position=position, **inputs)
                    self.assertAlmostEqual(screen.x, uv.x, places=12)
                    self.assertAlmostEqual(screen.y, uv.y, places=12)
                    self.assertAlmostEqual(screen.z, depth, places=12)

    def test_working_range_changes_normalization_without_distorting_metres(self):
        uv = Vector(.15, .8)
        positions = []
        for scale in (100, 1000, 10000):
            inputs = self.inputs(scale)
            depth = execute("OpSceneLinearDepth", uv=uv, raw=inputs["OpSceneProjection"].z/25, **inputs)
            positions.append(execute("OpSceneViewPosition", uv=uv, normalizedDepth=depth, **inputs))
            self.assertAlmostEqual(depth, 25/scale)
        self.assertEqual(positions[0], positions[1])
        self.assertEqual(positions[1], positions[2])
        # Screen-down reflection preserves metric lengths and angles.
        self.assertAlmostEqual(math.dist(positions[0], (0,0,0)), math.sqrt(sum(x*x for x in positions[0])))

    def test_sky_and_missing_binding_never_divide_by_zero(self):
        inputs = self.inputs()
        for ready, raw in [(True, 0), (False, .5), (False, 0)]:
            inputs["OpSceneReady"] = ready
            self.assertEqual(execute("OpSceneLinearDepth", uv=Vector(.5,.5), raw=raw, **inputs), 2)


if __name__ == "__main__":
    unittest.main()
