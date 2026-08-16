#!/usr/bin/env python3
"""Per-frame frame-time distributions, and residency-spike correlation, from a
farfield-movecam run log.

WHY THIS EXISTS
---------------
The capture JSON that farfield-bench.ps1 validates reports `cpu_frame_phases_ms` as
avg / p95 / max over a 256-frame ring, and a single `GPU frame total` sample.  A mean
hides a stutter, a p95 over a 256-frame window that spans a phase boundary mixes two
populations, and neither gives a median or a p99.  The one thing that answers "what does
a player actually experience" is the full per-frame series.

scripts/farfield-movecam.ps1 emits exactly that: its generated init.sqs writes one
`FT <phase> <missiontime> <framecounter>` line per rendered frame, and the engine log
stamps every line with a millisecond wall clock.  The DELTA between consecutive stamps
within a phase is the frame time.

    W = settle (streaming fill-in; discarded by default)
    S = static, measured
    M = moving, measured

THE FRAME COUNTER IS WHY THIS CAN BE BELIEVED.  A script loop that ticks once per frame
is an assumption, and the failure is silent in the flattering direction: if the loop ran
twice per frame the deltas would be half a frame each and the report would invent a
frame rate twice the real one.  `triFrameCount` is the RENDERER's own counter
(GEngine->GetFrameCounter()), so every delta carries the number of frames it actually
spans.  Deltas are divided by that span, and the share of samples with span == 1 is
reported as `1:1` coverage.  Anything below 100% means the trace is coarser than
per-frame there, and a p99 computed over averaged multi-frame spans UNDERSTATES the
worst frame — so the coverage figure has to be read before the percentiles are quoted.

RESIDENCY CORRELATION (--residency)
-----------------------------------
Landscape::UpdateModernObjectResidency quantises its window centre to 4 cells and does
nothing until the camera crosses a boundary, at which point it rebuilds, gathers and
sorts the whole window in one go (LandSave.cpp).  That predicts moving frame time is
BIMODAL — cheap frames punctuated by recentre spikes — rather than uniformly elevated.

It logs "Modern object residency: centre=(X, Z) ..." on every centre change, so the
prediction is directly testable: bucket each frame by whether a recentre landed inside
it, and compare the two distributions.  If the worst frames sit on recentre events the
mechanism is named; if they fall between them, residency recentring is NOT the cause and
the streaming-transient theory needs rethinking.  Both outcomes are printed plainly —
a null result here is the more valuable one, so it must not be possible to miss it.
"""

from __future__ import annotations

import argparse
import math
import re
import sys
from datetime import datetime
from pathlib import Path

# [2026-08-15 23:56:59.655] [app-c604] [INFO] [SCRIPT]  "FT M 1.389 8213"
# The level/channel fields carry ANSI colour, so anchor on the stamp and the payload only.
# The frame counter is optional so a trace from an older init.sqs still parses; without it
# every span is assumed to be one frame and the coverage figure says so.
STAMP = re.compile(r"^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})\]")
FT = re.compile(r'"FT ([A-Z]) ([-0-9.eE]+)(?: ([-0-9.eE]+))?"')
MARK = re.compile(r'"(FFCAM [^"]*)"')
RESIDENCY = re.compile(
    r"Modern object residency: centre=\((-?\d+), (-?\d+)\) window=(\d+) cells "
    r"requested=(\d+) resident=(\d+) created=(\d+) released=(\d+)(?: refused=(\d+))?")

PHASE_NAMES = {"W": "settle (discarded)", "S": "static", "M": "moving"}


def parse_stamp(text: str) -> float:
    return datetime.strptime(text, "%Y-%m-%d %H:%M:%S.%f").timestamp()


def percentile(values: list[float], q: float) -> float:
    """Nearest-rank percentile on an already-sorted list.

    Nearest-rank, not linear interpolation: at a p99 over a few hundred frames the
    interpolated value is a blend of two real frames and belongs to neither.  The
    question here is "how bad is the 1%-worst frame that actually happened", so the
    answer has to be a frame that happened.
    """
    if not values:
        return float("nan")
    rank = max(1, math.ceil(q / 100.0 * len(values)))
    return values[min(rank, len(values)) - 1]


