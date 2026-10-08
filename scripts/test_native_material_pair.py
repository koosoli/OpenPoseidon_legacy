import unittest

from assert_native_material_pair import MATERIAL, OVERLAY, REQUIRED, validate_pair


def fixture(enabled):
    env = {key: "1" for key in REQUIRED}
    env.update({OVERLAY: str(enabled), "POSEIDON_USER_DIR": "own-on" if enabled else "own-off"})
    for key in ("LAYER_ORDER", "NORMAL", "SRGB"):
        env["WGR_REFORGER_" + key + "_MATERIAL"] = MATERIAL
    log = ("Shutdown complete\nWgpu lazy BCR tint: owned fixture\n"
           "Wgpu authored UV0 tangent preview: material=Assets/" + MATERIAL + " admission=true producer=3\n"
           "[wgr] native PBR sRGB views: model=assets/pubvillage_e_2l01.xob admittedSections=6/6\n")
    if enabled:
        log += ("Wgpu native global overlay preview: material=Assets/" + MATERIAL +
                " sourceSupported=true uv1=true sourceAdmission=true\n"
                "[wgr] native global overlay bindings: model=assets/pubvillage_e_2l01.xob admittedSections=6/6\n")
    return {
        "meta": {"status": "ok", "exit_code": 0, "timed_out": False, "contended": False,
                 "performance_comparable": True, "problems": [], "env": env, "deployed_from": "owned-build",
                 "arguments": '--test-mission "owned.eden" --test-world "owned world" --test-world-freefly 1 2 3 4 5 --test-world-hour 16'},
        "metrics": {"build": {"render_width": 1920, "render_height": 1080, "msaa_samples": 4, "dlss_active": False},
                    "objects": {"valid": True, "registered_instances": 20683, "main_instances": 6295, "main_tris": 11826164},
                    "streaming": {"preparer": {"queued": 0, "ready": 0}},
                    "gpu_timings_ms": [{"name": "Terrain: main colour", "milliseconds": 1.0}]},
        "log": log,
    }


class MaterialPairTests(unittest.TestCase):
    def assert_declines(self, change, pattern):
        off, on = fixture(0), fixture(1)
        change(off, on)
        with self.assertRaisesRegex(ValueError, pattern):
            validate_pair(off, on)

    def test_positive_registration_is_distinct_from_visible_acceptance(self):
        report = validate_pair(fixture(0), fixture(1))
        self.assertEqual(report["scope"], "complete-scene-and-registration-bindings-only")
        self.assertEqual(report["runs"]["on"]["overlay_bindings"], [[6, 6]])

    def test_cpu_source_admission_without_renderer_binding_declines(self):
        self.assert_declines(lambda off, on: on.update(log=on["log"].replace(
            "native global overlay bindings", "unrelated operation")), "No positive target")

    def test_zero_or_partial_bindings_decline(self):
        for counts in ("0/6", "5/6", "0/0", "7/6"):
            self.assert_declines(lambda off, on: on.update(log=on["log"].replace(
                "overlay bindings: model=assets/pubvillage_e_2l01.xob admittedSections=6/6",
                "overlay bindings: model=assets/pubvillage_e_2l01.xob admittedSections=" + counts)), "incomplete target")

    def test_old_or_declined_tangent_receipt_declines(self):
        for replacement in ("admission=false producer=3", "admission=true producer=2"):
            self.assert_declines(lambda off, on: on.update(log=on["log"].replace(
                "admission=true producer=3", replacement)), "authored tangent V3")

    def test_empty_scene_and_unsettled_preparer_decline(self):
        self.assert_declines(lambda off, on: on["metrics"]["objects"].update(main_tris=0), "incomplete object scene")
        self.assert_declines(lambda off, on: on["metrics"]["streaming"]["preparer"].update(ready=1), "unsettled")

    def test_different_camera_environment_build_or_scene_declines(self):
        self.assert_declines(lambda off, on: on["meta"].update(arguments=on["meta"]["arguments"].replace("1 2 3 4 5", "1 2 3 4 6")), "argument differs")
        self.assert_declines(lambda off, on: on["meta"]["env"].update(WGR_OTHER_SWITCH="1"), "Environment differs")
        self.assert_declines(lambda off, on: on["meta"].update(deployed_from="different-build"), "Different installed builds")
        self.assert_declines(lambda off, on: on["metrics"]["objects"].update(main_instances=1), "Scene counts differ")

    def test_validation_error_timeout_and_off_overlay_decline(self):
        self.assert_declines(lambda off, on: on.update(log=on["log"] + "Validation Error"), "validation/panic")
        self.assert_declines(lambda off, on: on["meta"].update(timed_out=True), "lifecycle")
        self.assert_declines(lambda off, on: off.update(log=on["log"]), "off: overlay")

    def test_wrong_material_or_texture_role_gate_declines(self):
        self.assert_declines(lambda off, on: on["meta"]["env"].update(WGR_REFORGER_SRGB_PREVIEW="0"), "fixed material preview")
        self.assert_declines(lambda off, on: on["meta"]["env"].update(WGR_REFORGER_NORMAL_MATERIAL="other.emat"), "wrong target")


if __name__ == "__main__":
    unittest.main()
