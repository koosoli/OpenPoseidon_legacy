# MAT-053 — the bridged cockpit POV: the proxy record's translation is mirrored

- **Ticket:** MAT-053
- **Date:** 2026-08-26
- **Status:** A1 and A2 FIXED and capture-verified; A3 plausible, wants a play-test.

## The report

Owner, standing for weeks: on bridged A1/A2/A3 vehicles the first-person view
sits "1-2 metres too far behind the cockpit — height and axes are right".

## Why there was no seat at all

A bridged vehicle's crew proxies point at Arma-era placeholders
(`ca/temp/proxies/mi35/pilot`, `a3/data_f/proxies/heli_light_01/pilot`) that
exist neither as files nor as CfgNonAIVehicles classes. `ShapeAdapter` skips
them — 875 skips in one eleven-vehicle boot, every pilot and driver seat among
them. A skipped crew proxy is not just a missing placeholder body: it is the
only thing that populates the driver/gunner `ManProxy`, so
`Transport::GetProxyCamera()` returns false and the interior camera falls back
to the hull.

## The mechanism, and the measurement that pinned it

The skipped proxy RECORD still carries the authored seat transform, so the
adapter now remembers the first pilot/driver seat per shape and
`Transport::InsideCamera`'s degraded path consults it.

**The record's translation is MIRRORED in X and Z relative to engine model
space.** The record's own 3x3 is identity, so this is a sign convention in the
record, not a rotation — `R·t` was tested and does not explain it.

Cross-checked on all three generations against each model's own authored
BOARDING point (`pos driver`), which lives in vertex space and therefore
cannot share the record's frame:

| model | boarding `pos driver` | record | record negated |
|---|---|---|---|
| A1 `uh_60` | +1.72, −2.39, **+4.75** | −0.65, −1.51, **−4.57** | +0.65, −1.51, **+4.57** |
| A2 `mi24_v` | +1.68, −2.65, **+6.15** | +0.10, −1.38, **−5.62** | −0.10, −1.38, **+5.62** |
| A3 `heli_light_01_f` | −1.45, −1.92, **+1.11** | +0.34, −0.59, **−1.24** | −0.34, −0.59, **+1.24** |

Every negated seat lands just inboard of that model's own door, on the correct
side; every raw record lands at the tail. Independently, the A1 carries marker
GEOMETRY for the same proxy in its view LOD (vertex space, +0.65,−1.49,+3.57),
which agrees in sign — and drives that model through a separate, preferred path.

## Two anchor sources, in order

1. **Marker geometry** in the VIEW-PILOT LOD (`proxy:...pilot` named selection
   with vertices). Vertex space, no frame ambiguity. This is what the A1 uses.
   Empty on ODOL 48+, which ships selection names with zero vertices.
2. **The seat registry** (this record's mechanism) for the generations where
   marker selections are empty — A2 and A3.

The seated eye offset above either anchor is taken from the engine's own
`Person::GetPilotPosition()` — the same vector `GetProxyCamera` composes for
stock vehicles — rather than a tuned constant, so the degraded path lands where
the resolved path would.

## Dead ends, measured (do not re-walk)

- **Memory points are boarding positions, not seats.** `pos driver` /
  `pos_driver` measure Y ≈ −1.9 to −2.7 and X ≈ ±1.5 on all three — outside and
  below, beside the door. Even on A3, which additionally has `getin_pilot`,
  `pos driver` is still the outside point. `driverview` and `pilot` are absent
  on all three (all zero).
- **Recovering the crew proxy as a generic `ProxyDriver` class** (so a real
  `ManProxy` exists and `GetProxyCamera` runs) puts the A1 camera in the grass:
  it feeds the same mirrored record through the resolved path. Reverted.
- **`R·t`** — the record's rotation is identity; it cannot produce the flip.

## Status per generation

- **A1 UH-60** — verified in capture, seated in the cockpit (marker path).
- **A2 Mi-35** — verified in capture: instrument coaming, console and canopy
  frame all in view (registry path).
- **A3 MH-9** — numerically consistent with its door and step, but the Little
  Bird is an open-frame cockpit and the static capture shows mostly outside
  view, so it is NOT visually confirmed. Play-test before closing.
- **Stock content is untouched**: its crew proxies resolve, so none of this
  code runs. Regression-checked on the stock UH-60 (pilottest) — pixel values
  unchanged.
