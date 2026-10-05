"""Private, pinned source inventory/export. Never pushes or changes visibility.

Inventory is a proposal, not approval. Draft exports are local and may contain
unresolved findings; final export requires every finding/review to be resolved.
Reports stay outside the export. Reads Git blobs, never the working directory.
"""

import argparse
import collections
import csv
import hashlib
import json
import re
import subprocess
from pathlib import Path, PurePosixPath


def digest(data):
    return hashlib.sha256(data).hexdigest()


def run(repo, *args):
    return subprocess.check_output(["git", "-C", str(repo), *args])


def safe_path(value):
    p = PurePosixPath(value)
    if not value or p.is_absolute() or p.as_posix() != value or any(x.casefold() in ("..", ".git", "") for x in p.parts):
        raise ValueError(f"Unsafe export path: {value}")
    if "\\" in value or ":" in value:
        raise ValueError(f"Non-portable export path: {value}")
    for part in p.parts:
        if part.endswith((".", " ")) or re.fullmatch(r"(?i)(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?", part):
            raise ValueError(f"Windows-unsafe export path: {value}")
    return p


def replacement_bytes(policy_path, value):
    relative = safe_path(value)
    base = policy_path.parent.resolve()
    resolved = (base / relative).resolve()
    if base not in resolved.parents:
        raise ValueError("Replacement escapes policy directory")
    return resolved.read_bytes()


def classify(path, policy):
    if path in policy["rewrites"]:
        return "INCLUDE_REWRITTEN", "curated-public-document", "Public replacement; original remains private"
    if path in policy["include_files"]:
        return "INCLUDE_UNCHANGED", "explicit-file", "Build, license or public project configuration"
    for prefix in policy["exclude_prefixes"]:
        if path.startswith(prefix):
            return "EXCLUDE_INTERNAL", "internal-prefix", "Private documentation, agent state, evidence or experiments"
    if path in policy["exclude_files"]:
        return "EXCLUDE_INTERNAL", "internal-file", "Private development instructions or process record"
    if any(path.startswith(p) for p in policy["include_prefixes"]):
        return "INCLUDE_UNCHANGED", "source-candidate", "Required source/tests/tools; content review remains explicit"
    return "REVIEW_REQUIRED", "unclassified", "No approved path rule; do not silently discard"


PATTERNS = {
    "private-repository": rb"(?i)PoseidonWGPU-private-safety-mirror|CWR-CE-paavohuhtalafork",
    "personal-build-path": rb"(?i)[A-Z]:[\\/]+Users[\\/]+(?:mail|olik)[\\/]|F:[\\/]ofpisnotdead\.com[\\/]sources",
    "agent-or-private-document-reference": rb"(?i)\.claude[\\/]|\.codex[\\/]|docs[\\/]roadmap|AGENTS\.md|CLAUDE\.md",
    "credential-shaped-value": rb"(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,}|AKIA[A-Z0-9]{16}|-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----)",
    "conflict-marker": rb"(?m)^(?:<<<<<<< |>>>>>>> |=======$)",
    "lfs-pointer": rb"\Aversion https://git-lfs.github.com/spec/v1",
}


def scan(data):
    # Operates on bytes: binary savegames and archives can also carry paths.
    return [{"kind": name, "line": data[:m.start()].count(b"\n") + 1}
            for name, pattern in PATTERNS.items() for m in re.finditer(pattern, data)]


