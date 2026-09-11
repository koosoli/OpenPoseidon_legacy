"""Strict parser for bounded FrameProfiler windows, not log-time interpolation."""
import math
import re


def parse_windows(lines):
    windows = {}
    phase = None
    active = None
    armed = False
    for line in lines:
        if "FFCAM phase=static " in line or "FFCAM phase=move " in line:
            if active is not None:
                raise ValueError("phase changed inside an incomplete capture")
            phase = "S" if "phase=static " in line else "M"
        if "triPerfCapture " not in line:
            continue
        if "triPerfCapture armed " in line:
            armed = True
        if "triPerfCapture failed " in line:
            raise ValueError("engine failed to start capture")
        if "triPerfCapture begin " in line:
            match = re.search(r"begin n=(\d+) dropped=(\d+)", line)
            if not match or active is not None or phase is None or phase in windows:
                raise ValueError("invalid or duplicate capture header")
            count, dropped = map(int, match.groups())
            if not 0 < count <= 16384 or dropped:
                raise ValueError("empty or overflowing capture")
            active = (count, [])
        elif "triPerfCapture offset=" in line:
            match = re.search(r"offset=(\d+) ms=(\S+)", line)
            if not match or active is None:
                raise ValueError("orphan or malformed capture chunk")
            count, values = active
            chunk = [float(value) for value in match[2].split(",")]
            if (int(match[1]) != len(values) or not 0 < len(chunk) <= 128
                    or len(values) + len(chunk) > count
                    or any(not math.isfinite(value) or value < 0 for value in chunk)):
                raise ValueError("invalid capture offset, count or duration")
            values.extend(chunk)
        elif "triPerfCapture end" in line:
            if active is None or len(active[1]) != active[0]:
                raise ValueError("incomplete capture")
            windows[phase] = active[1]
            active = None
    if active is not None:
        raise ValueError("truncated capture")
    if (armed or windows) and set(windows) != {"S", "M"}:
        raise ValueError("capture missing a measured phase")
    return windows
