#!/usr/bin/env python3
"""AST-015 evidence: resolve every real model dependency through the canonical form.

    python scripts/build_reference_fixture_index.py --full full.json
    python scripts/verify_virtual_path_resolution.py full.json

Unit tests prove the rules behave as written; they cannot show the rules are the
right ones. That only comes from pointing them at a real corpus and counting what
fails to resolve.

Builds an addon namespace from every PBO prefix AST-007 recorded, plus the loose
`.../CA/...` source trees which carry the same `ca\\...` namespace without any
archive being read. Then takes every material and texture reference from every
indexed model and asks whether its canonical form finds a target.

Mirrors `engine/Poseidon/Asset/Addon/VirtualPath.hpp`. Results are reported per
prefix, never as one percentage: "96% resolve" hides which namespace is missing,
and a compatibility claim has to name its denominator.
"""
from __future__ import annotations

import argparse
import collections
import json
import pathlib
import re
import sys

IMAGE_SOURCE_EXTENSIONS = (".tga", ".tif", ".tiff", ".png")


def canonical(raw: str) -> str:
    """The VirtualPath::Parse rules, kept deliberately in step with the header."""
    text = raw.replace("/", "\\").lower().lstrip("\\")
    while "\\\\" in text:
        text = text.replace("\\\\", "\\")
    return text.rstrip("\\")


def shipped(path: str) -> str:
    for extension in IMAGE_SOURCE_EXTENSIONS:
        if path.endswith(extension):
            return path[: -len(extension)] + ".paa"
    return path


def owner(namespace: dict[str, str], path: str) -> str | None:
    """Longest declared prefix owning this path, matching AddonNamespace::Owner."""
    at = len(path)
    while at > 0:
        found = namespace.get(path[:at])
        if found:
            return found
        at = path.rfind("\\", 0, at)
        if at < 0:
            break
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("full_index", type=pathlib.Path)
    args = parser.parse_args()
    if not args.full_index.is_file():
        sys.exit(f"missing {args.full_index}; generate it with --full")
    index = json.loads(args.full_index.read_text(encoding="utf-8"))

    namespace: dict[str, str] = {}
    collisions: list[str] = []
    loose: set[str] = set()

    for root in index["roots"]:
        for fixture in root["fixtures"]:
            if fixture["kind"] == "archive":
                prefix = canonical(fixture["detail"].get("prefix") or "")
                if not prefix:
                    continue
                source = f"{root['alias']}:{fixture['path']}"
                if prefix in namespace and namespace[prefix] != source:
                    collisions.append(f"{prefix}: {namespace[prefix]} vs {source}")
                    continue
                namespace[prefix] = source
            # Everything under a `CA` directory is addressed as `ca\...`; this is
            # how the Arma 2 sample models satisfy their own dependencies.
            match = re.search(r"(?:^|/)CA/(.+)$", fixture["path"], re.IGNORECASE)
            if match:
                loose.add(canonical("ca\\" + match.group(1)))

    print(f"addon prefixes declared by PBOs : {len(namespace)}")
    print(f"prefix collisions               : {len(collisions)}")
    for line in collisions[:5]:
        print(f"    {line}")
    print(f"loose virtual paths             : {len(loose)}")

    total = 0
    resolved = collections.Counter()
    unresolved: collections.Counter[str] = collections.Counter()
    examples: dict[str, str] = {}

    for root in index["roots"]:
        for fixture in root["fixtures"]:
            if fixture["kind"] != "model":
                continue
            references = fixture["detail"].get("references") or {}
            for raw in references.get("materials", []) + references.get("textures", []):
                if not raw or raw.startswith("#"):
                    continue  # procedural textures such as #(argb,8,8,3)color(...)
                total += 1
                base = canonical(raw)
                target = shipped(base)
                if base in loose or target in loose:
                    resolved["loose"] += 1
                elif owner(namespace, target):
                    resolved["archive"] += 1
                else:
                    prefix = target.split("\\", 1)[0]
                    unresolved[prefix] += 1
                    examples.setdefault(prefix, raw)

    print()
    print(f"model dependency references checked : {total}")
    print(f"  resolved in a loose source tree   : {resolved['loose']}")
    print(f"  resolved in an addon archive      : {resolved['archive']}")
    print(f"  unresolved                        : {sum(unresolved.values())}")
    for prefix, count in unresolved.most_common(10):
        print(f"      {prefix or '<no prefix>':14} {count:6}   e.g. {examples[prefix]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
