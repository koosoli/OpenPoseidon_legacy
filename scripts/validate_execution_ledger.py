#!/usr/bin/env python3
"""Overlay-scoped validation of the shared execution ledger.

`docs/roadmap/status-ledger.yaml` is the single lock source for every execution
overlay on the integration target, because the WIP limits overlays declare are
scoped to the target rather than to one overlay -- two ledger files could not
enforce them.  But more than one overlay now claims tickets in that file, so a
validator that checks the whole ledger against one overlay is wrong: the
COMPAT-C1 bootstrap asks for its seed records to be merged into the same file
that `validate_preview0_ledger.py` requires to hold *exactly* the Preview-0
ticket set.  Satisfying either half of that broke the other.

Each ledger record therefore carries an `overlay:` field naming the overlay that
owns it, and validation runs per overlay over that subset.  Preview-0 keeps its
exact-set rule; an overlay may relax it to a superset when it deliberately seeds
umbrella/hold records it does not authorise for claiming.

This module holds the shared checks.  Overlay-specific evidence rules live in
the per-overlay entry points that call `validate()`.
"""
from __future__ import annotations

import hashlib
import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
LEDGER = ROOT / "docs/roadmap/status-ledger.yaml"

SCHEDULING_STATES = {"OPEN", "ACTIVE", "BLOCKED", "DONE", "HOLD"}
LIFECYCLE_STATES = {
    "PLANNED", "RESEARCHED", "PROTOTYPED", "INTEGRATED",
    "VALIDATED", "SHIPPABLE", "DEFERRED", "SUPERSEDED",
}
OUTCOMES = {"ADOPTED", "ADAPTED", "REJECTED", "DEFERRED", "NOT_APPLICABLE"}

REQUIRED_FIELDS = (
    "id:", "overlay:", "is_example:", "title:", "class:", "obligation:",
    "execution_mode:", "scheduling_state:", "status:", "outcome:",
    "owner:", "branch:", "baseline_commit:", "verification_commit:",
    "milestone:", "milestone_role:", "depends_on:", "blocked_by:",
    "blocks:", "implementation_commits:", "production_call_sites:",
    "test_ids:", "benchmark_artifacts:", "evidence_hash:",
    "decision_record:", "known_failures:", "fallback:", "reviewer:",
    "reviewer_method:", "supersedes:", "evidence:",
)


def text(path: pathlib.Path) -> str:
    if not path.is_file():
        raise ValueError(f"missing: {path.relative_to(ROOT)}")
    return path.read_text(encoding="utf-8")


def canonical_bytes(path: pathlib.Path) -> bytes:
    """Read a tracked text file as CI will check it out.

    The roadmap and overlay hashes are recorded by hand on Windows and verified
    on Linux, so hashing the working-tree bytes makes the check depend on line
    endings rather than on content.  `.gitattributes` pins `*.md` to `eol=lf`,
    so a Windows editor that writes CRLF produces a file whose hash cannot match
    the blob CI reads -- and the failure is invisible locally, because the same
    CRLF bytes are on both sides of the local comparison.  That happened: an
    activation commit recorded a CRLF hash and would have failed hosted CI only.

    Normalising to LF costs nothing, since the recorded values were always LF
    hashes; it only stops a CRLF worktree from agreeing with itself.
    """
    if not path.is_file():
        raise ValueError(f"missing: {path.relative_to(ROOT)}")
    return path.read_bytes().replace(b"\r\n", b"\n")


def ids(doc: str, key: str) -> list[str]:
    lines = doc.splitlines()
    try:
        start = next(i for i, line in enumerate(lines) if line == f"{key}:") + 1
    except StopIteration as exc:
        raise ValueError(f"missing overlay key: {key}") from exc
    found = []
    for line in lines[start:]:
        if line and not line.startswith(" "):
            break
        stripped = line.strip()
        if stripped.startswith("- id: "):
            found.append(stripped.removeprefix("- id: "))
        elif key in {"preview_blockers", "authorised_tickets"} and stripped.startswith("- "):
            found.append(stripped.removeprefix("- "))
    return found