def stats(deltas: list[float]) -> dict:
    ordered = sorted(deltas)
    total = sum(ordered)
    return {
        "frames": len(ordered),
        "wall_seconds": total,
        "mean_ms": (total / len(ordered) * 1000.0) if ordered else float("nan"),
        "median_ms": percentile(ordered, 50) * 1000.0,
        "p95_ms": percentile(ordered, 95) * 1000.0,
        "p99_ms": percentile(ordered, 99) * 1000.0,
        "worst_ms": (ordered[-1] * 1000.0) if ordered else float("nan"),
        "best_ms": (ordered[0] * 1000.0) if ordered else float("nan"),
        # Throughput fps, not 1000/mean-of-frame-times: over a fixed wall window they are
        # the same number, and this one cannot be inflated by a handful of cheap frames.
        "fps_throughput": (len(ordered) / total) if total > 0 else float("nan"),
        "fps_median": (1.0 / percentile(ordered, 50)) if ordered else float("nan"),
        # Share of wall time spent inside frames worse than 100 ms (= under 10 fps).  A
        # median can look healthy while most of the CLOCK is spent in a few awful frames.
        "share_over_100ms": (sum(d for d in ordered if d > 0.100) / total) if total > 0 else float("nan"),
        "frames_over_100ms": sum(1 for d in ordered if d > 0.100),
        "frames_over_250ms": sum(1 for d in ordered if d > 0.250),
    }


class Run:
    def __init__(self) -> None:
        self.samples: list[tuple[str, float, float, float]] = []
        self.residency: list[dict] = []
        self.marks: list[str] = []
        # phase -> list of (t_start, t_end, per_frame_seconds, frame_span)
        self.intervals: dict[str, list[tuple[float, float, float, int]]] = {}
        self.coverage: dict[str, tuple[int, int]] = {}
        self.mission_span: dict[str, float] = {}
        self.notes: list[str] = []


def read_log(path: Path) -> Run:
    run = Run()
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            stamp = STAMP.match(line)
            if not stamp:
                continue
            when = parse_stamp(stamp.group(1))

            res = RESIDENCY.search(line)
            if res:
                run.residency.append({
                    "when": when, "text": stamp.group(1),
                    "cx": int(res.group(1)), "cz": int(res.group(2)),
                    "requested": int(res.group(4)), "resident": int(res.group(5)),
                    "created": int(res.group(6)), "released": int(res.group(7)),
                })
                continue

            mark = MARK.search(line)
            if mark:
                run.marks.append(f"{stamp.group(1)}  {mark.group(1)}")
                continue

            hit = FT.search(line)
            if hit:
                counter = float(hit.group(3)) if hit.group(3) is not None else float("nan")
                run.samples.append((hit.group(1), when, float(hit.group(2)), counter))
    return run


def build_intervals(run: Run, drop_first: int, drop_settle: bool) -> None:
    # Group into runs of consecutive same-phase samples. A frame time is only meaningful
    # between two frames of the SAME phase: the delta that straddles a boundary contains
    # the phase-change work (triPerfStats formats a 700-byte string) and belongs to
    # neither population.
    groups: list[tuple[str, list[tuple[float, float, float]]]] = []
    for phase, wall, mission, counter in run.samples:
        if groups and groups[-1][0] == phase:
            groups[-1][1].append((wall, mission, counter))
        else:
            groups.append((phase, [(wall, mission, counter)]))

    for phase, points in groups:
        if drop_settle and phase == "W":
            continue
        usable = points[drop_first:] if len(points) > drop_first else []
        if len(usable) < 2:
            continue

        exact, counted = run.coverage.get(phase, (0, 0))
        bucket = run.intervals.setdefault(phase, [])
        wall_total = 0.0
        mission_total = 0.0
        for i in range(1, len(usable)):
            wall_delta = usable[i][0] - usable[i - 1][0]
            frame_span = usable[i][2] - usable[i - 1][2]
            if not math.isfinite(frame_span) or frame_span < 1:
                # No counter, or the counter did not advance: treat the sample as one
                # frame and count it as non-exact so the coverage figure stays honest.
                frame_span = 1.0
            else:
                counted += 1
                if frame_span == 1.0:
                    exact += 1
            span = int(frame_span)
            bucket.append((usable[i - 1][0], usable[i][0], wall_delta / span, span))
            wall_total += wall_delta
            mission_total += usable[i][1] - usable[i - 1][1]

        run.coverage[phase] = (exact, counted)
        run.mission_span[phase] = run.mission_span.get(phase, 0.0) + (usable[-1][1] - usable[0][1])

        if wall_total > 0 and abs(mission_total - wall_total) / wall_total > 0.05:
            run.notes.append(
                f"phase {phase}: mission time advanced {mission_total:.1f} s over "
                f"{wall_total:.1f} s of wall clock ({mission_total / wall_total:.2f}x). "
                f"Simulation time is not tracking the clock, so anything derived from "
                f"_time is suspect; the wall-clock figures below still stand.")


