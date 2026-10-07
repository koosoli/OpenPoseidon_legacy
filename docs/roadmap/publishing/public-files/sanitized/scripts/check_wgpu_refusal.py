#!/usr/bin/env python3
"""Assert the WGPU no-silent-fallback contract, on a machine with no GPU.

REN-GL33-001 gate bullet 5 asks CI to contain "an explicit WGPU-start assertion".
On GitHub-hosted runners that is not available: there is no GPU, and
`Renderer::new` (engine/WgpuRenderer/rust/src/lib.rs:984,1016) hard-refuses any
adapter without TEXTURE_BINDING_ARRAY + non-uniform indexing and
VERTEX_WRITABLE_STORAGE.  See design notes for the
constraint analysis.

What IS assertable anywhere is the other half of the same contract, and it is
the half that can regress silently: **an explicit `--render wgpu` that cannot
create WGPU must fail loudly and must never fall through to GL33.**  It is
triggerable on demand via WGR_FORCE_INIT_FAIL=1
(engine/WgpuRenderer/GraphicsBackendWgpu.cpp:29), which refuses at the factory,
before any device or window bring-up -- so it behaves identically on a
workstation with a 4090 and on a headless runner.

Three arms, all of which must hold; the first exists so the other two cannot
pass vacuously:

  A. control      --render dummy, WGR_FORCE_INIT_FAIL=1
                  -> exit 0, no probe line, no refusal line.
                  Proves the binary + data + harness work at all, and that the
                  probe is scoped to the WGPU factory rather than breaking the
                  process globally.  Without this arm, a binary that crashed on
                  startup for any reason would "pass" arm B.

  B. refusal      --render wgpu, WGR_FORCE_INIT_FAIL=1
                  -> non-zero exit, probe line present, refusal line present,
                     and NO evidence a renderer was created (no GL33 init, no
                     wgpu creation, no "defaulting to Auto").
                  This is the contract.

  C. mechanism    arm B's probe line must be ABSENT from arm A.
                  Proves the env var is actually read, not that the runner
                  happens to fail WGPU anyway (which it does, and which would
                  otherwise make arm B's exit code meaningless).

What this does NOT assert: that WGPU starts.  Nothing here touches a device.
Say so anywhere this script's result is quoted.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

# Exact log text, with the file each comes from.  If a message is reworded the
# check must fail loudly rather than quietly stop looking for it -- that is the
# whole point, and it is why these are asserted as substrings of the run's own
# output and not pattern-matched loosely.
PROBE_LINE = "WGR_FORCE_INIT_FAIL=1: refusing to create the WGPU backend"
REFUSAL_LINE = "refusing automatic GL33 fallback"

# Evidence that *some* renderer came up.  Any of these in the refusal arm means
# the refusal did not hold.
GL33_CREATED = "GL33: Initializing engine"  # engine/PoseidonGL33/EngineGL33.cpp:267
WGPU_CREATED = "Wgpu: creating renderer"  # engine/WgpuRenderer/EngineWgpu.cpp:805
SILENT_FALLBACK = "defaulting to Auto"  # apps/cwr/Game/GameApplication.cpp

TIMEOUT_SEC = 300


class Failure(Exception):
    pass


def run(
    exe: Path, data_dir: Path, backend: str, force: bool, out_dir: Path, launcher: list[str]
) -> tuple[int, str]:
    env = dict(os.environ)
    if force:
        env["WGR_FORCE_INIT_FAIL"] = "1"
    else:
        env.pop("WGR_FORCE_INIT_FAIL", None)
    argv = [
        *launcher,
        str(exe),
        "-C",
        str(data_dir),
        "--window",
        "--check",
        "--log-format",
        "jsonl",
        "--render",
        backend,
    ]
    print(f"$ WGR_FORCE_INIT_FAIL={'1' if force else '<unset>'} {' '.join(argv)}", flush=True)
    try:
        proc = subprocess.run(
            argv,
            env=env,
            capture_output=True,
            text=True,
            errors="replace",
            timeout=TIMEOUT_SEC,
        )
    except subprocess.TimeoutExpired as exc:
        raise Failure(f"{backend} arm did not exit within {TIMEOUT_SEC}s") from exc
    output = (proc.stdout or "") + (proc.stderr or "")
    log_path = out_dir / f"wgpu-refusal-{backend}{'-forced' if force else ''}.log"
    log_path.write_text(output, encoding="utf-8", errors="replace")
    print(f"  exit={proc.returncode}  log={log_path}", flush=True)
    return proc.returncode, output


def require(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", required=True, type=Path, help="game client binary (PoseidonGame / OpenPoseidon.exe)")
    parser.add_argument("--data-dir", required=True, type=Path, help="game data dir passed as -C (Demo data is enough)")
    parser.add_argument("--out-dir", default=Path("wgpu-refusal-output"), type=Path)
    parser.add_argument(
        "--launcher",
        default="",
        help="argv prefix placed before --exe. Exists for scripts/check_wgpu_refusal_selftest.py, "
        "which drives this checker against stub binaries to prove it rejects a broken contract. "
        "Leave empty for a real run.",
    )
    args = parser.parse_args()
    launcher = args.launcher.split() if args.launcher else []

    require(args.exe.is_file(), f"no such executable: {args.exe}")
    require(args.data_dir.is_dir(), f"no such data dir: {args.data_dir}")
    args.out_dir.mkdir(parents=True, exist_ok=True)

    # ---- Arm A: control -------------------------------------------------
    code_a, log_a = run(args.exe, args.data_dir, "dummy", force=True, out_dir=args.out_dir, launcher=launcher)
    require(
        code_a == 0,
        f"control arm (--render dummy, forced) exited {code_a}, expected 0. "
        "The binary or the game data is broken, so nothing below would mean anything.",
    )
    require(
        PROBE_LINE not in log_a,
        "control arm logged the WGR_FORCE_INIT_FAIL probe. The probe must be scoped to the "
        "WGPU factory; if dummy sees it, the forcing mechanism is too broad to test anything.",
    )
    require(
        REFUSAL_LINE not in log_a,
        "control arm logged the WGPU refusal. --render dummy must not go near the WGPU path.",
    )

    # ---- Arm B: the contract --------------------------------------------
    code_b, log_b = run(args.exe, args.data_dir, "wgpu", force=True, out_dir=args.out_dir, launcher=launcher)
    require(
        code_b != 0,
        "explicit --render wgpu could not create WGPU and still exited 0. "
        "A failed explicit backend request must fail the process.",
    )
    require(
        PROBE_LINE in log_b,
        f"forced arm never logged the probe line {PROBE_LINE!r}. Either WGR_FORCE_INIT_FAIL is no "
        "longer read (GraphicsBackendWgpu.cpp) or the message changed; without it this arm's "
        "non-zero exit proves nothing, because a GPU-less runner fails WGPU anyway.",
    )
    require(
        REFUSAL_LINE in log_b,
        f"forced arm never logged the refusal line {REFUSAL_LINE!r}. The process refused, but said "
        "nothing readable about why -- which is the hidden-switch failure gate bullet 7 is about. "
        "(It was exactly this, silently, while the message went through RptF.)",
    )
    for marker, what in (
        (GL33_CREATED, "GL33 was initialized"),
        (WGPU_CREATED, "a WGPU renderer was created"),
        (SILENT_FALLBACK, "the request fell through to Auto"),
    ):
        require(
            marker not in log_b,
            f"explicit --render wgpu refused, but {what} anyway ({marker!r} in the log). "
            "This is a silent fallback and it makes every WGPU smoke test a liar.",
        )

    print(
        "\nWGPU no-silent-fallback contract holds:\n"
        f"  control  --render dummy  exit 0, clean\n"
        f"  forced   --render wgpu   exit {code_b}, probe logged, refusal logged, no renderer created\n"
        "NOTE: this asserts the REFUSAL path. It does not assert that WGPU starts; that needs a GPU.",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Failure as exc:
        print(f"\nFAIL: {exc}", file=sys.stderr)
        sys.exit(1)