def ticket_block(doc: str, ticket: str) -> str:
    marker = f"- id: {ticket}\n"
    try:
        return marker + doc.split(marker, 1)[1].split("\n  - id:", 1)[0]
    except IndexError as exc:
        raise ValueError(f"missing ledger record: {ticket}") from exc


def value(block: str, field: str) -> str:
    for line in block.splitlines():
        if line.strip().startswith(f"{field}:"):
            return line.split(":", 1)[1].strip()
    raise ValueError(f"missing {field}")


def section_value(doc: str, section: str, field: str) -> str:
    """Read `field` from inside a named top-level mapping.

    `value()` takes the first match anywhere, which is ambiguous once an overlay
    carries the same key under more than one section -- `path:` appears under
    both `roadmap:` and `ledger:` in the COMPAT-C1 overlay, and the Preview-0
    reading happened to be correct only because `roadmap:` came first.
    """
    lines = doc.splitlines()
    try:
        start = next(i for i, line in enumerate(lines) if line == f"{section}:") + 1
    except StopIteration as exc:
        raise ValueError(f"missing overlay section: {section}") from exc
    for line in lines[start:]:
        if line and not line.startswith(" "):
            break
        if line.strip().startswith(f"{field}:"):
            return line.split(":", 1)[1].strip()
    raise ValueError(f"missing {section}.{field}")


def git(*args: str) -> str:
    result = subprocess.run(("git", *args), cwd=ROOT, text=True, capture_output=True)
    if result.returncode:
        raise ValueError(result.stderr.strip() or f"git {' '.join(args)} failed")
    return result.stdout.strip()


def git_bytes(*args: str) -> bytes:
    result = subprocess.run(("git", *args), cwd=ROOT, capture_output=True)
    if result.returncode:
        message = result.stderr.decode("utf-8", errors="replace").strip()
        raise ValueError(message or f"git {' '.join(args)} failed")
    return result.stdout


def tracked_file(raw_path: str, context: str) -> pathlib.Path:
    path = ROOT / raw_path
    if not path.is_file():
        raise ValueError(f"{context} missing file: {raw_path}")
    git("ls-files", "--error-unmatch", "--", raw_path)
    return path


def evidence_paths(block: str) -> list[str]:
    paths: list[str] = []
    for line in block.splitlines():
        match = re.match(r"\s+-\s+((?:docs|engine|apps|scripts|\.github)/\S+)", line)
        if match:
            paths.append(match.group(1))
    return paths


def list_values(raw: str) -> list[str]:
    return [item for item in raw.strip("[]").replace(" ", "").split(",") if item]


def verify_production_call_sites(block: str, ticket: str) -> None:
    """Require stable file/symbol records instead of prose-only call sites."""
    lines = block.splitlines()
    start = next(i for i, line in enumerate(lines) if line.strip().startswith("production_call_sites:"))
    if lines[start].strip() == "production_call_sites: []":
        return
    entries: list[dict[str, str]] = []
    current: dict[str, str] | None = None
    for line in lines[start + 1:]:
        if line.startswith("    ") and not line.startswith("      "):
            break
        stripped = line.strip()
        if stripped.startswith("- file: "):
            if current is not None:
                entries.append(current)
            current = {"file": stripped.removeprefix("- file: ")}
        elif current is not None and stripped.startswith("symbol: "):
            current["symbol"] = stripped.removeprefix("symbol: ")
    if current is not None:
        entries.append(current)
    for entry in entries:
        if not entry.get("file") or not entry.get("symbol"):
            raise ValueError(f"{ticket} production call site needs file and symbol")
        tracked_file(entry["file"], f"{ticket} production call site")
    raw_lines = [line.strip() for line in lines[start + 1:] if line.startswith("      - ")]
    if raw_lines and not entries:
        raise ValueError(f"{ticket} production call sites must use file/symbol records")


