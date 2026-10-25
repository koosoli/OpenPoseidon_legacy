# Conquest on Everon (Slice 0 dev mission)

An NPC-only war between WEST (HQ Morton) and EAST (HQ the airfield) over Figari, Entre Deux,
Montignac, Gravette and Tyrone. The engine's Conquest referee (`engine/Poseidon/Game/Conquest`)
reads `class CfgConquest` in `description.ext`; everything else (squads, orders, captures,
purchases, cleanup, map markers) is the referee's work. The player is a captive, invulnerable
spectator at Le Moule.

```text
--window --test-mission <repository>/dev-missions/conquest-everon.Eden --log-file <log>
```

- Map (M): one ellipse per objective in its owner's colour, labelled with capture progress.
- Dev panel (`--dev`, Ctrl+`): the Conquest tab shows sides, objectives, groups and the war log.
- Script: `conquestState` returns the one-line status; the engine logs it every game minute
  ("Conquest status:") and every capture, purchase and order ("Conquest:").

Plan and rules: `docs/roadmap/conquest/CONQUEST-PLAN.md`. Uses stock CWA classes only; no game data
is included.
