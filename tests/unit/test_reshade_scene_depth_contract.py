"""CPU-only validation of the built optional bridge; never registers or uses a GPU.

Set OP_SCENE_DEPTH_TEST_ADDON to this worktree's freshly built .addon64.
Only the POD publication/query/invalidation exports run; AddonInit is not called.
"""
import ctypes as C
import os
from pathlib import Path
import unittest


class Publication(C.Structure):
    _fields_ = [("bytes", C.c_uint32), ("version", C.c_uint32)] + [
        (name, C.c_uint64) for name in (
            "generation", "frame", "device", "queue", "image", "view", "sampleFrame", "sampleGeneration")
    ] + [(name, C.c_uint32) for name in (
        "width", "height", "samples", "flags", "finiteSamples", "nonzeroSamples", "gridWidth", "gridHeight")
    ] + [(name, C.c_float) for name in ("nearPlane", "minimum", "maximum", "reserved")] + [
        ("projection", C.c_float * 16), ("grid", C.c_float * 256)]


class State(C.Structure):
    _fields_ = [("publication", Publication), ("boundFrame", C.c_uint64), ("boundGeneration", C.c_uint64),
                ("status", C.c_uint32), ("reserved", C.c_uint32)]


@unittest.skipUnless(os.name == "nt" and os.environ.get("OP_SCENE_DEPTH_TEST_ADDON"), "explicit local CPU bridge artifact required")
class Contract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = C.CDLL(str(Path(os.environ["OP_SCENE_DEPTH_TEST_ADDON"]).resolve()))
        cls.publish = cls.library.OpReShadePublishSceneDepthV1
        cls.publish.argtypes = [C.POINTER(Publication), C.c_uint32, C.c_uint32]
        cls.publish.restype = C.c_uint32
        cls.query = cls.library.OpReShadeGetSceneDepthV1
        cls.query.argtypes = [C.POINTER(State), C.c_uint32, C.c_uint32]
        cls.query.restype = C.c_uint32
        cls.invalidate = cls.library.OpReShadeInvalidateSceneDepthV1
        cls.invalidate.argtypes = []
        cls.invalidate.restype = None

    def setUp(self):
        self.invalidate()  # No AddonInit => no runtime/device/queue exists here.

    def publication(self):
        p = Publication(bytes=C.sizeof(Publication), version=1, generation=1, frame=1,
                        device=11, queue=12, image=13, view=14, width=1600, height=900,
                        samples=1, flags=1, gridWidth=16, gridHeight=16, nearPlane=0.1)
        p.projection[10] = p.projection[11] = 1
        p.projection[0] = p.projection[5] = 1
        p.projection[14] = -0.1
        return p

    def test_layout_and_abi_rejections(self):
        self.assertEqual(C.sizeof(Publication), 1208)
        self.assertEqual(Publication.grid.offset, 184)
        self.assertEqual(C.sizeof(State), 1232)
        p = self.publication()
        self.assertEqual(self.publish(C.byref(p), 1200, 1), 0)
        self.assertEqual(self.publish(C.byref(p), 1208, 2), 0)
        p.view = 0
        self.assertEqual(self.publish(C.byref(p), 1208, 1), 0)
        p.view = 14
        p.projection[0] = 0
        self.assertEqual(self.publish(C.byref(p), 1208, 1), 0)

    def test_publication_is_not_binding_and_rejects_stale_or_invalid_witness(self):
        p = self.publication()
        self.assertEqual(self.publish(C.byref(p), 1208, 1), 1)
        state = State()
        self.assertEqual(self.query(C.byref(state), 1232, 1), 1)
        self.assertEqual(state.publication.image, 13)
        self.assertEqual(state.status, 1)
        self.assertEqual(state.boundFrame, 0)  # Handles alone never establish binding.
        self.assertEqual(self.publish(C.byref(p), 1208, 1), 0)  # Duplicate frame.
        p.frame = 2
        p.flags = 3
        p.sampleFrame = 1
        p.sampleGeneration = 1
        p.finiteSamples = 256
        p.nonzeroSamples = 100
        p.maximum = 0.05
        for i in range(100):
            p.grid[i] = 0.05
        self.assertEqual(self.publish(C.byref(p), 1208, 1), 1)
        p.frame = 3
        p.nonzeroSamples = 99  # A claimed statistic must match the actual grid.
        self.assertEqual(self.publish(C.byref(p), 1208, 1), 0)
        p.nonzeroSamples = 100
        p.grid[0] = float("nan")
        self.assertEqual(self.publish(C.byref(p), 1208, 1), 0)
        self.invalidate()
        self.assertEqual(self.query(C.byref(state), 1232, 1), 1)
        self.assertEqual(state.status, 0)
        self.assertEqual(state.publication.view, 0)


if __name__ == "__main__":
    unittest.main()
