#!/usr/bin/env python3
"""Merge an overlay's ledger seed into the shared execution ledger.

    python scripts/merge_ledger_seed.py <seed.yaml> --overlay <OVERLAY-ID> [--write]

The seed files ship at `schema_version: 1` while the live ledger is at 3, so a
literal paste produces records the validator rejects for missing
`reviewer_method`, `supersedes` and `evidence`.  This fills those in as empty,
stamps the owning overlay, resolves `baseline_commit` from the checkout, and
re-emits lists in flow style -- the block style the seeds use reads as an empty
value through the validator's line-oriented field reader, which would silently
skip every dependency check.

Merge is additive by ticket ID: an ID already present in the ledger is reported
and left exactly as it is, so a live claim can never be overwritten by a seed.
No external YAML dependency, matching the other ledger scripts.
"""
from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
LEDGER = ROOT / "docs" / "roadmap" / "status-ledger.yaml"

# Emitted in this order so merged records read like the ones already in the file.
FIELD_ORDER = (
    "overlay", "is_example", "title", "class", "obligation", "execution_mode",
    "scheduling_state", "status", "outcome", "owner", "branch", "baseline_commit",
    "verification_commit", "milestone", "milestone_role", "depends_on",
    "blocked_by", "blocks", "implementation_commits", "production_call_sites",
    "test_ids", "benchmark_artifacts", "evidence_hash", "decision_record",
    "known_failures", "fallback", "reviewer", "reviewer_method", "supersedes",
    "evidence",
)
LIST_FIELDS = {
    "depends_on", "blocked_by", "blocks", "implementation_commits",
    "production_call_sites", "test_ids", "benchmark_artifacts",
    "known_failures", "supersedes", "evidence",
}
# Schema-3 fields the schema-1 seeds do not carry.
DEFAULTS = {"reviewer_method": "null", "supersedes": "[]", "evidence": "[]"}


def parse_seed(text: str) -> list[dict[str, object]]:
    """Parse the flat `tickets:` list of a seed file."""
    records: list[dict[str, object]] = []
    current: dict[str, object] | None = None
    field: str | None = None
    in_tickets = False
    for line in text.splitlines():
        if line.rstrip() == "tickets:":
            in_tickets = True
            continue
        if not in_tickets or not line.strip():
            continue
        if line.startswith("- id: "):
            current = {"id": line.removeprefix("- id: ").strip()}
            records.append(current)
            field = None
        elif current is None:
            continue
        elif line.startswith("  - ") and field in LIST_FIELDS:
            current.setdefault(field, [])  # type: ignore[arg-type]
            current[field].append(line.removeprefix("  - ").strip())  # type: ignore[union-attr]
        elif line.startswith("  ") and ": " in line or line.rstrip().endswith(":"):
            key, _, raw = line.strip().partition(":")
            field = key.strip()
            raw = raw.strip()
            current[field] = [] if (field in LIST_FIELDS and raw in ("", "[]")) else raw
    return records


def render(record: dict[str, object]) -> list[str]:
    lines = [f"  - id: {record['id']}"]
    for field in FIELD_ORDER:
        raw = record.get(field, DEFAULTS.get(field, "null"))
        if isinstance(raw, list):
            raw = "[" + ", ".join(raw) + "]"
        lines.append(f"    {field}: {raw}")
    return lines


def ledger_ids(text: str) -> set[str]:
    return {
        line.strip().removeprefix("- id: ")
        for line in text.splitlines()
        if line.strip().startswith("- id: ")
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("seed", type=pathlib.Path)
    parser.add_argument("--overlay", required=True, help="overlay_id that will own the merged records")
    parser.add_argument("--baseline", help="baseline commit (default: git rev-parse HEAD)")
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()

    baseline = args.baseline or subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
    ).strip()

    seed_text = args.seed.read_text(encoding="utf-8")
    if "MERGE_ONLY" not in seed_text:
        print(f"refusing: {args.seed.name} does not declare a MERGE_ONLY policy", file=sys.stderr)
        return 1

    with LEDGER.open("r", encoding="utf-8", newline="") as handle:
        raw_ledger = handle.read()
    newline = "\r\n" if "\r\n" in raw_ledger else "\n"
    ledger_text = raw_ledger.replace("\r\n", "\n")
    existing = ledger_ids(ledger_text)

    added, skipped, new_lines = [], [], []
    for record in parse_seed(seed_text):
        ticket = record["id"]
        if ticket in existing:
            skipped.append(ticket)
            continue
        record["overlay"] = args.overlay
        if record.get("baseline_commit") in (None, "null", ""):
            record["baseline_commit"] = baseline
        added.append(ticket)
        new_lines.extend(render(record))

    for ticket in skipped:
        print(f"skip (already live): {ticket}")
    for ticket in added:
        print(f"merge: {ticket}")

    if not added:
        print("nothing to merge")
        return 0
    if not args.write:
        print(f"\n{len(added)} record(s) would be added; re-run with --write")
        return 0

    merged = ledger_text.rstrip("\n") + "\n" + "\n".join(new_lines) + "\n"
    with LEDGER.open("w", encoding="utf-8", newline="") as handle:
        handle.write(merged.replace("\n", newline))
    print(f"\nledger updated: {len(added)} record(s) added, baseline {baseline[:7]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
