"""Validate installed-engine overlay A/B receipts without launching a renderer.

These logs prove registration/binding admission, not that a particular material
contributed a visible fragment or that its appearance matches another renderer.
"""
import argparse
import json
import re
from pathlib import Path

MATERIAL = "Pub_Village_E_01_L_Ext.emat"
MODEL = "pubvillage_e_2l01.xob"
OVERLAY = "WGR_REFORGER_GLOBAL_OVERLAY_PREVIEW"
REQUIRED = ("WGR_REFORGER_PBR_PREVIEW", "WGR_REFORGER_LAYER_ORDER_PREVIEW",
            "WGR_REFORGER_SRGB_PREVIEW", "WGR_REFORGER_NORMAL_PREVIEW",
            "WGR_REFORGER_TANGENT_PREVIEW")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_bounded(path, maximum):
    require(path.is_file() and 0 < path.stat().st_size <= maximum,
            f"Missing, empty or oversized receipt: {path}")
    return path.read_text(encoding="utf-8-sig", errors="strict")


def load_run(stem):
    return {
        "meta": json.loads(read_bounded(Path(str(stem) + ".meta.json"), 16 << 20)),
        "metrics": json.loads(read_bounded(Path(str(stem) + ".json"), 16 << 20)),
        "log": read_bounded(Path(str(stem) + ".log"), 128 << 20),
    }


def option(arguments, name):
    match = re.search(r"(?:^|\s)--" + re.escape(name) + r"(?:=|\s+)(.*?)(?=\s+--|$)", arguments)
    require(match is not None and match[1].strip(), f"Missing comparison argument --{name}")
    return match[1].strip()


def binding_counts(log, operation, model):
    expression = (re.escape(operation) + r": model=([^\r\n]+?) admittedSections=(\d+)/(\d+)")
    result = []
    for name, admitted, requested in re.findall(expression, log, re.I):
        if name.lower().endswith(model.lower()):
            admitted, requested = int(admitted), int(requested)
            require(0 < admitted == requested, f"{operation}: incomplete target bindings {admitted}/{requested}")
            result.append([admitted, requested])
    require(result, f"No positive target {operation} bindings")
    return result


def validate_pair(off, on, material=MATERIAL, model=MODEL, expected_registered=20683):
    result = {"status": "pass", "scope": "complete-scene-and-registration-bindings-only", "runs": {}}
    for label, run, enabled in (("off", off, "0"), ("on", on, "1")):
        meta, metrics, log = run["meta"], run["metrics"], run["log"]
        require(meta.get("status") == "ok" and meta.get("exit_code") == 0 and
                meta.get("timed_out") is False and meta.get("contended") is False and
                meta.get("performance_comparable") is True and not meta.get("problems"),
                f"{label}: incomplete/contended lifecycle")
        require("Shutdown complete" in log, f"{label}: no completed shutdown")
        require(not re.search(r"Validation Error|uncaptured.*error|panicked at|invalid.*wgr_gpu_driven_pipeline", log, re.I),
                f"{label}: renderer validation/panic failure")
        env = meta.get("env", {})
        require(all(str(env.get(key)) == "1" for key in REQUIRED), f"{label}: missing fixed material preview gates")
        require(str(env.get(OVERLAY, "0")) == enabled, f"{label}: incorrect overlay gate")
        for key in ("WGR_REFORGER_LAYER_ORDER_MATERIAL", "WGR_REFORGER_NORMAL_MATERIAL",
                    "WGR_REFORGER_SRGB_MATERIAL"):
            require(str(env.get(key, "")).lower() == material.lower(), f"{label}: wrong target in {key}")
        require(str(env.get("WGR_REFORGER_GLOBAL_OVERLAY_MATERIAL", MATERIAL) or MATERIAL).lower() == material.lower(),
                f"{label}: wrong overlay material target")
        build, objects = metrics["build"], metrics["objects"]
        require((build["render_width"], build["render_height"], build["msaa_samples"], build["dlss_active"]) ==
                (1920, 1080, 4, False), f"{label}: unsupported render profile")
        require(objects.get("valid") is True and objects["registered_instances"] == expected_registered and
                objects["main_instances"] > 0 and objects["main_tris"] > 0, f"{label}: incomplete object scene")
        preparer = metrics["streaming"]["preparer"]
        require(preparer["queued"] == preparer["ready"] == 0, f"{label}: unsettled object preparation")
        require("Wgpu lazy BCR tint:" in log, f"{label}: no actual lazy BCR")
        require(not re.search(r"Landscape: skipped .*terrain rect", log), f"{label}: rejected terrain pass")
        require(sum(t["name"] == "Terrain: main colour" and t["milliseconds"] >= 0 for t in metrics["gpu_timings_ms"]) == 1,
                f"{label}: no valid terrain timing")
        target = re.escape(material)
        require(re.search(r"authored UV0 tangent preview: material=[^\r\n]*" + target +
                          r" admission=true producer=3(?:\s|$)", log, re.I), f"{label}: no target authored tangent V3 admission")
        srgb = binding_counts(log, "native PBR sRGB views", model)
        overlay = []
        if enabled == "1":
            require(re.search(r"native global overlay preview: material=[^\r\n]*" + target +
                              r" sourceSupported=true uv1=true sourceAdmission=true(?:\s|$)", log, re.I),
                    "on: no supported target overlay source admission")
            overlay = binding_counts(log, "native global overlay bindings", model)
        else:
            require("native global overlay preview:" not in log and "native global overlay bindings:" not in log,
                    "off: overlay operation was admitted")
        result["runs"][label] = {
            "registered": objects["registered_instances"], "main": objects["main_instances"],
            "triangles": objects["main_tris"], "srgb_bindings": srgb, "overlay_bindings": overlay,
            "gpu_frame_ms": meta.get("gpu_frame_ms"),
        }
    require(off["meta"]["deployed_from"] == on["meta"]["deployed_from"], "Different installed builds")
    require(off["metrics"]["build"] == on["metrics"]["build"], "Different renderer profiles")
    for key in ("registered_instances", "main_instances", "main_tris"):
        require(off["metrics"]["objects"][key] == on["metrics"]["objects"][key], f"Scene counts differ: {key}")
    for name in ("test-mission", "test-world", "test-world-freefly", "test-world-hour"):
        require(option(off["meta"]["arguments"], name) == option(on["meta"]["arguments"], name),
                f"Comparison argument differs: --{name}")
    ignored = {"POSEIDON_USER_DIR", OVERLAY}
    a = {k: str(v) for k, v in off["meta"]["env"].items() if k not in ignored}
    b = {k: str(v) for k, v in on["meta"]["env"].items() if k not in ignored}
    require(a == b, "Environment differs beyond overlay gate and isolated user directory")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--off", type=Path, required=True, help="run stem without .log/.json/.meta.json")
    parser.add_argument("--on", type=Path, required=True)
    parser.add_argument("--material", default=MATERIAL)
    parser.add_argument("--model", default=MODEL)
    parser.add_argument("--expected-registered", type=int, default=20683)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    try:
        result = validate_pair(load_run(args.off), load_run(args.on), args.material, args.model, args.expected_registered)
    except (ValueError, KeyError, TypeError, OSError) as error:
        parser.exit(1, f"FAIL: {error}\n")
    output = json.dumps(result, indent=2) + "\n"
    if args.report:
        args.report.write_text(output, encoding="utf-8")
    print(output, end="")


if __name__ == "__main__":
    main()