def scoped_ledger_ids(ledger: str, overlay_id: str) -> list[str]:
    """Ticket IDs whose ledger record declares this overlay as its owner."""
    scoped: list[str] = []
    current: str | None = None
    for line in ledger.splitlines():
        stripped = line.strip()
        if stripped.startswith("- id: "):
            current = stripped.removeprefix("- id: ")
        elif current is not None and stripped.startswith("overlay: "):
            if stripped.removeprefix("overlay: ") == overlay_id:
                scoped.append(current)
            current = None
    return scoped


def verify_roadmap(overlay: str, overlay_path: pathlib.Path) -> tuple[pathlib.Path, str]:
    raw_roadmap = section_value(overlay, "roadmap", "path")
    roadmap = ROOT / raw_roadmap
    tracked_file(raw_roadmap, "canonical roadmap")
    tracked_file(str(overlay_path.relative_to(ROOT)), "canonical overlay")
    expected_hash = section_value(overlay, "roadmap", "sha256")
    actual_hash = hashlib.sha256(canonical_bytes(roadmap)).hexdigest()
    if actual_hash != expected_hash:
        raise ValueError("canonical roadmap hash mismatch")
    return roadmap, actual_hash


def validate(overlay_path: pathlib.Path, extra_checks=None) -> int:
    """Validate one overlay against the tickets it owns in the shared ledger."""
    try:
        overlay = text(overlay_path)
        ledger = text(LEDGER)
        overlay_id = value(overlay, "overlay_id")

        if value(overlay, "template") not in {"true", "false"}:
            raise ValueError("overlay template must be true or false")
        overlay_template = value(overlay, "template") == "true"
        overlay_activation = section_value(overlay, "execution", "activation_state")
        activation = section_value(ledger, "execution", "activation_state")
        if activation not in {"PREPARED", "ACTIVE"}:
            raise ValueError(f"invalid activation state {activation}")
        if overlay_template and overlay_activation != "PREPARED":
            raise ValueError("template overlay must remain PREPARED")
        if not overlay_template and overlay_activation != "ACTIVE":
            raise ValueError("resolved overlay must be ACTIVE")
        # A template overlay is not yet execution truth, so it may sit alongside
        # an already-active ledger; a resolved one must agree with it.
        if not overlay_template and overlay_activation != activation:
            raise ValueError("overlay and ledger activation states differ")

        roadmap, actual_hash = verify_roadmap(overlay, overlay_path)
        roadmap_text = text(roadmap)
        if extra_checks:
            extra_checks(roadmap, overlay_path)

        authorised = ids(overlay, "authorised_tickets")
        blockers = ids(overlay, "preview_blockers")
        queue = ids(overlay, "execution_queue")
        holds = ids(overlay, "named_holds")
        candidates = ids(overlay, "next_candidates")
        ledger_ids = scoped_ledger_ids(ledger, overlay_id)
        if not ledger_ids:
            raise ValueError(f"no ledger record declares overlay {overlay_id}")

        all_ledger_ids = [
            line.strip().removeprefix("- id: ")
            for line in ledger.splitlines()
            if line.strip().startswith("- id: ")
        ]
        if len(queue) != len(set(queue)) or len(all_ledger_ids) != len(set(all_ledger_ids)):
            raise ValueError("duplicate ticket ID")
        if set(authorised) != set(queue):
            raise ValueError("authorised and queue ticket sets differ")

        # EXACT is the default so an overlay cannot silently widen its own scope;
        # SUPERSET is for overlays that deliberately seed umbrella/hold records
        # they do not authorise for claiming.
        policy = "EXACT"
        for line in overlay.splitlines():
            if line.startswith("ledger_ticket_set_policy: "):
                policy = line.removeprefix("ledger_ticket_set_policy: ").strip()
        if policy not in {"EXACT", "SUPERSET"}:
            raise ValueError(f"invalid ledger_ticket_set_policy {policy}")
        if policy == "EXACT":
            if set(authorised) != set(ledger_ids):
                raise ValueError("authorised and scoped ledger ticket sets differ")
        elif not set(authorised).issubset(ledger_ids):
            # Only authorised tickets are required to have a record: the ledger
            # is the claim lock, and a hold or an unpromoted candidate cannot be
            # claimed. Demanding records for holds too would force this overlay
            # to take ownership of pre-existing holds it only restates.
            missing = sorted(set(authorised) - set(ledger_ids))
            raise ValueError(f"authorised tickets absent from the scoped ledger: {missing}")

        if not set(blockers).issubset(authorised):
            raise ValueError("preview blocker is not authorised")
        if set(holds) & set(authorised):
            raise ValueError("named hold appears in authorised tickets")
        canonical_ticket_ids = set(re.findall(r"^##\s+([A-Z][A-Z0-9-]+)\s+[—-]", roadmap_text, re.MULTILINE))
        for ticket in authorised + holds + candidates:
            if ticket not in canonical_ticket_ids:
                raise ValueError(f"canonical roadmap does not define {ticket}")

        active = []
        for ticket in ledger_ids:
            block = ticket_block(ledger, ticket)
            for field in REQUIRED_FIELDS:
                if field not in block:
                    raise ValueError(f"{ticket} missing {field}")
            verify_production_call_sites(block, ticket)
            scheduling = value(block, "scheduling_state")
            lifecycle = value(block, "status")
            if scheduling not in SCHEDULING_STATES:
                raise ValueError(f"{ticket} has invalid scheduling state {scheduling}")
            if lifecycle not in LIFECYCLE_STATES:
                raise ValueError(f"{ticket} has invalid lifecycle status {lifecycle}")
            completed = scheduling == "DONE" or lifecycle in {"VALIDATED", "SHIPPABLE"}
            outcome = value(block, "outcome")
            # A planned ticket has no outcome yet and saying so beats picking one
            # of the five verdicts before any work has happened; a completed one
            # must commit to a verdict.
            if outcome not in OUTCOMES and not (outcome == "null" and not completed):
                raise ValueError(f"{ticket} has invalid outcome {outcome}")
            evidence = evidence_paths(block)
            for evidence_path in evidence:
                tracked_file(evidence_path, ticket)
            decision = value(block, "decision_record")
            if decision != "null":
                tracked_file(decision, ticket)
            if completed:
                verify_completed(ledger, block, ticket, evidence)
            if scheduling == "ACTIVE":
                for field in ("owner", "branch", "baseline_commit"):
                    if value(block, field) in {"", "null"}:
                        raise ValueError(f"ACTIVE {ticket} has no {field}")
                active.append(ticket)
            for dependency in list_values(value(block, "depends_on")):
                if not dependency:
                    continue
                # A planned ticket may depend on one that no overlay has seeded
                # yet, as long as the roadmap defines it -- otherwise seeding a
                # follow-on would force fake ledger ownership of every ticket it
                # names. A completed one may not: you cannot finish against a
                # dependency nothing is tracking.
                if dependency not in all_ledger_ids:
                    if dependency not in canonical_ticket_ids:
                        raise ValueError(f"{ticket} depends on unknown ticket {dependency}")
                    if scheduling == "DONE":
                        raise ValueError(f"completed {ticket} depends on unledgered {dependency}")
                    continue
                if scheduling == "DONE" and value(ticket_block(ledger, dependency), "scheduling_state") != "DONE":
                    raise ValueError(f"completed {ticket} depends on unfinished {dependency}")

        # WIP is a property of the integration target, not of one overlay, so it
        # is counted across every ACTIVE record in the file.
        all_active = [
            ticket for ticket in all_ledger_ids
            if value(ticket_block(ledger, ticket), "scheduling_state") == "ACTIVE"
        ]
        verify_wip(overlay, queue, all_active, len(authorised))

        print(
            f"{overlay_id} ledger valid: {len(ledger_ids)} scoped tickets "
            f"({len(authorised)} authorised, {len(all_active)} ACTIVE target-wide), "
            f"roadmap SHA-256 {actual_hash}"
        )
    except ValueError as exc:
        print(f"{overlay_path.name} ledger invalid: {exc}", file=sys.stderr)
        return 1
    return 0


