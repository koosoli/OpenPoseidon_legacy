#!/usr/bin/env python3
"""PACE-001 -- analyse a frame-pacing trace (POSEIDON_FRAME_TRACE) per mission phase.

Reads <prefix>.main.csv / .producer.csv / .worker.csv written by the engine and the run
log's `FT <S|M> <time> <frame>` lines (scripts/farfield-movecam.ps1) to cut the trace into
the static and moving phases by ENGINE FRAME NUMBER, then reports what a player feels
rather than what an average shows:

  * frame period (start-to-start on the main thread): median, p95, p99, max, and the
    JITTER = mean |period[n] - period[n-1]| / median -- a bimodal 8/34 ms stream has a
    low mean and a high jitter; a steady 21 ms stream has the same mean and no jitter;
  * where the time went: pacer sleep, InitDraw wait for the worker, slot wait, and on the
    worker: acquire (swapchain back-pressure), submit, present, GPU total;
  * ticks per frame: how often consecutive frames advance the world by a different number
    of 1/60 s ticks (a 1,1,2,1,1,2 sequence is motion judder with perfectly even frames).

usage: pace-analyse.py <prefix> [--log run.log] [--png out.png] [--label name] [--json out.json]
"""
import argparse
import csv
import json
import math
import re
import sys
from pathlib import Path


def read_csv(path):
    rows = []
    with open(path, newline="") as f:
        for line in f:
            if line.startswith("#"):
                continue
            rows.append(line.rstrip("\n"))
    if not rows:
        return []
    reader = csv.DictReader(rows)
    out = []
    for r in reader:
        out.append({k: (float(v) if k not in ("frame", "seq", "steps", "total_ticks", "cap_fps",
                                             "render_thread", "overlap") else int(v))
                    for k, v in r.items()})
    return out


def percentile(values, p):
    if not values:
        return float("nan")
    s = sorted(values)
    k = (len(s) - 1) * p / 100.0
    lo = math.floor(k)
    hi = math.ceil(k)
    if lo == hi:
        return s[int(k)]
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def phases_from_log(log_path):
    """{tag: (first_frame, last_frame)} from the FT lines. Also warns when an auto-screenshot
    (a blocking readback) or a slow-frame/slow-block log line fell inside a measured window."""
    if not log_path:
        return {}
    pat = re.compile(r"FT ([WSMX]) ([0-9.]+) ([0-9]+)(?: ([0-9.]+))?")
    shot = re.compile(r"Auto-screenshot saved: frame=([0-9]+)")
    frames = {}
    shots = []
    speeds = {}
    with open(log_path, errors="replace") as f:
        for line in f:
            m = pat.search(line)
            if m:
                tag, frame = m.group(1), int(m.group(3))
                lo, hi = frames.get(tag, (frame, frame))
                frames[tag] = (min(lo, frame), max(hi, frame))
                if m.group(4) is not None:
                    speeds.setdefault(tag, []).append(float(m.group(4)))
                continue
            m = shot.search(line)
            if m:
                shots.append(int(m.group(1)))
    for tag, (lo, hi) in frames.items():
        for sf in shots:
            if lo <= sf <= hi:
                print(f"WARNING: auto-screenshot at frame {sf} lies inside phase {tag} ({lo}..{hi}); "
                      f"its readback stall is in this window's p99/max")
    if shots:
        print(f"auto-screenshot frames: {shots}; phases: {frames}")
    # Ride missions log the vehicle speed on every moving-phase line. A "moving" phase in
    # which the vehicle did not move is not a measurement: fail loudly, never analyse it.
    global RIDE_FAILED
    for tag in ("M", "X"):
        if tag in speeds:
            med = percentile(speeds[tag], 50)
            print(f"ride phase {tag}: vehicle speed median {med:.1f} km/h (OFP `speed`) over {len(speeds[tag])} samples")
            if med < 1.0:
                print(f"FAIL: ride phase {tag} — the vehicle did not move (median speed {med:.2f} km/h); phase discarded")
                frames.pop(tag, None)
                RIDE_FAILED = True
    return frames


RIDE_FAILED = False


def stats(values):
    if not values:
        return {}
    mean = sum(values) / len(values)
    med = percentile(values, 50)
    return {
        "n": len(values),
        "mean": mean,
        "median": med,
        "p95": percentile(values, 95),
        "p99": percentile(values, 99),
        "max": max(values),
        "min": min(values),
    }