def expand(intervals: list[tuple[float, float, float, int]]) -> list[float]:
    """One entry per rendered FRAME, not per trace sample.

    A sample that spanned three frames is three frames' worth of wall clock and must not
    enter the distribution weighted as one.
    """
    out: list[float] = []
    for _, _, per_frame, span in intervals:
        out.extend([per_frame] * span)
    return out


def recentres(run: Run) -> list[dict]:
    """Residency log rows where the quantised window centre actually MOVED.

    The same line is also emitted for fill-in progress at a fixed centre, and counting
    those as recentre events would smear the correlation until it said nothing.
    """
    out: list[dict] = []
    last: tuple[int, int] | None = None
    for row in run.residency:
        here = (row["cx"], row["cz"])
        if last is not None and here != last:
            out.append(row)
        last = here
    return out


def correlate(run: Run, phase: str) -> dict | None:
    intervals = run.intervals.get(phase)
    if not intervals:
        return None
    events = [r for r in recentres(run) if intervals[0][0] <= r["when"] <= intervals[-1][1]]
    if not events:
        return None

    on: list[tuple[float, int]] = []
    off: list[tuple[float, int]] = []
    matched = 0
    cursor = 0
    stamps = sorted(e["when"] for e in events)
    for t0, t1, per_frame, span in intervals:
        while cursor < len(stamps) and stamps[cursor] < t0:
            cursor += 1
        hit = cursor < len(stamps) and t0 <= stamps[cursor] <= t1
        if hit:
            matched += 1
            on.append((per_frame, span))
        else:
            off.append((per_frame, span))

    def flatten(pairs: list[tuple[float, int]]) -> list[float]:
        out: list[float] = []
        for value, span in pairs:
            out.extend([value] * span)
        return out

    on_frames = flatten(on)
    off_frames = flatten(off)
    all_frames = sorted(expand(intervals), reverse=True)
    worst_cut = all_frames[max(0, min(len(all_frames), 20) - 1)] if all_frames else float("inf")
    worst20_on = sum(1 for v in on_frames if v >= worst_cut)

    return {
        "events": len(events),
        "matched": matched,
        "on": stats(on_frames) if on_frames else None,
        "off": stats(off_frames) if off_frames else None,
        "worst20_on_recentre": worst20_on,
        "created_total": sum(e["created"] for e in events),
        "released_total": sum(e["released"] for e in events),
        "seconds": intervals[-1][1] - intervals[0][0],
    }


