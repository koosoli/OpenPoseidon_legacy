#!/usr/bin/env python3
"""Summarize a successful installed four-window receipt; never measures a GPU."""
import argparse
import json
import math
from pathlib import Path
import statistics


def statistics_ms(values):
    if not values:
        return None
    ordered = sorted(values)
    return {"count": len(values), "meanMs": statistics.mean(values),
            "medianMs": statistics.median(values),
            "p95Ms": ordered[math.ceil(0.95 * len(ordered)) - 1]}


def summarize(receipt):
    def require(condition, message):
        if not condition:
            raise ValueError(message)

    def finite(value, positive=False):
        return (isinstance(value, (int, float)) and not isinstance(value, bool)
                and math.isfinite(value) and (value > 0 if positive else value >= 0))

    require(receipt.get("captureSucceeded") is True and receipt.get("cleanTeardown") is True
            and receipt.get("measureGpu") is True and not receipt.get("error"),
            "A successful measured capture/clean-teardown receipt is required")
    windows = receipt.get("timingWindows", [])
    require(len(windows) == 4, "Exactly four OFF/ON/ON/OFF windows required")
    result = {"installed": receipt.get("installed"), "mode": receipt.get("mode"),
              "msaaSamples": receipt.get("msaaSamples"), "windows": [],
              "scope": "GPU: completed ReShade effect brackets only, excludes engine snapshot/overlay/layer work outside callbacks. Present: wall frame cadence, not GPU busy time. No engine timer fallback or quality/cost acceptance.",
              "percentile": "nearest-rank p95; median uses midpoint for even counts"}
    previous_generation = 0
    preview = None
    pooled_wall = {False: [], True: []}
    pooled_on_gpu = []
    for index, (window, enabled) in enumerate(zip(windows, (False, True, True, False))):
        require(window.get("enabled") is enabled, "Window order must be OFF/ON/ON/OFF")
        timing = window["timing"]
        require(timing.get("status") == 5 and timing.get("frames") == 120
                and timing.get("recorded") == 120, "Completed 120-frame windows required")
        generation = timing.get("generation", 0)
        require(finite(generation, True) and generation > previous_generation,
                "Timing generations must increase")
        previous_generation = generation
        require(finite(timing.get("frequency"), True), "Supported timestamp frequency required")
        require(timing.get("preview") in (2, 3, 4), "An explicitly selected depth preview is required")
        if preview is None:
            preview = timing["preview"]
        require(timing["preview"] == preview, "Preview changed between windows")
        samples = timing.get("samples", [])
        require(len(samples) == 120, "Missing raw samples")
        gpu, wall = [], []
        for sample in samples:
            flags = sample.get("flags")
            require(isinstance(flags, int) and not isinstance(flags, bool) and flags & ~7 == 0
                    and flags & 1 and bool(flags & 4) == enabled, "Invalid sample scope/state flags")
            require(finite(sample.get("presentMs"), True), "Invalid wall interval")
            wall.append(sample["presentMs"])
            if flags & 2:
                require(finite(sample.get("gpuMs")) and sample["gpuMs"] < 1000,
                        "Invalid completed GPU bracket")
                gpu.append(sample["gpuMs"])
        require(timing.get("completedGpu") == len(gpu), "GPU completion count disagrees with raw samples")
        require(not enabled or len(gpu) >= 108, "Too few genuine ON GPU brackets")
        pooled_wall[enabled].extend(wall)
        if enabled:
            pooled_on_gpu.extend(gpu)
        result["windows"].append({"index": index + 1, "enabled": enabled, "generation": generation,
                                  "gpu": statistics_ms(gpu), "present": statistics_ms(wall)})
    result["onGpu"] = statistics_ms(pooled_on_gpu)
    result["offPresent"] = statistics_ms(pooled_wall[False])
    result["onPresent"] = statistics_ms(pooled_wall[True])
    result["onMinusOffMedianPresentMs"] = result["onPresent"]["medianMs"] - result["offPresent"]["medianMs"]
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("receipt", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        report = summarize(json.loads(args.receipt.read_text(encoding="utf-8-sig")))
    except (ValueError, KeyError, TypeError) as error:
        parser.exit(2, f"Invalid measurement receipt: {error}\n")
    text = json.dumps(report, indent=2, allow_nan=False) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
