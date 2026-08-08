#!/usr/bin/env python3
"""Validate the ASSET-010 third-party source registry.

A registry nothing checks is a comment. ASSET-010 requires CI to reject
unregistered derived assets, so this does three things:

1. Every entry carries the full field set. A missing licence_url or source_url is
   the difference between provenance and a note about provenance.
2. Where the fetched source files are present on disk, their sha256 and byte
   length must match what the registry records. This is what makes the entry a
   claim about specific bytes rather than about a URL that may have changed
   content since.
3. Exactly one source per intended use may be SELECTED. GRS-GATE-1 requires one
   path chosen after comparing candidates; two selected sources means nobody
   chose.

Source files themselves are NOT required to be present -- they live under the
gitignored assets/ tree, so CI will not have them. The hash check runs when it
can and is skipped, loudly, when it cannot.

    python scripts/validate_asset_registry.py
    python scripts/validate_asset_registry.py --require-files   # fail if absent

Parsed line-wise, matching the other ledger validators, so it runs on a bare
checkout with no third-party packages.
"""

from __future__ import annotations

import hashlib
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
REGISTRY = ROOT / "docs" / "assets" / "source-registry.yaml"
SOURCE_ROOT = ROOT / "assets" / "sources"

REQUIRED_FIELDS = (
    "id",
    "source_url",
    "provider_url",
    "product_name",
    "provider",
    "author",
    "licence",
    "licence_url",
    "licence_quote",
    "downloaded_utc",
    "intended_use",
    "selection",
)


def parse_sources(text: str, block_name: str) -> list[dict]:
    """Split a source-list block into per-entry dicts with a `files` list."""
    lines = text.splitlines()
    try:
        start = next(i for i, line in enumerate(lines) if line.rstrip() == f"{block_name}:") + 1
    except StopIteration:
        return []

    entries: list[dict] = []
    current: dict | None = None
    for line in lines[start:]:
        if line and not line.startswith(" "):
            break  # next top-level key ends the block
        entry_start = re.match(r"  - id: (\S+)", line)
        if entry_start:
            if current:
                entries.append(current)
            current = {"id": entry_start.group(1), "files": [], "_fields": {"id"}}
            continue
        if current is None:
            continue
        field = re.match(r"    ([a-z_]+):\s*(.*)$", line)
        if field:
            key, value = field.group(1), field.group(2).strip()
            current["_fields"].add(key)
            if value not in {">-", ">", "|-", "|", ""}:
                current[key] = value
            continue
        name = re.match(r"      - name: (\S+)", line)
        if name:
            current["files"].append({"name": name.group(1)})
            continue
        detail = re.match(r"        (bytes|sha256): (\S+)", line)
        if detail and current["files"]:
            current["files"][-1][detail.group(1)] = detail.group(2)
            continue
        # Folded-scalar continuation lines carry no fields; ignore them.
    if current:
        entries.append(current)
    return entries


def main(argv: list[str]) -> int:
    require_files = "--require-files" in argv
    if not REGISTRY.is_file():
        print(f"missing registry: {REGISTRY.relative_to(ROOT)}", file=sys.stderr)
        return 1

    problems: list[str] = []
    text = REGISTRY.read_text(encoding="utf-8")
    entries = parse_sources(text, "sources") + parse_sources(text, "additional_sources")
    if not entries:
        print("registry lists no sources", file=sys.stderr)
        return 1

    seen: set[str] = set()
    selected: list[str] = []
    checked = skipped = 0

    for entry in entries:
        ident = entry["id"]
        if ident in seen:
            problems.append(f"{ident}: duplicate id")
        seen.add(ident)

        for field in REQUIRED_FIELDS:
            if field not in entry["_fields"]:
                problems.append(f"{ident}: missing {field}")

        selection = entry.get("selection", "")
        # Folded scalars put the value on following lines, so read the verdict from
        # the raw block instead of the one-line value.
        block = REGISTRY.read_text(encoding="utf-8").split(f"  - id: {ident}", 1)[1]
        block = block.split("\n  - id:", 1)[0]
        verdicts = [v for v in ("SELECTED", "REJECTED", "EVALUATING") if re.search(rf"\b{v}\b", block)]
        if not verdicts:
            problems.append(f"{ident}: selection must state SELECTED, REJECTED or EVALUATING")
        elif verdicts[0] == "SELECTED":
            selected.append(ident)

        if not entry["files"]:
            problems.append(f"{ident}: no files recorded")
        for record in entry["files"]:
            if "sha256" not in record or "bytes" not in record:
                problems.append(f"{ident}: {record['name']} needs both bytes and sha256")
                continue
            if not re.fullmatch(r"[0-9a-f]{64}", record["sha256"]):
                problems.append(f"{ident}: {record['name']} has a malformed sha256")
                continue
            path = SOURCE_ROOT / entry.get("product_name", "") / record["name"]
            local_path = entry.get("local_path")
            if local_path:
                path = ROOT / local_path / record["name"]
            if not path.is_file():
                skipped += 1
                if require_files:
                    problems.append(f"{ident}: missing source file {path.relative_to(ROOT)}")
                continue
            data = path.read_bytes()
            checked += 1
            if len(data) != int(record["bytes"]):
                problems.append(
                    f"{ident}: {record['name']} is {len(data)} bytes, registry says {record['bytes']}"
                )
            if hashlib.sha256(data).hexdigest() != record["sha256"]:
                problems.append(f"{ident}: {record['name']} sha256 does not match the registry")

    if len(selected) > 1:
        problems.append(f"more than one SELECTED source: {', '.join(selected)}")

    if problems:
        print("asset registry invalid:", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        return 1

    note = f"{checked} file(s) hash-verified"
    if skipped:
        note += f", {skipped} not present locally (skipped)"
    print(f"asset registry valid: {len(entries)} source(s), {note}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