def analyse_phase(main, producer, worker, lo, hi, trim_ms=0.0):
    rows = [r for r in main if lo <= r["frame"] <= hi]
    rows.sort(key=lambda r: r["frame"])
    if trim_ms > 0 and len(rows) > 10:
        # e.g. the external-view phase begins with the camera swinging into place; that
        # transition is real but it is not the motion under test.
        t0 = rows[0]["t_start_ms"]
        rows = [r for r in rows if r["t_start_ms"] - t0 >= trim_ms]
    if len(rows) < 3:
        return None
    periods = [b["t_start_ms"] - a["t_start_ms"] for a, b in zip(rows, rows[1:])]
    med = percentile(periods, 50)
    jitter = sum(abs(b - a) for a, b in zip(periods, periods[1:])) / max(1, len(periods) - 1)
    within10 = sum(1 for p in periods if abs(p - med) <= 0.10 * med) / len(periods)
    long_frames = sum(1 for p in periods if p > 1.5 * med)
    short_frames = sum(1 for p in periods if p < 0.5 * med)
    # alternation: sign changes of successive period differences, normalised. A strictly
    # short/long/short stream gives ~1.0; a random one ~0.67; a steady one ~0 (noise).
    diffs = [b - a for a, b in zip(periods, periods[1:])]
    signs = [1 if d > 0.05 * med else (-1 if d < -0.05 * med else 0) for d in diffs]
    flips = sum(1 for a, b in zip(signs, signs[1:]) if a != 0 and b != 0 and a != b)
    alternation = flips / max(1, len(signs) - 1)

    # Motion as seen: camera displacement per frame. Even frames with uneven displacement is
    # judder; the relative jitter of dx is the number that says so.
    dx = [r.get("cam_dx_m", 0.0) for r in rows[1:]]
    dx_med = percentile(dx, 50) if dx else 0.0
    dx_jitter = (sum(abs(b - a) for a, b in zip(dx, dx[1:])) / max(1, len(dx) - 1)) if dx else 0.0
    # displacement per millisecond of frame period: constant for smooth motion
    speed = [d / p * 1000.0 for d, p in zip(dx, periods) if p > 0]
    dyaw = [r.get("cam_dyaw_deg", 0.0) for r in rows[1:]]
    dyaw_med = percentile(dyaw, 50) if dyaw else 0.0
    dyaw_jitter = (sum(abs(b - a) for a, b in zip(dyaw, dyaw[1:])) / max(1, len(dyaw) - 1)) if dyaw else 0.0
    yaw_rate = [d / p * 1000.0 for d, p in zip(dyaw, periods) if p > 0]
    steps = [r["steps"] for r in rows]
    step_changes = sum(1 for a, b in zip(steps, steps[1:]) if a != b) / max(1, len(steps) - 1)
    step_hist = {}
    for s in steps:
        step_hist[s] = step_hist.get(s, 0) + 1

    out = {
        "frames": len(rows),
        "first_frame": rows[0]["frame"],
        "last_frame": rows[-1]["frame"],
        "duration_s": (rows[-1]["t_start_ms"] - rows[0]["t_start_ms"]) / 1000.0,
        "fps_avg": 1000.0 * len(periods) / max(1e-9, (rows[-1]["t_start_ms"] - rows[0]["t_start_ms"])),
        "period": stats(periods),
        "jitter_ms": jitter,
        "jitter_rel": jitter / med if med > 0 else float("nan"),
        "within_10pct": within10,
        "long_frames_gt_1_5x": long_frames,
        "short_frames_lt_0_5x": short_frames,
        "alternation": alternation,
        "cap_fps": rows[len(rows) // 2]["cap_fps"],
        "pace_sleep_req": stats([r["pace_sleep_req_ms"] for r in rows]),
        "pace_sleep_act": stats([r["pace_sleep_act_ms"] for r in rows]),
        "pace_sleep_overshoot": stats([r["pace_sleep_act_ms"] - r["pace_sleep_req_ms"] for r in rows if r["pace_sleep_req_ms"] > 0]),
        "frames_slept": sum(1 for r in rows if r["pace_sleep_req_ms"] > 0) / len(rows),
        "sim_ms": stats([r["sim_ms"] for r in rows]),
        "draw_init_ms": stats([r["draw_init_ms"] for r in rows]),
        "draw_ms": stats([r["draw_ms"] for r in rows]),
        "swap_ms": stats([r["swap_ms"] for r in rows]),
        "deltaT_ms": stats([r["deltaT_ms"] for r in rows]),
        "steps_hist": step_hist,
        "steps_change_rate": step_changes,
        "ticks_per_frame": sum(steps) / len(steps),
        "cam_dx": stats(dx),
        "cam_dx_jitter_rel": (dx_jitter / dx_med) if dx_med > 1e-6 else float("nan"),
        "cam_speed": stats(speed),
        "interp_alpha": stats([r.get("interp_alpha", 0.0) for r in rows]),
        "cam_dyaw": stats(dyaw),
        "cam_dyaw_jitter_rel": (dyaw_jitter / dyaw_med) if dyaw_med > 1e-6 else float("nan"),
        "cam_yaw_rate": stats(yaw_rate),
    }
    prod = [r for r in producer if lo <= r["frame"] <= hi]
    if prod:
        out["producer"] = {
            "predraw_wait": stats([r["predraw_wait_ms"] for r in prod]),
            "lazy_wait": stats([r["lazy_wait_ms"] for r in prod]),
            "slot_wait": stats([r["slot_wait_ms"] for r in prod]),
            "lockstep_wait": stats([r["lockstep_wait_ms"] for r in prod]),
            "render_thread": prod[len(prod) // 2]["render_thread"],
            "overlap": prod[len(prod) // 2]["overlap"],
        }
    t0, t1 = rows[0]["t_start_ms"], rows[-1]["t_end_ms"]
    work = [r for r in worker if t0 <= r["t_start_ms"] <= t1]
    if work:
        gpu = [r["gpu_frame_ms"] for r in work if r["gpu_frame_ms"] >= 0]
        out["worker"] = {
            "blocks": len(work),
            "idle_before": stats([r["idle_before_ms"] for r in work]),
            "acquire": stats([r["acquire_ms"] for r in work if r["acquire_ms"] >= 0]),
            "submit": stats([r["submit_ms"] for r in work if r["submit_ms"] >= 0]),
            "present": stats([r["present_ms"] for r in work if r["present_ms"] >= 0]),
            "render": stats([r["render_ms"] for r in work]),
            "busy": stats([r["t_end_ms"] - r["t_start_ms"] for r in work]),
            "gpu_frame": stats(gpu),
            "acquire_blocked_gt_2ms": sum(1 for r in work if r["acquire_ms"] > 2.0) / len(work),
        }
    out["_periods"] = periods
    out["_t"] = [r["t_start_ms"] for r in rows[1:]]
    out["_steps"] = steps
    return out


def fmt_stats(s, unit="ms"):
    if not s:
        return "-"
    return f"med {s['median']:.2f} mean {s['mean']:.2f} p95 {s['p95']:.2f} p99 {s['p99']:.2f} max {s['max']:.1f} {unit}"


def report(label, phase, a):
    print(f"--- {label} / {phase}: {a['frames']} frames, {a['duration_s']:.1f} s, cap {a['cap_fps']}")
    print(f"  fps avg {a['fps_avg']:.1f} | period {fmt_stats(a['period'])}")
    print(f"  jitter {a['jitter_ms']:.2f} ms ({100*a['jitter_rel']:.0f}% of median) | within +-10% {100*a['within_10pct']:.0f}% | "
          f"long(>1.5x) {a['long_frames_gt_1_5x']} short(<0.5x) {a['short_frames_lt_0_5x']} | alternation {a['alternation']:.2f}")
    print(f"  pacer: slept on {100*a['frames_slept']:.0f}% of frames, req {fmt_stats(a['pace_sleep_req'])}; overshoot {fmt_stats(a['pace_sleep_overshoot'])}")
    print(f"  main: sim {fmt_stats(a['sim_ms'])}")
    print(f"        drw:init (wait for worker) {fmt_stats(a['draw_init_ms'])}")
    print(f"        draw {fmt_stats(a['draw_ms'])}")
    print(f"        swap/publish {fmt_stats(a['swap_ms'])}")
    print(f"  sim input deltaT {fmt_stats(a['deltaT_ms'])} | ticks/frame {a['ticks_per_frame']:.2f} hist {dict(sorted(a['steps_hist'].items()))} | "
          f"step-count changes between consecutive frames {100*a['steps_change_rate']:.0f}%")
    print(f"  camera motion: dx/frame {fmt_stats(a['cam_dx'], 'm')} | dx jitter {100*a['cam_dx_jitter_rel']:.0f}% of median | speed {fmt_stats(a['cam_speed'], 'm/s')} | interp alpha {fmt_stats(a['interp_alpha'], '')}")
    print(f"  camera turn:   dyaw/frame {fmt_stats(a['cam_dyaw'], 'deg')} | dyaw jitter {100*a['cam_dyaw_jitter_rel']:.0f}% of median | yaw rate {fmt_stats(a['cam_yaw_rate'], 'deg/s')}")
    if "producer" in a:
        p = a["producer"]
        print(f"  producer (thread={p['render_thread']} overlap={p['overlap']}): predraw {fmt_stats(p['predraw_wait'])}")
        print(f"        lazy {fmt_stats(p['lazy_wait'])} | slot {fmt_stats(p['slot_wait'])} | lockstep {fmt_stats(p['lockstep_wait'])}")
    if "worker" in a:
        w = a["worker"]
        print(f"  worker: {w['blocks']} blocks, busy {fmt_stats(w['busy'])}")
        print(f"        acquire {fmt_stats(w['acquire'])} (blocked >2 ms on {100*w['acquire_blocked_gt_2ms']:.0f}%)")
        print(f"        submit {fmt_stats(w['submit'])} | present {fmt_stats(w['present'])} | idle-before {fmt_stats(w['idle_before'])}")
        print(f"        GPU frame total {fmt_stats(w['gpu_frame'])}")


def plot(label, results, png):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as e:  # pragma: no cover
        print(f"(no plot: {e})")
        return
    phases = [p for p in ("S", "M", "X") if p in results]
    fig, axes = plt.subplots(len(phases), 2, figsize=(14, 3.6 * len(phases)), squeeze=False)
    for i, ph in enumerate(phases):
        a = results[ph]
        t = [(x - a["_t"][0]) / 1000.0 for x in a["_t"]]
        ax = axes[i][0]
        ax.plot(t, a["_periods"], lw=0.6)
        ax.set_ylim(0, max(50, min(200, a["period"]["p99"] * 1.5)))
        ax.set_xlabel("s")
        ax.set_ylabel("frame period ms")
        phname = {'S': 'static', 'M': 'moving', 'X': 'moving-external'}[ph]
        ax.set_title(f"{label} {phname}: med {a['period']['median']:.1f} p99 {a['period']['p99']:.1f} jitter {a['jitter_ms']:.1f} ms")
        ax = axes[i][1]
        ax.hist(a["_periods"], bins=80, range=(0, max(50, min(200, a["period"]["p99"] * 1.5))))
        ax.set_xlabel("frame period ms")
        ax.set_ylabel("frames")
    fig.tight_layout()
    fig.savefig(png, dpi=110)
    print(f"plot: {png}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix")
    ap.add_argument("--log")
    ap.add_argument("--png")
    ap.add_argument("--json")
    ap.add_argument("--label", default="")
    ap.add_argument("--window", help="frame range lo:hi instead of the log's phases")
    args = ap.parse_args()
    prefix = args.prefix
    main_rows = read_csv(prefix + ".main.csv")
    producer = read_csv(prefix + ".producer.csv") if Path(prefix + ".producer.csv").exists() else []
    worker = read_csv(prefix + ".worker.csv") if Path(prefix + ".worker.csv").exists() else []
    if not main_rows:
        print("no main rows", file=sys.stderr)
        return 2
    label = args.label or Path(prefix).name
    windows = {}
    if args.window:
        lo, hi = args.window.split(":")
        windows["W"] = (int(lo), int(hi))
    else:
        windows = phases_from_log(args.log) if args.log else {}
        windows.pop("W", None)  # settle is discarded
    if not windows:
        windows["ALL"] = (main_rows[0]["frame"], main_rows[-1]["frame"])
    results = {}
    for tag, (lo, hi) in sorted(windows.items()):
        a = analyse_phase(main_rows, producer, worker, lo, hi, trim_ms=3000.0 if tag == "X" else 0.0)
        if a is None:
            print(f"--- {label} / {tag}: too few frames in {lo}..{hi}")
            continue
        results[tag] = a
        report(label, {"S": "static", "M": "moving", "X": "moving-external"}.get(tag, tag), a)
    if args.png and results:
        plot(label, results, args.png)
    if args.json:
        slim = {k: {kk: vv for kk, vv in v.items() if not kk.startswith("_")} for k, v in results.items()}
        with open(args.json, "w") as f:
            json.dump(slim, f, indent=1, default=str)
    return 3 if RIDE_FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