def report(path: Path, args) -> int:
    print("=" * 78)
    print(path)
    print("=" * 78)
    if not path.exists():
        print(f"  MISSING: {path}")
        return 1

    run = read_log(path)
    if not run.samples:
        print(f"  !! NO `FT` LINES in {path}. The staged init.sqs did not run, or the run "
              f"died before gameplay. This is not a zero — it is no measurement.")
        return 1
    build_intervals(run, args.drop_first, not args.keep_settle)
    if not run.intervals:
        print("  !! No phase had two usable trace samples.")
        return 1

    if args.marks:
        for mark in run.marks:
            print(f"  {mark}")
        print()

    header = (f"{'phase':<20}{'frames':>7}{'wall s':>8}{'median':>8}{'p95':>8}"
              f"{'p99':>9}{'worst':>10}{'fps':>7}{'>100ms':>8}{'>250ms':>8}{'1:1':>7}")
    print(header)
    print("-" * len(header))
    ordered_phases = sorted(run.intervals, key=lambda p: "WSM".find(p))
    summaries: dict[str, dict] = {}
    for phase in ordered_phases:
        row = stats(expand(run.intervals[phase]))
        summaries[phase] = row
        exact, counted = run.coverage.get(phase, (0, 0))
        cover = f"{exact / counted * 100:.0f}%" if counted else "n/a"
        print(f"{PHASE_NAMES.get(phase, phase):<20}{row['frames']:>7}{row['wall_seconds']:>8.1f}"
              f"{row['median_ms']:>8.1f}{row['p95_ms']:>8.1f}{row['p99_ms']:>9.1f}"
              f"{row['worst_ms']:>10.1f}{row['fps_throughput']:>7.1f}"
              f"{row['frames_over_100ms']:>8}{row['frames_over_250ms']:>8}{cover:>7}")
    print()
    for phase in ordered_phases:
        row = summaries[phase]
        print(f"  {PHASE_NAMES.get(phase, phase)}: median {row['median_ms']:.1f} ms "
              f"({row['fps_median']:.1f} fps) | throughput {row['fps_throughput']:.1f} fps | "
              f"{row['share_over_100ms'] * 100:.0f}% of wall time in frames over 100 ms")

    if "S" in summaries and "M" in summaries:
        s, m = summaries["S"], summaries["M"]
        print()
        print(f"  MOVING vs STATIC: median {s['median_ms']:.1f} -> {m['median_ms']:.1f} ms "
              f"({m['median_ms'] / s['median_ms']:.2f}x) | "
              f"throughput {s['fps_throughput']:.1f} -> {m['fps_throughput']:.1f} fps | "
              f"p99 {s['p99_ms']:.1f} -> {m['p99_ms']:.1f} ms | "
              f"worst {s['worst_ms']:.1f} -> {m['worst_ms']:.1f} ms")

    for phase in ordered_phases:
        exact, counted = run.coverage.get(phase, (0, 0))
        if counted and exact / counted < 0.98:
            run.notes.append(
                f"phase {phase}: only {exact / counted * 100:.1f}% of trace samples advanced "
                f"the renderer's frame counter by exactly 1. The rest were averaged across "
                f"the frames they spanned, which SMOOTHS the tail — the true p99 and worst "
                f"frame are at least as bad as reported, not better.")

    if args.residency:
        print()
        print("  --- residency recentre correlation " + "-" * 40)
        if not run.residency:
            print("    no `Modern object residency:` lines in this log at all.")
        for phase in ordered_phases:
            corr = correlate(run, phase)
            label = PHASE_NAMES.get(phase, phase)
            if corr is None:
                print(f"    {label}: no window-centre change occurred during this phase.")
                continue
            rate = corr["events"] / corr["seconds"] if corr["seconds"] > 0 else float("nan")
            print(f"    {label}: {corr['events']} recentre events in {corr['seconds']:.0f} s "
                  f"({rate:.2f}/s), {corr['matched']} landed inside a traced frame; "
                  f"created={corr['created_total']} released={corr['released_total']}")
            if corr["on"] and corr["off"]:
                on, off = corr["on"], corr["off"]
                print(f"      frames ON a recentre  : n={on['frames']:>5} median "
                      f"{on['median_ms']:>7.1f} ms  p99 {on['p99_ms']:>7.1f}  worst {on['worst_ms']:>7.1f}")
                print(f"      frames BETWEEN        : n={off['frames']:>5} median "
                      f"{off['median_ms']:>7.1f} ms  p99 {off['p99_ms']:>7.1f}  worst {off['worst_ms']:>7.1f}")
                print(f"      of the 20 worst frames in this phase, {corr['worst20_on_recentre']} "
                      f"sit on a recentre event")
                ratio = on["median_ms"] / off["median_ms"] if off["median_ms"] > 0 else float("nan")
                if ratio >= 2.0:
                    print(f"      => CONFIRMED: a recentre frame is {ratio:.1f}x the median "
                          f"of a frame between recentres.")
                else:
                    print(f"      => NOT CONFIRMED: a recentre frame is only {ratio:.1f}x a "
                          f"frame between recentres. The cost is NOT concentrated on "
                          f"recentre events; residency recentring does not explain this phase.")

    print()
    for note in run.notes:
        print(f"  !! {note}")
    if run.notes:
        print()
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+", type=Path, help="run-NN.log files from farfield-movecam")
    ap.add_argument("--drop-first", type=int, default=5,
                    help="frames to discard at the start of each phase (default 5): the first "
                         "frames after a phase change still carry the change's own cost")
    ap.add_argument("--keep-settle", action="store_true",
                    help="also report the discarded settle phase (useful to see the fill-in)")
    ap.add_argument("--marks", action="store_true", help="print the FFCAM phase markers")
    ap.add_argument("--residency", action="store_true",
                    help="correlate frame times against object-residency recentre events")
    args = ap.parse_args()

    exit_code = 0
    for path in args.logs:
        exit_code = max(exit_code, report(path, args))
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