def queue_sizes(overlay: str) -> dict[str, str]:
    """Ticket -> declared size, read from the overlay's execution_queue."""
    sizes: dict[str, str] = {}
    current: str | None = None
    for line in overlay.splitlines():
        stripped = line.strip()
        if stripped.startswith("- id: "):
            current = stripped.removeprefix("- id: ")
        elif current and stripped.startswith("size: "):
            sizes[current] = stripped.removeprefix("size: ")
    return sizes


def verify_wip(overlay: str, queue: list[str], all_active: list[str], authorised_count: int) -> None:
    """Enforce the WIP limits the overlay declares, by size where it declares them.

    The previous rule was a flat maximum of three ACTIVE tickets, inherited from
    Preview-0 and then applied target-wide when the ledger became shared. That is
    stricter than any overlay actually asks for -- COMPAT-C1 declares 2 large plus
    3 small -- so it was rejecting states its own overlay permits.

    Sizes come from the overlay's execution_queue. An ACTIVE ticket no overlay
    sizes is counted against the total only, since guessing its size would either
    invent headroom or invent a blockage.
    """
    limits: dict[str, int] = {}
    in_limits = False
    for line in overlay.splitlines():
        if line.rstrip() == "wip_limits:":
            in_limits = True
            continue
        if in_limits:
            if line and not line.startswith(" "):
                break
            stripped = line.strip()
            for key in ("active_large_tickets_max", "active_small_tickets_max"):
                if stripped.startswith(f"{key}:"):
                    limits[key] = int(stripped.split(":", 1)[1].strip())

    sizes = queue_sizes(overlay)
    large = [t for t in all_active if sizes.get(t) == "LARGE"]
    small = [t for t in all_active if sizes.get(t) == "SMALL"]

    if "active_large_tickets_max" in limits and len(large) > limits["active_large_tickets_max"]:
        raise ValueError(f"active LARGE tickets exceed the WIP maximum: {large}")
    if "active_small_tickets_max" in limits and len(small) > limits["active_small_tickets_max"]:
        raise ValueError(f"active SMALL tickets exceed the WIP maximum: {small}")

    total_max = limits.get("active_large_tickets_max", 0) + limits.get("active_small_tickets_max", 0)
    if not total_max:
        total_max = 3  # Preview-0's original flat rule, for an overlay declaring no sizes.
    if len(all_active) > total_max:
        raise ValueError(f"active ticket count {len(all_active)} exceeds the WIP maximum {total_max}: {all_active}")