def blobs(repo, entries):
    p = subprocess.Popen(["git", "-C", str(repo), "cat-file", "--batch"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    try:
        for entry in entries:
            p.stdin.write((entry["source_blob"] + "\n").encode())
            p.stdin.flush()
            header = p.stdout.readline().split()
            if len(header) != 3 or header[1] != b"blob":
                raise ValueError("Missing Git blob or unsupported gitlink")
            data = p.stdout.read(int(header[2]))
            if p.stdout.read(1) != b"\n":
                raise ValueError("Invalid Git batch response")
            yield entry, data
    finally:
        p.stdin.close()
        p.stdout.close()
        p.wait()


def inventory(repo, commit, policy_path, reports):
    sha = run(repo, "rev-parse", "--verify", commit + "^{commit}").decode().strip()
    policy_data = policy_path.read_bytes()
    policy = json.loads(policy_data)
    entries = []
    names = set()
    for record in run(repo, "ls-tree", "-rz", "--long", sha).split(b"\0"):
        if not record:
            continue
        meta, raw_name = record.split(b"\t", 1)
        mode, kind, blob, size = meta.decode().split()
        path = raw_name.decode("utf-8")
        safe_path(path)
        if path.casefold() in names:
            raise ValueError(f"Case-colliding path: {path}")
        names.add(path.casefold())
        cls, rule, reason = classify(path, policy)
        entries.append(dict(source_path=path, source_blob=blob, git_mode=mode,
                            size=int(size) if size != "-" else 0, classification=cls,
                            destination_path=path if cls.startswith("INCLUDE") else None,
                            rule_id=rule, reason=reason, review_state="PENDING",
                            license_review="PENDING", build_dependency="NOT_YET_VALIDATED"))
        if kind != "blob" or mode not in ("100644", "100755"):
            raise ValueError(f"Submodule/symlink requires separate review: {path}")
    additions = []
    for e, data in blobs(repo, entries):
        e["source_sha256"] = digest(data)
        e["findings"] = []
        if e["classification"].startswith("INCLUDE"):
            if e["classification"] == "INCLUDE_REWRITTEN":
                data = replacement_bytes(policy_path, policy["rewrites"][e["source_path"]])
            e["export_sha256"] = digest(data)
            e["findings"] = scan(data)
            if e["source_path"] in policy["license_holds"]:
                e["findings"].append({"kind": "license-source-chain-hold", "line": 0})
                e["license_review"] = "BLOCKED"
            if e["source_path"].startswith(("assets/", "resources/", "tests/fixtures/")):
                e["license_review"] = "INDIVIDUAL_RESOURCE_REVIEW_REQUIRED"
    for dest, replacement in policy.get("add_files", {}).items():
        safe_path(dest)
        if dest.casefold() in names:
            raise ValueError(f"Added path collides: {dest}")
        names.add(dest.casefold())
        data = replacement_bytes(policy_path, replacement)
        additions.append(dict(source_path=None, source_blob=None, source_sha256=None,
                              git_mode="100644", size=len(data), classification="INCLUDE_REWRITTEN",
                              destination_path=dest, export_sha256=digest(data), rule_id="public-addition",
                              reason="New curated public documentation", findings=scan(data),
                              review_state="PENDING", license_review="PENDING", build_dependency="NOT_YET_VALIDATED"))
    manifest = {"schema": 1, "source_commit": sha,
                "source_tree": run(repo, "rev-parse", sha + "^{tree}").decode().strip(),
                "policy_sha256": digest(policy_data), "entries": entries + additions,
                "publication_state": "PRIVATE_DRAFT_NOT_APPROVED"}
    reports.mkdir(parents=True, exist_ok=False)
    manifest_bytes = (json.dumps(manifest, indent=2, ensure_ascii=False) + "\n").encode()
    (reports / "inventory.json").write_bytes(manifest_bytes)
    (reports / "inventory.sha256").write_text(digest(manifest_bytes) + "\n", encoding="utf-8")
    with (reports / "exclusions.csv").open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["source_path", "classification", "rule_id", "reason"])
        w.writeheader()
        for e in entries:
            if not e["classification"].startswith("INCLUDE"):
                w.writerow({k: e[k] for k in w.fieldnames})
    summary = {"source_commit": sha, "tracked_files": len(entries),
               "classifications": dict(collections.Counter(e["classification"] for e in entries)),
               "included_files_with_findings": sum(bool(e["findings"]) for e in entries + additions),
               "finding_types": dict(collections.Counter(f["kind"] for e in entries + additions for f in e["findings"])),
               "final_export_ready": False}
    (reports / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return summary


def export(repo, manifest_path, policy_path, destination, draft):
    manifest = json.loads(manifest_path.read_bytes())
    policy_bytes = policy_path.read_bytes()
    policy = json.loads(policy_bytes)
    if digest(policy_bytes) != manifest["policy_sha256"]:
        raise ValueError("Policy changed after inventory")
    if destination.exists():
        raise ValueError("Destination must be new; existing files will never be overwritten")
    destination = destination.resolve()
    if destination == repo.resolve() or repo.resolve() in destination.parents:
        raise ValueError("Export must be outside the private worktree")
    if destination == manifest_path.parent.resolve() or manifest_path.parent.resolve() in destination.parents:
        raise ValueError("Private reports must not be inside the export")
    entries = manifest["entries"]
    if not draft and any(e["classification"] == "REVIEW_REQUIRED" or e["review_state"] != "REVIEWED" or
                         (e["classification"].startswith("INCLUDE") and
                          (e["findings"] or e["license_review"] != "REVIEWED"))
                         for e in entries):
        raise ValueError("Unresolved findings or reviews: final export blocked")
    outputs = {}
    for e in entries:
        if e["classification"].startswith("INCLUDE"):
            name = str(safe_path(e["destination_path"]))
            if name.casefold() in outputs:
                raise ValueError("Duplicate output path")
            outputs[name.casefold()] = e
    destination.mkdir(parents=True)
    for e, source in blobs(repo, [e for e in entries if e["source_blob"]]):
        if digest(source) != e["source_sha256"]:
            raise ValueError("Source blob differs from pinned inventory")
        if not e["classification"].startswith("INCLUDE"):
            continue
        data = source
        if e["classification"] == "INCLUDE_REWRITTEN":
            data = replacement_bytes(policy_path, policy["rewrites"][e["source_path"]])
        write_file(destination, e, data)
    for e in entries:
        if e["source_path"] is None:
            data = replacement_bytes(policy_path, policy["add_files"][e["destination_path"]])
            write_file(destination, e, data)
    verify(manifest_path, destination)
    return {"state": "PRIVATE_DRAFT_NOT_APPROVED" if draft else "EXPORT_VERIFIED_NOT_PUBLISHED",
            "destination": str(destination), "files": len(outputs)}


def write_file(destination, entry, data):
    if digest(data) != entry["export_sha256"]:
        raise ValueError("Replacement differs from manifest")
    p = destination / safe_path(entry["destination_path"])
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data)
    if entry["git_mode"] == "100755":
        p.chmod(0o755)


def verify(manifest_path, destination):
    entries = json.loads(manifest_path.read_bytes())["entries"]
    expected = {e["destination_path"]: e["export_sha256"] for e in entries if e["classification"].startswith("INCLUDE")}
    actual = {}
    for p in destination.rglob("*"):
        if p.is_symlink():
            raise ValueError("Symlink in export")
        if p.name.casefold() == ".git":
            raise ValueError("Unexpected Git metadata in export file set/hashes")
        if p.is_file():
            actual[p.relative_to(destination).as_posix()] = digest(p.read_bytes())
    if actual != expected:
        raise ValueError("Export file set/hashes differ from manifest (including unexpected Git metadata)")
    return {"verified_files": len(actual)}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("mode", choices=("inventory", "draft", "export", "verify"))
    ap.add_argument("--repo", type=Path, required=True)
    ap.add_argument("--policy", type=Path)
    ap.add_argument("--commit")
    ap.add_argument("--reports", type=Path)
    ap.add_argument("--manifest", type=Path)
    ap.add_argument("--destination", type=Path)
    args = ap.parse_args()
    if args.mode == "inventory":
        if not all((args.commit, args.policy, args.reports)):
            ap.error("inventory needs --commit, --policy, --reports")
        result = inventory(args.repo, args.commit, args.policy, args.reports)
    elif args.mode == "verify":
        result = verify(args.manifest, args.destination)
    else:
        result = export(args.repo, args.manifest, args.policy, args.destination, args.mode == "draft")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
