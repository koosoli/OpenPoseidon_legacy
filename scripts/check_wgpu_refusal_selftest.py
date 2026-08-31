#!/usr/bin/env python3
"""Prove scripts/check_wgpu_refusal.py rejects a broken no-silent-fallback contract.

A CI assertion nobody has ever seen fail is not an assertion; it is a green tick.
This drives the checker against stub "game" binaries -- one that honours the
contract and seven that break it in each of the ways the contract can actually
break -- and requires exactly one pass and seven failures.

It needs no GPU, no game data and no game binary, so it runs in the same lint
job as the rest of the repo's self-testing gates (see lint.yml's
"Validate explicit visual-review acceptance gate", which does the same thing to
the capture-comparison harness).

The stubs are the honest limit of this file: they exercise the CHECKER, not the
engine. That the engine still emits these lines is what the real run in
build.yml's `wgpu-refusal` job establishes.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

CHECKER = Path(__file__).resolve().parent / "check_wgpu_refusal.py"

PROBE = "WGR_FORCE_INIT_FAIL=1: refusing to create the WGPU backend (gate bullet 7 probe)"
REFUSAL = (
    "Requested WGPU renderer is unavailable; refusing automatic GL33 fallback. "
    "Pass --render gl33 explicitly, or --render auto, if a fallback is wanted."
)

# Each stub is a tiny program standing in for the game client.  It reads
# --render and WGR_FORCE_INIT_FAIL and prints what that build would print.
STUB = '''\
import os, sys
render = sys.argv[sys.argv.index("--render") + 1]
forced = os.environ.get("WGR_FORCE_INIT_FAIL") == "1"
BREAK = {break_!r}

if render == "dummy":
    if BREAK == "control-dies":
        sys.exit(3)
    print("Dummy: headless engine")
    print("Initialization check complete - exiting")
    sys.exit(0)

if render == "wgpu":
    if BREAK == "probe-ignored":
        # WGR_FORCE_INIT_FAIL no longer read; the runner's own lack of a GPU
        # still produces a refusal and a non-zero exit, so the arm "passes"
        # without the forcing mechanism working at all.
        print({refusal!r})
        sys.exit(1)
    if forced:
        if BREAK != "no-probe-log":
            print({probe!r})
        if BREAK in ("silent-fallback", "fallback-then-fail"):
            # The `wgpu` arm of CreateGraphicsEngine deleted: falls through to Auto.
            print("Unknown or unavailable render backend 'wgpu', defaulting to Auto")
            print("GL33: Initializing engine - bootstrap 1024x768")
            if BREAK == "fallback-then-fail":
                # Fell back and started GL33, then died later for an unrelated
                # reason. Exit code alone cannot see this; the markers can.
                print({refusal!r})
                sys.exit(1)
            print("Initialization check complete - exiting")
            sys.exit(0)
        if BREAK == "refusal-unlogged":
            # The LOG_ERROR reverted to RptF: refuses correctly, says nothing.
            sys.exit(1)
        if BREAK == "exit-zero":
            print({refusal!r})
            sys.exit(0)
        print({refusal!r})
        sys.exit(1)
    print("Wgpu: creating renderer WGPU (Rust / wgpu)")
    print("Initialization check complete - exiting")
    sys.exit(0)

sys.exit(0)
'''

CASES = [
    (None, True, "honours the contract"),
    ("silent-fallback", False, "explicit wgpu falls through to Auto and starts GL33"),
    ("fallback-then-fail", False, "falls back to GL33, then exits non-zero for another reason"),
    ("exit-zero", False, "refuses, logs it, but exits 0"),
    ("refusal-unlogged", False, "refuses and exits 1 but logs no reason (the RptF regression)"),
    ("no-probe-log", False, "forcing works but the probe line is gone"),
    ("probe-ignored", False, "WGR_FORCE_INIT_FAIL no longer read (arm B passes vacuously)"),
    ("control-dies", False, "the control arm itself is broken, so nothing is proven"),
]


def main() -> int:
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        data_dir = tmpdir / "data"
        data_dir.mkdir()
        for break_, should_pass, description in CASES:
            stub = tmpdir / f"stub_{break_ or 'good'}.py"
            stub.write_text(STUB.format(break_=break_, probe=PROBE, refusal=REFUSAL), encoding="utf-8")
            proc = subprocess.run(
                [
                    sys.executable,
                    str(CHECKER),
                    "--exe",
                    str(stub),
                    "--data-dir",
                    str(data_dir),
                    "--out-dir",
                    str(tmpdir / f"out_{break_ or 'good'}"),
                    "--launcher",
                    sys.executable,
                ],
                capture_output=True,
                text=True,
                env={**os.environ, "PYTHONIOENCODING": "utf-8"},
            )
            passed = proc.returncode == 0
            verdict = "PASS" if passed else "FAIL"
            expected = "PASS" if should_pass else "FAIL"
            ok = passed == should_pass
            print(f"[{'ok ' if ok else 'BAD'}] {break_ or 'good':<18} -> {verdict} (want {expected}) : {description}")
            if not ok:
                failures.append((break_, description, proc.stdout + proc.stderr))
            elif not passed:
                # Show the reason the checker gave, so a reader can see it is the
                # right reason and not an unrelated crash.
                reason = next(
                    (line for line in (proc.stdout + proc.stderr).splitlines() if line.startswith("FAIL:")),
                    "(no FAIL: line)",
                )
                print(f"                          {reason}")

    if failures:
        print("\nself-test failed; the WGPU refusal checker does not behave as documented:", file=sys.stderr)
        for break_, description, output in failures:
            print(f"\n--- {break_}: {description} ---\n{output}", file=sys.stderr)
        return 1
    print(f"\nAll {len(CASES)} cases behaved as specified: the checker accepts the contract and rejects {len(CASES) - 1} ways of breaking it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
