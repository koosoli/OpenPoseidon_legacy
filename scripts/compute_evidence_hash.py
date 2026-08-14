#!/usr/bin/env python3
"""Compute a Preview-0 ticket's evidence hash.

The ledger requires `evidence_hash` on any ticket that reaches VALIDATED or
SHIPPABLE. Until now nothing produced that value and the validator only checked
it was 64 hex characters — so any invented string satisfied it, and the field
attested to nothing.

This derives it from the ticket's own tracked evidence files, so the hash
changes when the evidence changes and a stale or fabricated value is detectable.
`validate_preview0_ledger.py` recomputes it the same way and rejects a mismatch.

    python scripts/compute_evidence_hash.py CORE-NEG-001
    python scripts/compute_evidence_hash.py --all
    python scripts/compute_evidence_hash.py --all --write   # write them back

Content is read through git's filters (as `write_preview0_manifest.py` does), so
the hash is stable across checkouts with different line-ending settings rather
than changing between Windows and Linux.
"""
from __future__ import annotations

import hashlib
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
LEDGER = ROOT / "docs" / "roadmap" / "status-ledger.yaml"
EVIDENCE_LINE = re.compile(r"\s+-\s+((?:docs|engine|apps|scripts|\.github)/\S+)")


def canonical_bytes(rel_path: str) -> bytes:
    """File content as git stores it, independent of checkout line endings."""
    data = (ROOT / rel_path).read_bytes()
    try:
        # -w writes the filtered blob into the object store. Without it `hash-object`
        # only reports the id it WOULD have, and the `cat-file` below then fails with
        # "fatal: bad file" for any evidence file whose current content is not already
        # committed or staged — which is every hash computed while preparing a commit.
        # The except-clause below would then silently fall back to unfiltered bytes and
        # produce a different hash on a CRLF checkout than on an LF one, which is the
        # exact cross-platform difference this function exists to remove.
        blob = subprocess.check_output(
            ["git", "hash-object", "-w", "--path", rel_path, "--filters", "--stdin"],
            cwd=ROOT,
            input=data,
        ).strip()
        return subprocess.check_output(["git", "cat-file", "blob", blob], cwd=ROOT)
    except (OSError, subprocess.CalledProcessError):
        return data


def ticket_blocks(text: str) -> dict[str, str]:
    blocks: dict[str, str] = {}
    current_id, buf = None, []
    for line in text.splitlines():
        if line.startswith("  - id:"):
            if current_id:
                blocks[current_id] = "\n".join(buf)
            current_id, buf = line.split(":", 1)[1].strip(), [line]
        elif current_id is not None:
            buf.append(line)
    if current_id:
        blocks[current_id] = "\n".join(buf)
    return blocks


def evidence_hash(block: str) -> tuple[str, list[str]]:
    """SHA-256 over each evidence file's path and canonical content.

    Sorted so the value does not depend on the order entries happen to appear
    in, and length-prefixed so no concatenation of two files can collide with a
    different pair.
    """
    paths = sorted({m.group(1) for line in block.splitlines() if (m := EVIDENCE_LINE.match(line))})
    digest = hashlib.sha256()
    for rel in paths:
        if not (ROOT / rel).is_file():
            raise SystemExit(f"evidence file missing: {rel}")
        body = canonical_bytes(rel)
        digest.update(f"{rel}\0{len(body)}\0".encode("utf-8"))
        digest.update(body)
    return digest.hexdigest(), paths


def rewrite_ledger(updates: dict[str, str]) -> list[str]:
    """Write recomputed hashes back into the ledger. Returns the tickets changed.

    Exists because doing this by hand is error-prone in a specific way: a `sed`
    substitution with an empty "old" pattern silently prepends the new digest to
    EVERY evidence_hash line, corrupting all six tickets while looking like it
    worked. Splitting on the ticket marker and rewriting one field per block cannot
    do that.
    """
    text = LEDGER.read_text(encoding="utf-8")
    blocks = text.split("  - id: ")
    out = [blocks[0]]
    changed: list[str] = []
    for block in blocks[1:]:
        ticket = block.splitlines()[0].strip()
        wanted = updates.get(ticket)
        # `null` has to match too, not just an existing digest. It did not, and the
        # effect was the opposite of the intent: a ticket promoted to VALIDATED with
        # the field still null was skipped by every sweep, so the one state that
        # needs a hash was the one state that never got one. Seven records reached
        # VALIDATED that way before the validator -- which fails on the first bad
        # ticket -- ever got far enough down the file to notice.
        current = re.search(r"    evidence_hash: ([0-9a-f]{64}|null)", block)
        if wanted and current and current.group(1) != wanted:
            changed.append(ticket)
            block = re.sub(
                r"    evidence_hash: (?:[0-9a-f]{64}|null)",
                "    evidence_hash: " + wanted,
                block,
                count=1,
            )
        out.append(block)
    if changed:
        LEDGER.write_text("  - id: ".join(out), encoding="utf-8", newline="")
    return changed


def main(argv: list[str]) -> int:
    if not argv or argv[0] in {"-h", "--help"}:
        print(__doc__)
        return 0
    write = "--write" in argv
    argv = [a for a in argv if a != "--write"]
    if not argv:
        print("--write needs a ticket id or --all", file=sys.stderr)
        return 1
    blocks = ticket_blocks(LEDGER.read_text(encoding="utf-8"))
    sweep = argv[0] == "--all"
    wanted = sorted(blocks) if sweep else [argv[0]]
    computed: dict[str, str] = {}
    for ticket in wanted:
        if ticket not in blocks:
            print(f"unknown ticket: {ticket}", file=sys.stderr)
            return 1
        value, paths = evidence_hash(blocks[ticket])
        if not paths:
            # Fatal when a specific ticket was asked for -- the caller expected a
            # hash and there is nothing to derive one from. Not fatal during a
            # sweep: an OPEN ticket legitimately has no evidence yet, and the
            # ledger only requires evidence_hash once a ticket reaches VALIDATED
            # or SHIPPABLE. Before this, adding one unstarted ticket made --all
            # exit non-zero before writing anything, so the recompute the
            # validator tells you to run silently did nothing at all.
            message = f"{ticket}: no tracked evidence files — nothing to hash"
            if not sweep:
                print(message, file=sys.stderr)
                return 1
            print(f"{message} (skipped)")
            continue
        computed[ticket] = value
        print(f"{ticket}  {value}  ({len(paths)} file(s))")
    if write:
        changed = rewrite_ledger(computed)
        print(f"ledger updated: {', '.join(changed) if changed else 'no change'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
