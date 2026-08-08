#!/usr/bin/env python3
"""Validate the Preview-0 overlay and the tickets it owns in the shared ledger.

The generic checks moved to `validate_execution_ledger.py` when a second overlay
(COMPAT-C1) began claiming tickets in the same ledger file.  What stays here is
Preview-0's own evidence contract: the packaged capture manifest and the 4.4.1
checksum manifest, neither of which any other overlay has.
"""
from __future__ import annotations

import hashlib
import json
import pathlib
import re
import sys


sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from validate_execution_ledger import (  # noqa: E402
    ROOT,
    canonical_bytes,
    git,
    git_bytes,
    text,
    tracked_file,
    validate,
)

OVERLAY = ROOT / "docs/roadmap/modernisation/PoseidonWGPU_Execution_Overlay_Preview0_v1.2.1.yaml"
MANIFEST = ROOT / "docs/roadmap/evidence/preview0-manifest.json"
CHECKSUMS = ROOT / "docs/roadmap/modernisation/PoseidonWGPU_Roadmap_4.4.1_CHECKSUMS_Overlay_v1.2.1.txt"


def verify_clean_manifest() -> None:
    """Verify the tracked Preview-0 evidence bundle was made from a clean tree."""
    tracked_file(str(MANIFEST.relative_to(ROOT)), "Preview-0 manifest")
    try:
        manifest = json.loads(text(MANIFEST))
    except json.JSONDecodeError as exc:
        raise ValueError(f"invalid Preview-0 manifest JSON: {exc}") from exc
    if manifest.get("git_dirty") is not False:
        raise ValueError("Preview-0 manifest must record git_dirty: false")
    commit = manifest.get("git_commit")
    if not isinstance(commit, str) or not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("Preview-0 manifest must record a full git commit")
    git("cat-file", "-e", f"{commit}^{{commit}}")
    git("merge-base", "--is-ancestor", commit, "HEAD")

    for section, required_keys in (
        ("runtime_check", ("log", "sha256")),
        ("capture", ("path", "sha256")),
        ("metrics", ("path", "sha256")),
    ):
        entry = manifest.get(section)
        if not isinstance(entry, dict) or not all(key in entry for key in required_keys):
            raise ValueError(f"Preview-0 manifest missing {section} evidence")
        raw_path = entry.get("path", entry.get("log"))
        digest = entry["sha256"]
        if not isinstance(raw_path, str) or not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ValueError(f"Preview-0 manifest has invalid {section} evidence metadata")
        repository_path = raw_path.replace("\\", "/")
        tracked_file(repository_path, f"Preview-0 manifest {section}")
        evidence_bytes = git_bytes("show", f"{commit}:{repository_path}")
        if hashlib.sha256(evidence_bytes).hexdigest() != digest:
            raise ValueError(f"Preview-0 manifest {section} evidence hash mismatch")

    shaders = manifest.get("shaders")
    if not isinstance(shaders, list) or not shaders:
        raise ValueError("Preview-0 manifest must list shader hashes")
    for shader in shaders:
        if not isinstance(shader, dict):
            raise ValueError("Preview-0 manifest has malformed shader metadata")
        raw_path, digest = shader.get("path"), shader.get("sha256")
        if not isinstance(raw_path, str) or not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ValueError("Preview-0 manifest has invalid shader hash")
        shader_path = raw_path.replace("\\", "/")
        # A clean capture may intentionally describe an earlier, pinned commit;
        # validate against that commit rather than the mutable checkout HEAD.
        shader_bytes = git_bytes("show", f"{commit}:{shader_path}")
        if hashlib.sha256(shader_bytes).hexdigest() != digest:
            raise ValueError(f"Preview-0 manifest shader hash mismatch: {raw_path}")


def verify_canonical_checksums(roadmap: pathlib.Path, overlay: pathlib.Path) -> None:
    tracked_file(str(CHECKSUMS.relative_to(ROOT)), "Preview-0 checksum manifest")
    expected_lines = {
        f"{hashlib.sha256(canonical_bytes(roadmap)).hexdigest()}  {roadmap.name}",
        f"{hashlib.sha256(canonical_bytes(overlay)).hexdigest()}  {overlay.name}",
    }
    recorded_lines = set(text(CHECKSUMS).splitlines())
    missing = expected_lines - recorded_lines
    if missing:
        raise ValueError(f"Preview-0 checksum manifest mismatch: {sorted(missing)[0]}")


def preview0_evidence(roadmap: pathlib.Path, overlay: pathlib.Path) -> None:
    verify_canonical_checksums(roadmap, overlay)
    verify_clean_manifest()


def main() -> int:
    return validate(OVERLAY, extra_checks=preview0_evidence)


if __name__ == "__main__":
    raise SystemExit(main())