# Classes whose result the owner can actually look at. A change that reaches the
# screen or the running game cannot be signed off by a test alone, because what is
# being judged is how it looks or behaves.
OWNER_VERIFIABLE_CLASSES = {"LOCAL_COSMETIC", "WORLD_VISUAL", "GAMEPLAY"}


def verify_review(ledger: str, block: str, ticket: str) -> None:
    """Check a completed ticket was reviewed by whichever route actually applies.

    Preview-0 had one route -- a named human -- and Oliver Kay is the only human on
    the project. For offline work with no runtime surface that gate blocks on a
    review that cannot meaningfully happen: there is nothing to look at, so waiting
    for someone to look at it stalls the queue forever while adding no scrutiny.

    So there are two routes, and which one a ticket may use is decided by its class
    rather than by whoever is writing the record:

    OWNER_REVIEW      a named reviewer, plus a method saying what was exercised.
                      Required for anything the owner can actually see or play.
    AUTOMATED_EVIDENCE machine-checkable proof only, and honest about its limits.
                      Permitted for classes with no runtime or visual surface.

    What does NOT change is the ledger's rule against asserting scrutiny that did
    not happen. AUTOMATED_EVIDENCE is not a quieter way to claim a human review --
    it records that no human reviewed it, names the tests that did, and must state
    what it did not check. A reader can therefore still tell the two apart.
    """
    review_model = "OWNER_REVIEW"
    for line in block.splitlines():
        if line.strip().startswith("review_model:"):
            review_model = line.split(":", 1)[1].strip()
    if review_model not in {"OWNER_REVIEW", "AUTOMATED_EVIDENCE"}:
        raise ValueError(f"{ticket} has invalid review_model {review_model}")

    ticket_class = value(block, "class")
    if review_model == "AUTOMATED_EVIDENCE":
        if ticket_class in OWNER_VERIFIABLE_CLASSES:
            raise ValueError(
                f"{ticket} is class {ticket_class}, whose result the owner can see; "
                "it needs OWNER_REVIEW, not AUTOMATED_EVIDENCE"
            )
        if not list_values(value(block, "test_ids")):
            raise ValueError(f"{ticket} claims AUTOMATED_EVIDENCE but names no test_ids")
        method = value(block, "reviewer_method")
        if method in {"", "null"}:
            raise ValueError(
                f"{ticket} claims AUTOMATED_EVIDENCE and needs a reviewer_method "
                "recording what was machine-checked and what was not"
            )
        # The limits clause is the honesty of this route. Without it the record
        # reads as a clean bill of health that nothing actually issued.
        if "not checked" not in block.lower() and "does not" not in block.lower():
            raise ValueError(
                f"{ticket} claims AUTOMATED_EVIDENCE but states no limits; "
                "record what the automated evidence does not cover"
            )
        if value(block, "reviewer") not in {"", "null"}:
            raise ValueError(
                f"{ticket} claims AUTOMATED_EVIDENCE but also names a reviewer; "
                "use OWNER_REVIEW if a human actually reviewed it"
            )
        return

    reviewer = value(block, "reviewer")
    owner = value(block, "owner")
    integration_owner = section_value(ledger, "execution", "integration_owner")
    if reviewer in {"", "null", owner}:
        raise ValueError(f"completed {ticket} needs a named reviewer")
    # The integration owner may review their own integration, but only on the
    # record.  Preview 0 has no second reviewer, so rather than let the ledger
    # imply an independence it does not have, an owner review must state how it
    # was carried out.
    if reviewer == integration_owner:
        method = value(block, "reviewer_method")
        if method in {"", "null"}:
            raise ValueError(
                f"completed {ticket} is reviewed by the integration owner "
                "and needs a reviewer_method recording how it was verified"
            )


