#!/usr/bin/env python3
"""Validate the COMPAT-C1 overlay and the tickets it owns in the shared ledger.

Shared checks live in `validate_execution_ledger.py`. What is specific to this
overlay is the pre-activation invariant it inherited from its agent prompt: the
historical Preview-0 overlay must still exist at its canonical tracked path, and
an untracked copy under `old/` does not satisfy it.
"""
from __future__ import annotations

import pathlib
import sys


sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from validate_execution_ledger import ROOT, tracked_file, validate  # noqa: E402

OVERLAY = ROOT / "docs/roadmap/modernisation/PoseidonWGPU_Execution_Overlay_CompatC1_v0.3.yaml"
PREVIEW0_OVERLAY = "docs/roadmap/modernisation/PoseidonWGPU_Execution_Overlay_Preview0_v1.2.1.yaml"


def preview0_invariant(roadmap: pathlib.Path, overlay: pathlib.Path) -> None:
    tracked_file(PREVIEW0_OVERLAY, "Preview-0 historical overlay")


def main() -> int:
    return validate(OVERLAY, extra_checks=preview0_invariant)


if __name__ == "__main__":
    raise SystemExit(main())
