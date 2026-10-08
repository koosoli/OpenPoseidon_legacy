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


class TimingSample(C.Structure):
    _fields_ = [("gpuMs", C.c_double), ("presentMs", C.c_double),
                ("flags", C.c_uint32), ("reserved", C.c_uint32)]


class Timing(C.Structure):
    _fields_ = [(name, C.c_uint32) for name in (
        "bytes", "version", "status", "frames", "recorded", "completedGpu", "warmup", "preview")
    ] + [("frequency", C.c_uint64), ("generation", C.c_uint64), ("samples", TimingSample * 240)]


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
        p.projection[0] = 1
        p.flags = 5  # Current capture, original source 4x; published image is 1x.
        self.assertEqual(self.publish(C.byref(p), 1208, 1), 1)
        p.frame = 2
        p.flags = 9  # Current capture explicitly proves native surface fog off.
        self.assertEqual(self.publish(C.byref(p), 1208, 1), 1)
        p.frame = 3
        p.flags = 17  # Unknown source-format bit must remain rejected.
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

    def test_preview_requests_are_bounded_and_do_not_touch_a_runtime(self):
        capabilities = self.library.OpReShadeGetPreviewCapabilitiesV1
        capabilities.restype = C.c_uint32
        self.assertEqual(capabilities(), 1)
        select = self.library.OpReShadeSelectPreviewV1
        select.argtypes = [C.c_uint32]
        select.restype = C.c_uint32
        wants = self.library.OpReShadeWantsSceneDepthV1
        wants.restype = C.c_uint32
        selected = self.library.OpReShadeGetPreviewV1
        selected.restype = C.c_uint32
        self.assertEqual(select(5), 0)  # External is observation only.
        self.assertEqual(select(0xffffffff), 0)
        self.assertEqual(select(3), 1)
        self.assertEqual(wants(), 1)  # Availability starts only on explicit request.
        self.assertEqual(selected(), 5)  # Queueing alone never pretends application.
        self.assertEqual(select(0), 1)
        self.assertEqual(wants(), 0)

    def test_fog_compatibility_is_fresh_capture_metadata_not_sticky_generation_state(self):
        p = self.publication()
        p.flags = 13  # Current capture,4x source,native fog disabled.
        self.assertEqual(self.publish(C.byref(p), 1208, 1), 1)
        state = State()
        self.assertEqual(self.query(C.byref(state), 1232, 1), 1)
        self.assertEqual(state.publication.flags, 13)
        p.frame = 2
        p.flags = 5  # Same generation/sample mode; fog enabled again.
        self.assertEqual(self.publish(C.byref(p), 1208, 1), 1)
        self.assertEqual(self.query(C.byref(state), 1232, 1), 1)
        self.assertEqual(state.publication.flags, 5)

    def test_timing_abi_and_bounded_queue_without_gpu(self):
        query = self.library.OpReShadeGetTimingV1
        query.argtypes = [C.POINTER(Timing), C.c_uint32, C.c_uint32]
        query.restype = C.c_uint32
        request = self.library.OpReShadeStartTimingV1
        request.argtypes = [C.c_uint32]
        request.restype = C.c_uint32
        self.assertEqual(C.sizeof(TimingSample), 24)
        self.assertEqual(C.sizeof(Timing), 5808)
        self.assertEqual(Timing.samples.offset, 48)
        state = Timing()
        self.assertEqual(query(None, 5808, 1), 0)
        self.assertEqual(query(C.byref(state), 5800, 1), 0)
        self.assertEqual(query(C.byref(state), 5808, 2), 0)
        self.assertEqual(query(C.byref(state), 5808, 1), 1)
        self.assertEqual((state.bytes, state.version, state.status, state.recorded), (5808, 1, 0, 0))
        for frames in (0, 29, 241, 0xffffffff):
            self.assertEqual(request(frames), 0)
        self.assertEqual(request(120), 1)
        self.assertEqual(request(120), 0)  # One bounded queued window, never an unbounded loop.
        self.assertEqual(query(C.byref(state), 5808, 1), 1)
        self.assertEqual(state.status, 0)  # Request alone cannot fabricate GPU evidence.


if __name__ == "__main__":
    unittest.main()
