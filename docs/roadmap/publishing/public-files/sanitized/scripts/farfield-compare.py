#!/usr/bin/env python3
"""Compare --capture-metrics JSONs from two or more benchmark arms.

Reads the output of scripts/farfield-bench.ps1 (one directory per arm, N repeats each)
and prints a GPU frame comparison: frame total, the top-level timed regions, the
residual, object instance/triangle counts and the main-view LOD histogram -- plus the
CPU frame breakdown (`cpu_frame_phases_ms`) when the capturing binary emitted one.

House rules, all of them load-bearing:

  * THE CPU FRAME IS THE ONE THAT DECIDES.  On the heavy imported worlds the GPU is
    roughly 9% of wall clock (design notes), so a change that shaves
    GPU milliseconds off a CPU-bound world moves nothing a player can see.  The headline
    row is therefore "CPU frame MINUS GPU frame": the part of the frame the GPU is not
    even involved in.  Shrink that or the frame does not shrink.

  * QUOTE THE CHEAPEST STEADY FRAME, NEVER THE MEAN.  A shared GPU inflates every timed
    region by a common factor (design notes measured 27.9 ms vs 7.6 ms
    for the same scene with a second process resident, with cull 2.214 vs 0.449 and
    colour 0.565 vs 0.210 -- a clock/sharing effect, not extra work).  The minimum across
    repeats is the only statistic that contamination cannot push downwards, so this tool
    picks one whole repeat -- the one with the smallest GPU frame total -- and reports
    every number from that single capture.  Mixing regions from different repeats would
    produce a frame that never existed.

  * milliseconds == -1 MEANS "DID NOT RUN".  It is never 0 ms of work.  Summing it as
    zero is how a gated pass silently becomes part of an "accounted for" total; showing
    it as 0.000 is how a skipped pass gets read as a free pass.  Both are shown as
    "did not run", and a region that ran in one arm and not the other reports its delta
    as "gated", not as a speed-up.

  * REPORT THE RESIDUAL.  frame total ~= sum of the rows whose contained_by == -1 (that
    rule is emitted per row by the capture writer).  Roughly half of every frame is
    currently in passes with no timer region at all, so the difference between the frame
    and the attributed sum is a first-class row here, not a rounding error.

  * `land:gnd` IS NOT ONLY TERRAIN.  `UpdateModernObjectResidency` is called from
    `TerrainWgpu::DrawTerrain`, so object streaming is billed inside the terrain phase.
    A large `land:gnd` on a heavy world is object admission, not terrain rasterisation.
    The tool shouts this on the row itself, because reading it as terrain cost sends the
    next day's work at the wrong subsystem.

Usage:
    python scripts/farfield-compare.py --arm A=.tmp-farfield-bench/legacy \
                                       --arm B=.tmp-farfield-bench/tiered
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import sys

FRAME_TOTAL_REGION = "GPU frame total"
DID_NOT_RUN = -1.0

LOD_LABELS = ["LOD 0", "LOD 1", "LOD 2", "LOD 3", "LOD 4", "LOD 5", "LOD 6", "LOD 7+"]

# The CPU-side block WriteCaptureMetrics emits from Poseidon::Dev::FrameProfiler.
CPU_BLOCK = "cpu_frame_phases_ms"

# UpdateModernObjectResidency runs inside TerrainWgpu::DrawTerrain, so every millisecond of
# object streaming is billed to the terrain phase.  Anyone reading this row as "terrain is
# slow" optimises the wrong subsystem, so the row carries the warning wherever it is printed.
STREAMING_PHASE = "land:gnd"
STREAMING_TAG = "<<< +OBJECT STREAMING"
STREAMING_NOTE = (
    "!! land:gnd INCLUDES OBJECT STREAMING.  UpdateModernObjectResidency is called from\n"
    "!! TerrainWgpu::DrawTerrain, so object admission is billed inside the terrain phase.\n"
    "!! A large land:gnd on a heavy world is OBJECT cost, not terrain rasterisation cost."
)


class BadCapture(Exception):
    """A capture file that cannot be trusted as a measurement."""


class Capture:
    """One metrics JSON, validated far enough to be quoted as a measurement."""

    def __init__(self, path: str, payload: dict) -> None:
        self.path = path
        self.payload = payload
        self.name = os.path.basename(path)
        self.contended = False

        if not payload.get("gpu_timestamps_available"):
            raise BadCapture(
                f"{path}: gpu_timestamps_available is false -- every region would read 0. "
                "This is not a measured frame."
            )
        rows = payload.get("gpu_timings_ms")
        if not isinstance(rows, list) or not rows:
            raise BadCapture(f"{path}: gpu_timings_ms is missing or empty.")

        self.rows: list[dict] = []
        for row in rows:
            if not isinstance(row, dict) or "name" not in row or "milliseconds" not in row:
                raise BadCapture(f"{path}: gpu_timings_ms contains a row without name/milliseconds.")
            self.rows.append(row)

        self.by_name = {row["name"]: row for row in self.rows}
        frame_row = self.by_name.get(FRAME_TOTAL_REGION)
        if frame_row is None:
            raise BadCapture(f"{path}: no '{FRAME_TOTAL_REGION}' region.")
        self.frame_total = float(frame_row["milliseconds"])
        if self.frame_total <= 0.0:
            raise BadCapture(f"{path}: {FRAME_TOTAL_REGION} is {self.frame_total} ms -- not a measured frame.")

        # Outermost rows only: leaves point at their container, so containers count exactly
        # once.  The frame-total row is excluded BY NAME rather than by trusting its
        # contained_by, because captures written by older binaries report the whole-frame
        # envelope as outermost -- summing it there gives an "attributed" larger than the
        # frame and a NEGATIVE residual, which reads as a measurement instead of the schema
        # mismatch it is.  Seen on design notes
        self.top_level = [
            row
            for row in self.rows
            if int(row.get("contained_by", -1)) == -1 and row["name"] != FRAME_TOTAL_REGION
        ]
        self.attributed = sum(
            float(row["milliseconds"]) for row in self.top_level if float(row["milliseconds"]) > 0.0
        )
        self.residual = self.frame_total - self.attributed

        # ---- CPU frame phases ---------------------------------------------------------
        # Optional on purpose: every capture taken before the instrumentation landed has no
        # such block, and those captures are still perfectly good GPU measurements.  A
        # missing block is reported as missing; it is never defaulted to zeros, which would
        # read as "the CPU did no work" -- the single most misleading thing this tool could
        # print about a CPU-bound world.
        block = payload.get(CPU_BLOCK)
        self.cpu: dict | None = block if isinstance(block, dict) else None
        self.cpu_phases: dict[str, dict] = {}
        if self.cpu is not None:
            phases = self.cpu.get("phases")
            if isinstance(phases, dict):
                for name, stats in phases.items():
                    if isinstance(stats, dict):
                        self.cpu_phases[str(name)] = stats

    @property
    def has_cpu(self) -> bool:
        return self.cpu is not None

    def cpu_value(self, key: str) -> float | None:
        """A scalar off the CPU block (frame_avg, frame_p95, frame_max, avg_fps, ...)."""
        if self.cpu is None:
            return None
        value = self.cpu.get(key)
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            return None
        return float(value)

    def cpu_phase(self, name: str, stat: str = "avg") -> float | None:
        stats = self.cpu_phases.get(name)
        if not isinstance(stats, dict):
            return None
        value = stats.get(stat)
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            return None
        return float(value)

    @property
    def cpu_frame(self) -> float | None:
        return self.cpu_value("frame_avg")

    @property
    def cpu_attributed(self) -> float | None:
        """Sum of the per-phase averages.

        The profiler marks phases sequentially from the frame start, so the phases partition
        the frame and their averages are additive.  (p95 is NOT additive across phases --
        different frames peak in different phases -- so only the averages are ever summed.)
        """
        if self.cpu is None or not self.cpu_phases:
            return None
        total = 0.0
        for name in self.cpu_phases:
            value = self.cpu_phase(name)
            if value is not None:
                total += value
        return total

    @property
    def cpu_residual(self) -> float | None:
        frame = self.cpu_frame
        attributed = self.cpu_attributed
        if frame is None or attributed is None:
            return None
        return frame - attributed

    @property
    def cpu_minus_gpu(self) -> float | None:
        """The headline: wall-clock CPU frame with the GPU frame taken out.

        This is the part of the frame no GPU change can touch.  A negative value means the
        frame really is GPU-bound and GPU work is the right target -- rare on the imported
        worlds.  The two terms come from different windows (CPU is an average over the
        profiler ring, GPU is the single cheapest repeat), so read it as a magnitude, not
        as an exact subtraction of two simultaneous measurements.
        """
        frame = self.cpu_frame
        if frame is None:
            return None
        return frame - self.frame_total

    def ms(self, region_name: str) -> float | None:
        row = self.by_name.get(region_name)
        if row is None:
            return None
        return float(row["milliseconds"])

    @property
    def renderer(self) -> str:
        return str(self.payload.get("renderer", "?"))

    @property
    def main_instances(self) -> int:
        """Instances actually drawn in the main view. 0 means the capture landed
        before the world streamed in, which makes its frame time meaningless."""
        try:
            return int(self.objects.get("main_instances", 0) or 0)
        except (TypeError, ValueError):
            return 0

    @property
    def objects(self) -> dict:
        objects = self.payload.get("objects")
        if isinstance(objects, dict):
            return objects
        return {}


class Arm:
    def __init__(self, label: str, spec: str) -> None:
        self.label = label
        self.spec = spec
        self.captures: list[Capture] = []
        self.rejected: list[str] = []
        self.chosen: Capture | None = None
        # Repeats dropped for having drawn nothing; surfaced so a thin arm is visible.
        self.degenerate: list[str] = []

    def choose(self, include_contended: bool) -> None:
        pool = [c for c in self.captures if not c.contended]
        if include_contended or not pool:
            # Falling back to contended repeats is better than reporting nothing, but the
            # caller is warned about it at the end of the run.
            pool = list(self.captures)
        if not pool:
            return

        # Reject repeats that drew nothing before applying the cheapest-frame rule.
        #
        # "Quote the cheapest steady frame" defends against GPU contention, which
        # inflates every timed region by a common factor. It does NOT defend against a
        # repeat that captured before the world finished streaming: such a frame is
        # genuinely the cheapest in the set (one was observed at 0.923 ms with zero main
        # instances) and the minimum rule selects it every time. That silently turns an
        # A/B into a comparison against an empty scene, which is exactly the class of
        # false finding the rule exists to prevent.
        #
        # Only applied when some repeat DID draw, so a legitimately empty scene (an
        # ocean-facing pose, a world with no objects) still reports rather than erroring.
        drew = [c for c in pool if c.main_instances > 0]
        if drew and len(drew) < len(pool):
            for c in pool:
                if c.main_instances <= 0:
                    self.degenerate.append(c.name)
                    self.rejected.append(
                        f"{c.name}: 0 main instances -- captured before the world streamed in, "
                        f"so its {c.frame_total:.3f} ms frame is not a measurement"
                    )
            pool = drew

        # Cheapest steady frame -- see the module docstring for why this is a minimum.
        self.chosen = min(pool, key=lambda c: c.frame_total)


def expand_spec(spec: str) -> list[str]:
    """A spec is a file, a directory of run-*.json, or a glob."""
    if os.path.isdir(spec):
        found = sorted(glob.glob(os.path.join(spec, "*.json")))
    elif any(ch in spec for ch in "*?["):
        found = sorted(glob.glob(spec))
    elif os.path.isfile(spec):
        return [spec]
    else:
        return []
    # arm.json is the manifest and *.meta.json is per-run bookkeeping; neither is a capture.
    return [
        path
        for path in found
        if os.path.basename(path) != "arm.json" and not path.endswith(".meta.json")
    ]


def load_meta(capture_path: str) -> dict:
    meta_path = os.path.splitext(capture_path)[0] + ".meta.json"
    if not os.path.isfile(meta_path):
        return {}
    try:
        with open(meta_path, "r", encoding="utf-8-sig") as handle:
            return json.load(handle)
    except (OSError, ValueError):
        return {}


def load_arm(label: str, spec: str) -> Arm:
    arm = Arm(label, spec)
    paths = expand_spec(spec)
    if not paths:
        raise BadCapture(f"arm '{label}': nothing matched '{spec}' (no capture JSON found).")
    for path in paths:
        try:
            with open(path, "r", encoding="utf-8-sig") as handle:
                payload = json.load(handle)
        except OSError as error:
            arm.rejected.append(f"{os.path.basename(path)}: unreadable ({error})")
            continue
        except ValueError as error:
            arm.rejected.append(f"{os.path.basename(path)}: malformed JSON ({error})")
            continue
        try:
            capture = Capture(path, payload)
        except BadCapture as error:
            arm.rejected.append(str(error))
            continue
        meta = load_meta(path)
        if meta.get("performance_comparable") is False:
            arm.rejected.append(f"{os.path.basename(path)}: visual motion sequence, not a performance sample")
            continue
        capture.contended = bool(meta.get("contended", False))
        arm.captures.append(capture)
    if not arm.captures:
        detail = "; ".join(arm.rejected) if arm.rejected else "no usable files"
        raise BadCapture(f"arm '{label}': no valid capture in '{spec}' -- {detail}")
    return arm


# ---------------------------------------------------------------------------------------
# formatting
# ---------------------------------------------------------------------------------------

VALUE_WIDTH = 13
NAME_WIDTH = 38


def fmt_ms(value: float | None) -> str:
    if value is None:
        return "absent"
    if value < 0:
        return "did not run"
    return f"{value:.3f}"


def fmt_count(value) -> str:
    if value is None:
        return "absent"
    try:
        return f"{int(value):,}"
    except (TypeError, ValueError):
        return str(value)


def delta_cells(base: float | None, other: float | None) -> tuple[str, str]:
    """Delta and percentage against the first arm, honouring 'did not run'."""
    if base is None or other is None:
        return ("n/a", "n/a")
    base_ran = base >= 0
    other_ran = other >= 0
    if base_ran != other_ran:
        # One arm skipped the pass entirely.  That is a gate firing, not a speed-up, and
        # subtracting -1 from a real time would manufacture a number.
        return ("gated", "gated")
    if not base_ran and not other_ran:
        return ("--", "--")
    delta = other - base
    if abs(base) < 1e-9:
        return (f"{delta:+.3f}", "n/a")
    return (f"{delta:+.3f}", f"{(delta / base) * 100.0:+.1f}%")


def fmt_cpu_ms(value: float | None) -> str:
    """CPU timings have no 'did not run' sentinel -- every phase is always entered.

    So a negative number here is a real negative (the CPU-minus-GPU row on a GPU-bound
    frame), and must print as such rather than being swallowed by the -1 convention that
    governs the GPU region table.
    """
    if value is None:
        return "absent"
    return f"{value:.3f}"


def delta_cells_plain(base: float | None, other: float | None) -> tuple[str, str]:
    """Delta for values with no sentinel encoding -- plain subtraction."""
    if base is None or other is None:
        return ("n/a", "n/a")
    delta = other - base
    if abs(base) < 1e-9:
        return (f"{delta:+.3f}", "n/a")
    return (f"{delta:+.3f}", f"{(delta / abs(base)) * 100.0:+.1f}%")


def row_line(name: str, cells: list[str], indent: int = 0) -> str:
    label = (" " * indent) + name
    if len(label) > NAME_WIDTH:
        label = label[: NAME_WIDTH - 1] + "…"
    parts = [label.ljust(NAME_WIDTH)]
    for cell in cells:
        parts.append(cell.rjust(VALUE_WIDTH))
    return "".join(parts)


def header_line(arms: list[Arm], extra: list[str]) -> str:
    return row_line("", [arm.label for arm in arms] + extra)


def separator() -> str:
    return "-" * (NAME_WIDTH + VALUE_WIDTH * 4)


# ---------------------------------------------------------------------------------------
# report
# ---------------------------------------------------------------------------------------


def report_cpu(arms: list[Arm], chosen: list[Capture], two_arm: bool, extra: list[str], show_all: bool) -> None:
    """The CPU side of the frame, and the headline CPU-minus-GPU row.

    Every row here is an AVERAGE over the profiler's ring (up to 256 frames), not the single
    cheapest frame the GPU table quotes.  The two windows differ; that is stated in the
    output rather than papered over, because silently mixing them is how a 'CPU frame' gets
    compared against a GPU frame from a different second and the difference gets believed.
    """
    print()
    print("CPU FRAME (ms)")
    print(separator())

    missing = [arm.label for arm, capture in zip(arms, chosen) if not capture.has_cpu]
    if len(missing) == len(arms):
        print(f"  {CPU_BLOCK} absent (binary predates the instrumentation)")
        print(f"    arms without it: {', '.join(missing)}")
        print("    The CPU side of these frames was never recorded. It is NOT zero, and it is")
        print("    NOT covered by the GPU numbers above -- on the imported worlds the GPU is")
        print("    about 9% of wall clock. Re-capture with a binary that emits the block")
        print("    before concluding anything about frame time from this run.")
        return
    if missing:
        print(f"  {CPU_BLOCK} absent (binary predates the instrumentation) in: {', '.join(missing)}")
        print("    Those arms show 'absent' below -- not zero. A mixed A/B like this compares")
        print("    two different binaries; re-capture both arms before quoting a CPU delta.")

    print(header_line(arms, extra))
    print(separator())

    frames_cells = [fmt_count(capture.cpu_value("sampled_frames")) for capture in chosen]
    print(row_line("sampled frames (profiler ring)", frames_cells))

    fps_cells = []
    for capture in chosen:
        value = capture.cpu_value("avg_fps")
        fps_cells.append("absent" if value is None else f"{value:.2f}")
    if two_arm:
        fps_cells += list(delta_cells_plain(chosen[0].cpu_value("avg_fps"), chosen[1].cpu_value("avg_fps")))
    print(row_line("avg fps", fps_cells))

    for key, label in (("frame_avg", "CPU frame (avg)"), ("frame_p95", "CPU frame (p95)"), ("frame_max", "CPU frame (max)")):
        cells = [fmt_cpu_ms(capture.cpu_value(key)) for capture in chosen]
        if two_arm:
            cells += list(delta_cells_plain(chosen[0].cpu_value(key), chosen[1].cpu_value(key)))
        print(row_line(label, cells))

    gpu_cells = [f"{capture.frame_total:.3f}" for capture in chosen]
    if two_arm:
        gpu_cells += list(delta_cells(chosen[0].frame_total, chosen[1].frame_total))
    print(row_line("GPU frame total (cheapest repeat)", gpu_cells))

    print(separator())
    headline_cells = [fmt_cpu_ms(capture.cpu_minus_gpu) for capture in chosen]
    if two_arm:
        headline_cells += list(delta_cells_plain(chosen[0].cpu_minus_gpu, chosen[1].cpu_minus_gpu))
    print(row_line(">> CPU frame MINUS GPU frame", headline_cells))

    share_cells = []
    for capture in chosen:
        frame = capture.cpu_frame
        if frame is None or frame <= 0.0:
            share_cells.append("absent")
        else:
            share_cells.append(f"{capture.frame_total / frame * 100.0:.1f}%")
    print(row_line("   GPU share of the CPU frame", share_cells))
    print(separator())
    print("  >> is the headline. It is the part of the frame no GPU change can touch: if it")
    print("     dominates, a GPU-only win moves nothing. A NEGATIVE value means the frame")
    print("     really is GPU-bound (or the two measurement windows drifted -- CPU rows are")
    print("     averaged over the ring, the GPU row is the single cheapest repeat).")

    # ---- per-phase ---------------------------------------------------------------------
    ordered: list[str] = []
    for capture in chosen:
        for name in capture.cpu_phases:
            if name not in ordered:
                ordered.append(name)

    def phase_peak(name: str) -> float:
        best = None
        for capture in chosen:
            value = capture.cpu_phase(name)
            if value is not None and (best is None or value > best):
                best = value
        return -1.0 if best is None else best

    ordered.sort(key=lambda name: -phase_peak(name))

    phaseless = [
        arm.label for arm, capture in zip(arms, chosen) if capture.has_cpu and not capture.cpu_phases
    ]
    if not ordered:
        print()
        print("  no arm's CPU block carries a 'phases' object -- there is a frame total but no")
        print("  breakdown. The frame rows above still stand; nothing below them can be said.")
        return

    print()
    print("CPU PHASES (ms, avg per frame; DESCENDING -- the dominant phase is first)")
    print(separator())
    if phaseless:
        print(f"  no 'phases' object in: {', '.join(phaseless)} -- every phase reads 'absent' there,")
        print("  which means unrecorded, not zero.")
    print(header_line(arms, extra))
    print(separator())
    for name in ordered:
        label = name
        if name == STREAMING_PHASE:
            label = f"{name}  {STREAMING_TAG}"
        cells = [fmt_cpu_ms(capture.cpu_phase(name)) for capture in chosen]
        if two_arm:
            cells += list(delta_cells_plain(chosen[0].cpu_phase(name), chosen[1].cpu_phase(name)))
        print(row_line(label, cells))

    print(separator())
    attributed_cells = [fmt_cpu_ms(capture.cpu_attributed) for capture in chosen]
    if two_arm:
        attributed_cells += list(delta_cells_plain(chosen[0].cpu_attributed, chosen[1].cpu_attributed))
    print(row_line("attributed (sum of phase avgs)", attributed_cells))

    residual_cells = []
    for capture in chosen:
        residual = capture.cpu_residual
        frame = capture.cpu_frame
        if residual is None:
            residual_cells.append("absent")
        elif frame and frame > 0.0:
            residual_cells.append(f"{residual:.3f} ({residual / frame * 100.0:.0f}%)")
        else:
            residual_cells.append(f"{residual:.3f}")
    if two_arm:
        residual_cells += list(delta_cells_plain(chosen[0].cpu_residual, chosen[1].cpu_residual))
    print(row_line("unattributed (frame - phases)", residual_cells))
    print("  (only AVERAGES are summed: p95 is not additive across phases -- different")
    print("   frames peak in different phases, so summing p95 invents a frame that never ran)")

    if STREAMING_PHASE in ordered:
        print()
        print("!" * (NAME_WIDTH + VALUE_WIDTH * 4))
        print(STREAMING_NOTE)
        print("!" * (NAME_WIDTH + VALUE_WIDTH * 4))

    if show_all:
        print()
        print("CPU PHASES (ms, p95 -- spike behaviour; never add these up)")
        print(separator())
        print(header_line(arms, extra))
        print(separator())
        for name in ordered:
            label = name
            if name == STREAMING_PHASE:
                label = f"{name}  {STREAMING_TAG}"
            cells = [fmt_cpu_ms(capture.cpu_phase(name, "p95")) for capture in chosen]
            if two_arm:
                cells += list(
                    delta_cells_plain(chosen[0].cpu_phase(name, "p95"), chosen[1].cpu_phase(name, "p95"))
                )
            print(row_line(label, cells))


def report(arms: list[Arm], show_all_regions: bool, top: int) -> None:
    two_arm = len(arms) == 2
    extra = ["delta", "%"] if two_arm else []
    chosen = [arm.chosen for arm in arms]

    print()
    print("=" * (NAME_WIDTH + VALUE_WIDTH * 4))
    print("CAPTURES  (cheapest steady frame per arm; the mean is never quoted -- RND-033)")
    print("=" * (NAME_WIDTH + VALUE_WIDTH * 4))
    for arm in arms:
        totals = []
        for capture in sorted(arm.captures, key=lambda c: c.name):
            mark = ""
            if capture is arm.chosen:
                mark = "*"
            if capture.contended:
                mark += "!"
            totals.append(f"{capture.name}={capture.frame_total:.3f}{mark}")
        print(f"  {arm.label}: {len(arm.captures)} capture(s)  {'  '.join(totals)}")
        if arm.rejected:
            for message in arm.rejected:
                print(f"      rejected: {message}")
        if arm.chosen is not None:
            print(f"      quoting {arm.chosen.name}  renderer={arm.chosen.renderer}")
    print("  (* = quoted, ! = another game process was resident: treat as contaminated)")

    print()
    print("FRAME BUDGET (ms)")
    print(separator())
    print(header_line(arms, extra))
    print(separator())

    frame_cells = [f"{c.frame_total:.3f}" for c in chosen]
    if two_arm:
        frame_cells += list(delta_cells(chosen[0].frame_total, chosen[1].frame_total))
    print(row_line(FRAME_TOTAL_REGION, frame_cells))

    attributed_cells = [f"{c.attributed:.3f}" for c in chosen]
    if two_arm:
        attributed_cells += list(delta_cells(chosen[0].attributed, chosen[1].attributed))
    print(row_line("attributed (sum of top-level)", attributed_cells))

    residual_cells = []
    for capture in chosen:
        share = 0.0
        if capture.frame_total > 0:
            share = capture.residual / capture.frame_total * 100.0
        residual_cells.append(f"{capture.residual:.3f} ({share:.0f}%)")
    if two_arm:
        residual_cells += list(delta_cells(chosen[0].residual, chosen[1].residual))
    print(row_line("residual (untimed passes)", residual_cells))
    for arm, capture in zip(arms, chosen):
        if capture.residual < 0:
            print(
                f"  WARNING: {arm.label} attributes more time than the frame it explains "
                f"({capture.attributed:.3f} > {capture.frame_total:.3f} ms). Its region-nesting "
                "table disagrees with this tool -- do not quote these numbers."
            )

    report_cpu(arms, chosen, two_arm, extra, show_all_regions)

    # ---- top-level regions -------------------------------------------------------------
    ordered_names: list[str] = []
    for row in chosen[0].top_level:
        ordered_names.append(row["name"])
    for capture in chosen[1:]:
        for row in capture.top_level:
            if row["name"] not in ordered_names:
                ordered_names.append(row["name"])

    def sort_key(name: str) -> float:
        best = -2.0
        for capture in chosen:
            value = capture.ms(name)
            if value is not None and value > best:
                best = value
        return -best

    ordered_names.sort(key=sort_key)
    ran = [name for name in ordered_names if sort_key(name) < 0]
    idle = [name for name in ordered_names if sort_key(name) >= 0]
    shown = ran
    if top > 0 and not show_all_regions:
        shown = ran[:top]

    print()
    print("TOP-LEVEL TIMED REGIONS (ms; contained_by == -1, so these sum to 'attributed')")
    print(separator())
    print(header_line(arms, extra))
    print(separator())
    for name in shown:
        cells = [fmt_ms(capture.ms(name)) for capture in chosen]
        if two_arm:
            cells += list(delta_cells(chosen[0].ms(name), chosen[1].ms(name)))
        print(row_line(name, cells))
    if top > 0 and not show_all_regions and len(ran) > top:
        print(row_line(f"... {len(ran) - top} more regions (--all-regions)", []))
    if idle:
        print()
        print(f"  {len(idle)} region(s) reported 'did not run' in every arm:")
        for name in idle:
            print(f"      {name}")

    if show_all_regions:
        print()
        print("NESTED REGIONS (ms; each is INSIDE a top-level row above -- never add these in)")
        print(separator())
        print(header_line(arms, extra))
        print(separator())
        index_to_name = {int(row["index"]): row["name"] for row in chosen[0].rows if "index" in row}
        for row in chosen[0].rows:
            parent = int(row.get("contained_by", -1))
            if parent == -1 or parent == int(row.get("index", -1)):
                continue
            name = row["name"]
            parent_name = index_to_name.get(parent, f"#{parent}")
            cells = [fmt_ms(capture.ms(name)) for capture in chosen]
            if two_arm:
                cells += list(delta_cells(chosen[0].ms(name), chosen[1].ms(name)))
            print(row_line(f"{name}  [in {parent_name}]", cells, indent=2))

    # ---- objects -----------------------------------------------------------------------
    print()
    print("OBJECTS")
    print(separator())
    print(header_line(arms, extra))
    print(separator())
    valid_flags = []
    for capture in chosen:
        objects = capture.objects
        if not objects:
            valid_flags.append("absent")
        elif objects.get("valid"):
            valid_flags.append("measured")
        else:
            valid_flags.append("NOT MEASURED")
    print(row_line("readback valid", valid_flags))
    if "NOT MEASURED" in valid_flags:
        print("  (valid == false means no readback landed; the counts below are unknown, not zero)")

    object_fields = [
        ("registered_instances", "registered instances"),
        ("main_instances", "main instances"),
        ("main_records", "main records"),
        ("main_draws", "main draws"),
        ("main_tris", "main triangles"),
        ("color_instances", "colour instances"),
        ("color_tris", "colour triangles"),
        ("direct_instances", "direct instances"),
        ("direct_tris", "direct triangles"),
    ]
    for key, label in object_fields:
        values = [capture.objects.get(key) for capture in chosen]
        if all(value is None for value in values):
            continue
        cells = [fmt_count(value) for value in values]
        if two_arm:
            first, second = values[0], values[1]
            if first is None or second is None:
                cells += ["n/a", "n/a"]
            else:
                delta = int(second) - int(first)
                if int(first) == 0:
                    cells += [f"{delta:+,}", "n/a"]
                else:
                    cells += [f"{delta:+,}", f"{(delta / int(first)) * 100.0:+.1f}%"]
        print(row_line(label, cells))

    # shadow cascades
    cascade_present = any(isinstance(capture.objects.get("shadow_cascades"), list) for capture in chosen)
    if cascade_present:
        for cascade in range(4):
            values = []
            for capture in chosen:
                cascades = capture.objects.get("shadow_cascades")
                entry = None
                if isinstance(cascades, list) and cascade < len(cascades):
                    entry = cascades[cascade]
                if isinstance(entry, dict):
                    values.append(entry.get("tris"))
                else:
                    values.append(None)
            if all(value is None for value in values):
                continue
            cells = [fmt_count(value) for value in values]
            if two_arm and values[0] is not None and values[1] is not None:
                delta = int(values[1]) - int(values[0])
                if int(values[0]) == 0:
                    cells += [f"{delta:+,}", "n/a"]
                else:
                    cells += [f"{delta:+,}", f"{(delta / int(values[0])) * 100.0:+.1f}%"]
            elif two_arm:
                cells += ["n/a", "n/a"]
            print(row_line(f"shadow cascade {cascade} triangles", cells))

    # ---- LOD histogram -----------------------------------------------------------------
    histograms = [capture.objects.get("main_lod_histogram") for capture in chosen]
    if any(isinstance(histogram, list) for histogram in histograms):
        print()
        print("MAIN-VIEW LOD HISTOGRAM (instances surviving the main cull)")
        print(separator())
        print(header_line(arms, extra))
        print(separator())
        for bucket in range(8):
            values = []
            for histogram in histograms:
                if isinstance(histogram, list) and bucket < len(histogram):
                    values.append(histogram[bucket])
                else:
                    values.append(None)
            if all(value is None for value in values):
                continue
            cells = [fmt_count(value) for value in values]
            if two_arm and values[0] is not None and values[1] is not None:
                delta = int(values[1]) - int(values[0])
                cells += [f"{delta:+,}", ""]
            elif two_arm:
                cells += ["n/a", ""]
            print(row_line(LOD_LABELS[bucket], cells))
        totals = []
        for histogram in histograms:
            if isinstance(histogram, list):
                totals.append(sum(int(value) for value in histogram))
            else:
                totals.append(None)
        cells = [fmt_count(value) for value in totals]
        if two_arm and totals[0] is not None and totals[1] is not None:
            delta = totals[1] - totals[0]
            cells += [f"{delta:+,}", ""]
        elif two_arm:
            cells += ["n/a", ""]
        print(row_line("total", cells))
    print()


def parse_arm(value: str) -> tuple[str, str]:
    if "=" not in value:
        raise argparse.ArgumentTypeError(f"--arm needs LABEL=PATH, got '{value}'")
    label, _, spec = value.partition("=")
    label = label.strip()
    spec = spec.strip()
    if not label or not spec:
        raise argparse.ArgumentTypeError(f"--arm needs a non-empty LABEL and PATH, got '{value}'")
    return (label, spec)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Compare GPU capture metrics across benchmark arms.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--arm",
        action="append",
        type=parse_arm,
        metavar="LABEL=PATH",
        help="An arm: a directory of run-*.json, a glob, or a single JSON. Repeat for A/B.",
    )
    parser.add_argument("--all-regions", action="store_true", help="Show every region, including nested ones.")
    parser.add_argument("--top", type=int, default=16, help="Top-level regions to list (default 16; 0 = all).")
    parser.add_argument(
        "--include-contended",
        action="store_true",
        help="Allow repeats that ran alongside another game process (default: excluded when possible).",
    )
    parser.add_argument("--json", metavar="PATH", help="Also write the quoted numbers as JSON.")
    args = parser.parse_args(argv)

    if not args.arm or len(args.arm) < 2:
        parser.error("at least two --arm LABEL=PATH values are required (this is an A/B tool)")

    arms: list[Arm] = []
    errors: list[str] = []
    for label, spec in args.arm:
        try:
            arms.append(load_arm(label, spec))
        except BadCapture as error:
            errors.append(str(error))
    if errors:
        for message in errors:
            print(f"error: {message}", file=sys.stderr)
        return 1

    for arm in arms:
        arm.choose(args.include_contended)
        if arm.chosen is None:
            print(f"error: arm '{arm.label}' has no usable capture after filtering.", file=sys.stderr)
            return 1

    report(arms, args.all_regions, args.top)

    if args.json:
        payload = {}
        for arm in arms:
            capture = arm.chosen
            payload[arm.label] = {
                "quoted_capture": capture.path,
                "contended": capture.contended,
                "repeats": len(arm.captures),
                "frame_total_ms": capture.frame_total,
                "attributed_ms": capture.attributed,
                "residual_ms": capture.residual,
                "regions": {row["name"]: float(row["milliseconds"]) for row in capture.top_level},
                "objects": capture.objects,
            }
            # null, not 0.0, when the binary predates the CPU instrumentation: a consumer
            # that sees zeros will believe them.
            if capture.has_cpu:
                payload[arm.label]["cpu"] = {
                    "present": True,
                    "sampled_frames": (
                        None
                        if capture.cpu_value("sampled_frames") is None
                        else int(capture.cpu_value("sampled_frames"))
                    ),
                    "avg_fps": capture.cpu_value("avg_fps"),
                    "frame_avg_ms": capture.cpu_frame,
                    "frame_p95_ms": capture.cpu_value("frame_p95"),
                    "frame_max_ms": capture.cpu_value("frame_max"),
                    "cpu_minus_gpu_ms": capture.cpu_minus_gpu,
                    "phases_attributed_ms": capture.cpu_attributed,
                    "phases_unattributed_ms": capture.cpu_residual,
                    "phases": {name: dict(stats) for name, stats in capture.cpu_phases.items()},
                    "note": (
                        f"{STREAMING_PHASE} includes object streaming "
                        "(UpdateModernObjectResidency runs inside TerrainWgpu::DrawTerrain)"
                    ),
                }
            else:
                payload[arm.label]["cpu"] = {
                    "present": False,
                    "reason": f"{CPU_BLOCK} absent (binary predates the instrumentation)",
                }
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, indent=2)
        print(f"wrote {args.json}")

    contaminated = [arm.label for arm in arms if arm.chosen is not None and arm.chosen.contended]
    if contaminated:
        print(
            "warning: quoted capture(s) for "
            + ", ".join(contaminated)
            + " ran alongside another game process; a shared GPU inflates every region by a "
            "common factor. Re-run these arms alone before trusting the deltas.",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
