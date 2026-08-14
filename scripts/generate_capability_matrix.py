#!/usr/bin/env python3
"""Generate the Preview-0 public capability matrix from the ledgers.

REL-000 requires a "works / partial / unavailable / experimental" matrix
*derived from validated ledger records*. The derivation is the requirement, not
a formatting preference: a hand-written capability table is the exact artifact
RND-030 found had drifted, where a plan was recorded as shipped on the strength
of a symbol that was evidence against it. A public release note is the worst
possible place to repeat that, because outside readers cannot check it.

So this reads the two ledgers and writes the matrix. `--check` re-generates and
diffs without writing, which is what CI runs, so the published table cannot
drift from the ledger it claims to summarise.

    python scripts/generate_capability_matrix.py            # write
    python scripts/generate_capability_matrix.py --check    # verify, exit 1 on drift

Status mapping, stated here rather than buried in the output:

    SHIPPED           -> Works          present, complete against its plan, on by default
    PARTIAL           -> Partial        present, with a stated gap
    SHIPPED_DISABLED  -> Experimental   complete but off by default; opt in
    (unbuilt)         -> Unavailable    no implementation

Deliberately parsed line-wise rather than with PyYAML, matching
validate_preview0_ledger.py and validate_renderer_ledger.py, so it runs on a
bare checkout with no third-party packages.
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SYSTEMS = ROOT / "docs" / "roadmap" / "renderer-systems-ledger.yaml"
TICKETS = ROOT / "docs" / "roadmap" / "status-ledger.yaml"
OUTPUT = ROOT / "docs" / "release" / "preview0" / "capability-matrix.md"
PREVIEW0_OVERLAY = "PREVIEW0-EXEC-001"

STATUS_LABEL = {
    "SHIPPED": "Works",
    "PARTIAL": "Partial",
    "SHIPPED_DISABLED": "Experimental",
    "UNAVAILABLE": "Unavailable",
}

# A period ends a sentence only when a capital follows and a digit does not
# precede it -- otherwise "0.518 ms" and "v1.2" split mid-measurement, which is
# how a summary ends up quoting half a number.
SENTENCE_END = re.compile(r"(?<![0-9])\.(?=\s+[A-Z])")


def read(path: pathlib.Path) -> str:
    if not path.is_file():
        raise SystemExit(f"missing: {path.relative_to(ROOT)}")
    return path.read_text(encoding="utf-8")


def records(text: str, marker: str = "  - id: ") -> list[dict[str, str]]:
    """Split a ledger into per-entry field maps.

    Folded scalars (`key: >-`) are joined into one string; everything else is
    taken verbatim. Nested lists are ignored -- nothing here needs them, and
    guessing at them would invent structure the callers do not check.
    """
    out: list[dict[str, str]] = []
    current: dict[str, str] | None = None
    folding: str | None = None
    buf: list[str] = []

    def flush() -> None:
        nonlocal folding, buf
        if current is not None and folding is not None:
            current[folding] = " ".join(buf).strip()
        folding, buf = None, []

    for line in text.splitlines():
        if line.startswith(marker):
            flush()
            if current:
                out.append(current)
            current = {"id": line[len(marker):].strip()}
            continue
        if current is None:
            continue
        field = re.match(r"    ([a-z_]+): ?(.*)$", line)
        if field:
            flush()
            key, value = field.group(1), field.group(2).strip()
            if value in {">-", ">", "|-", "|"}:
                folding = key
            else:
                current[key] = value
            continue
        if folding is not None and (line.startswith("      ") or not line.strip()):
            if line.strip():
                buf.append(line.strip())
            continue
        if line and not line.startswith(" "):
            # A new top-level key ends the last record.
            flush()
            if current:
                out.append(current)
                current = None
    flush()
    if current:
        out.append(current)
    return out


def first_sentence(text: str, limit: int = 240) -> str:
    text = text.strip()
    if not text:
        return ""
    match = SENTENCE_END.search(text)
    sentence = text[: match.start() + 1] if match else text
    if len(sentence) > limit:
        sentence = sentence[:limit].rsplit(" ", 1)[0] + "..."
    return sentence


def unbuilt(text: str) -> list[str]:
    """Names listed under `unbuilt_candidates`, which map to Unavailable."""
    block = text.split("unbuilt_candidates:", 1)
    if len(block) == 1:
        return []
    body = block[1]
    listed = re.search(r"new renderer work:\s*(.+?)\.", body, re.S)
    if not listed:
        return []
    return [name.strip() for name in listed.group(1).replace("\n", " ").split(",") if name.strip()]


def plan_link(repo_path: str) -> str:
    """Ledger paths are repo-root-relative; the matrix is three levels down.

    Emitting them verbatim produces links that 404 for every reader of the
    published file, which is a defect specific to a document whose whole purpose
    is that outsiders can check its claims.
    """
    depth = len(OUTPUT.parent.relative_to(ROOT).parts)
    return "../" * depth + repo_path


def cell(text: str) -> str:
    """Markdown table cells cannot contain a raw pipe or a line break."""
    return text.replace("|", "\\|").replace("\n", " ").strip()


def render() -> str:
    systems_text, tickets_text = read(SYSTEMS), read(TICKETS)
    systems = records(systems_text)
    # Preview-0 tickets only. `status-ledger.yaml` is shared between overlays --
    # PREVIEW0-EXEC-001 owns 7 records, COMPAT-C1-EXEC-001 owns the rest -- and this
    # generator had no filter, unlike generate_compat_capability_matrix.py, which
    # scopes by overlay. Without it every compat ticket is published in a table
    # headed "These are the Preview-0 release blockers", which is a false claim
    # about the release, manufactured by regenerating the file. The compat ledger
    # grew from 37 to 53 records on 2026-08-14, so the next regeneration would have
    # published 53 of them that way.
    tickets = [t for t in records(tickets_text) if t.get("overlay") == PREVIEW0_OVERLAY]
    if not tickets:
        sys.exit(f"no ledger record declares overlay {PREVIEW0_OVERLAY}")

    lines: list[str] = []
    add = lines.append

    add("# Preview 0 — capability matrix")
    add("")
    add("**Generated file — do not edit by hand.**")
    add("")
    add("Produced by `scripts/generate_capability_matrix.py` from")
    add("`docs/roadmap/renderer-systems-ledger.yaml` and `docs/roadmap/status-ledger.yaml`.")
    add("CI re-generates it and fails on drift, so this table cannot quietly disagree with")
    add("the ledgers it summarises. Regenerate with:")
    add("")
    add("```sh")
    add("python scripts/generate_capability_matrix.py")
    add("```")
    add("")
    add("| Rating | Meaning |")
    add("| --- | --- |")
    add("| **Works** | Present and complete against its own plan, on by default. |")
    add("| **Partial** | Present, with a stated gap. The gap is named in the row. |")
    add("| **Experimental** | Complete but off by default. You must opt in. |")
    add("| **Unavailable** | No implementation. Listed so absence is explicit rather than assumed. |")
    add("")
    add("`Audited` is the date the entry was last checked item by item against the branch.")
    add("An older date means the claim is carried from a report, not re-verified — read those")
    add("rows as \"exists, extent unconfirmed\".")
    add("")

    add("## Renderer systems")
    add("")
    add("| System | Rating | Summary | Audited | Plan |")
    add("| --- | --- | --- | --- | --- |")
    for entry in systems:
        status = entry.get("status", "")
        if status not in STATUS_LABEL:
            continue
        plan = entry.get("plan", "")
        plan_cell = f"[plan]({plan_link(plan)})" if plan and plan != "null" else "—"
        # The legend promises that a Partial row names its gap, so prefer the
        # explicit `known_gap` when the entry has one. Falling back to the first
        # sentence of `completeness` was leaving Partial rows describing what the
        # system DOES -- which reads as a Works row with a different label, and
        # makes the rating look arbitrary to the outside reader it exists for.
        summary = entry.get("known_gap") or first_sentence(entry.get("completeness", ""))
        add(
            f"| {cell(entry.get('title', entry['id']))} "
            f"| **{STATUS_LABEL[status]}** "
            f"| {cell(first_sentence(summary))} "
            f"| {cell(entry.get('audited', 'null'))} "
            f"| {plan_cell} |"
        )
    for name in unbuilt(systems_text):
        add(f"| {cell(name)} | **Unavailable** | No implementation on this branch. | — | — |")
    add("")

    add("## Build-truth tickets")
    add("")
    add("These are the Preview-0 release blockers. `VALIDATED` here means the integration")
    add("owner verified it, not that a second party did — the project has one human, so")
    add("every review is owner-performed and each ticket records what was actually")
    add("exercised. That distinction is the honest reading of every row below.")
    add("")
    add("| Ticket | Title | Lifecycle | Reviewed by |")
    add("| --- | --- | --- | --- |")
    for entry in tickets:
        if "status" not in entry:
            continue
        reviewer = entry.get("reviewer", "null")
        reviewer = "not yet reviewed" if reviewer in {"null", ""} else reviewer
        add(
            f"| `{entry['id']}` "
            f"| {cell(entry.get('title', ''))} "
            f"| {entry['status']} "
            f"| {cell(reviewer)} |"
        )
    add("")
    return "\n".join(lines) + "\n"


def main(argv: list[str]) -> int:
    generated = render()
    if "--check" in argv:
        if not OUTPUT.is_file():
            print(f"missing generated file: {OUTPUT.relative_to(ROOT)}", file=sys.stderr)
            return 1
        # Compare normalised, so a CRLF checkout is not reported as drift.
        current = OUTPUT.read_bytes().replace(b"\r\n", b"\n").decode("utf-8")
        if current != generated:
            print(
                f"{OUTPUT.relative_to(ROOT)} is out of date with the ledgers; "
                "regenerate with `python scripts/generate_capability_matrix.py`",
                file=sys.stderr,
            )
            return 1
        print(f"capability matrix up to date ({OUTPUT.relative_to(ROOT)})")
        return 0
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(generated.encode("utf-8"))
    print(f"wrote {OUTPUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
