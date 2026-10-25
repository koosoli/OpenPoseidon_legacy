"""Summarise a Conquest soak run (scripts/Run-ConquestSoak.ps1) against the Slice 0 checks.

Usage: python -I scripts/conquest_soak_report.py build/conquest-runs/<stamp>

Reads engine.log ("Conquest:" events, "Conquest status:" lines) and state.jsonl, and prints:
captures and neutralisations per game hour, who took what when, purchases and refusals, stalls,
the winner, live soldiers against the cap, and the referee's tick cost.
"""

import json
import re
import sys
from pathlib import Path


def main() -> int:
    run = Path(sys.argv[1])
    log = (run / "engine.log").read_text(encoding="utf-8", errors="replace").splitlines()
    events = [line.split("Conquest: ", 1)[1] for line in log if "Conquest: " in line]
    states = [json.loads(line) for line in (run / "state.jsonl").read_text().splitlines() if line.strip()]

    captured = [e for e in events if " captured " in e]
    neutralised = [e for e in events if " neutralised " in e]
    bought = [e for e in events if " bought a squad " in e]
    refused = [e for e in events if " refused: " in e]
    stalled = [e for e in events if " stalled " in e]
    wins = [e for e in events if " wins the war" in e]

    game_end = float(states[-1]["game"]) if states else 0.0
    hours = max(game_end / 3600.0, 1e-6)
    print(f"run: {run}")
    print(f"game time covered: {game_end / 60:.1f} min ({len(states)} polls)")
    print(f"captures: {len(captured)}  neutralisations: {len(neutralised)}  "
          f"changes of hands per game hour: {(len(captured) + len(neutralised)) / hours:.1f}")
    for e in captured + neutralised:
        print("  ", e)
    print(f"purchases: {len(bought)}  refused: {len(refused)}  stalls: {len(stalled)}")
    for e in refused[:10]:
        print("   refused:", e)
    for e in stalled[:10]:
        print("   stalled:", e)
    print("winner:", wins[0] if wins else "none yet")

    live = {}
    ticks = []
    for s in states:
        text = s["state"]
        for side, value in re.findall(r"\| (\w+) pool [\d.-]+ score [\d.-]+ live (\d+)", text):
            live.setdefault(side, []).append(int(value))
        m = re.search(r"tick ([\d.]+) ms \(max ([\d.]+)\)", text)
        if m:
            ticks.append((float(m.group(1)), float(m.group(2))))
    for side, values in live.items():
        print(f"live {side}: min {min(values)} max {max(values)} last {values[-1]}")
    if ticks:
        late = [t for t, _ in ticks[2:]] or [ticks[-1][0]]
        print(f"referee tick: last-sample mean {sum(late) / len(late):.2f} ms, max ever {ticks[-1][1]:.2f} ms")
    if states:
        print("final:", states[-1]["state"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