def verify_completed(ledger: str, block: str, ticket: str, evidence: list[str]) -> None:
    verification = value(block, "verification_commit")
    if not re.fullmatch(r"[0-9a-f]{7,64}", verification):
        raise ValueError(f"completed {ticket} needs a verification commit")
    git("cat-file", "-e", f"{verification}^{{commit}}")
    evidence_hash = value(block, "evidence_hash")
    if not re.fullmatch(r"[0-9a-f]{64}", evidence_hash):
        raise ValueError(f"completed {ticket} needs a SHA-256 evidence hash")
    # Recompute it. The format check alone accepted any 64 hex characters, so
    # the field attested to nothing: a hash could be invented, or left behind
    # after the evidence it covered was replaced. Deriving it from the evidence
    # files makes a stale or fabricated value fail here instead of shipping.
    try:
        import compute_evidence_hash as ceh
    except ImportError as exc:  # pragma: no cover - packaging guard
        raise ValueError(f"cannot verify {ticket} evidence hash: {exc}") from exc
    expected, hashed = ceh.evidence_hash(block)
    if not hashed:
        raise ValueError(f"completed {ticket} has an evidence hash but no tracked evidence files")
    if expected != evidence_hash:
        raise ValueError(
            f"{ticket} evidence hash does not match its evidence "
            f"({len(hashed)} file(s)); recompute with "
            f"`python scripts/compute_evidence_hash.py --all --write`"
        )
    verify_review(ledger, block, ticket)
    if not evidence:
        raise ValueError(f"completed {ticket} needs tracked file evidence")
    if not list_values(value(block, "implementation_commits")):
        raise ValueError(f"completed {ticket} needs implementation commits")
    for implementation_commit in list_values(value(block, "implementation_commits")):
        if not re.fullmatch(r"[0-9a-f]{7,64}", implementation_commit):
            raise ValueError(f"completed {ticket} has an invalid implementation commit")
        git("cat-file", "-e", f"{implementation_commit}^{{commit}}")
