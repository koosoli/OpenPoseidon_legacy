# REN-SKY-002 — The cloud light march samples a two-tap density: 2.89 → 1.84 ms at 128 steps

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Follows:** REN-SKY-001 (64 steps saves ~1 ms but the owner chose the 128-step picture)

## The mechanism

Every primary sample of the cloud march that lands in cloud runs a four-sample march towards
the sun to get its self-shadowing (`CLOUD_LIGHT_STEPS`, geometric spacing). Each of those
samples called the full `cloud_density`: a domain-warp tap, the weather tap, the shape tap and
the detail-erosion tap. Four light samples times four taps is the bulk of the march.

The sun march only needs the optical depth towards the sun. The warp and the erosion are
high-frequency shaping of the *silhouette*; integrated over a shadow ray they change the depth
by a few percent. `cloud_density_light` keeps the weather and shape taps and drops the other
two, so a light sample costs half. The primary march still uses the full density, so the
silhouettes and edges the owner chose the 128-step picture for are untouched.

## Measured (Everon `perf_combat`, 3441×1440, three alternating runs per arm, stamp `f31fa2bf+dirty`)

| | four-tap light density (old) | **two-tap** |
|---|---:|---:|
| Sky: cloud march | 2.89 (2.87–2.92) | **1.84** (1.81–1.86) |
| Sky: cloud composite | 0.11 | 0.11 |
| GPU frame | 20.3 (18.6–22.8) | **18.0** (17.9–18.2) |

−1.05 ms, 36% of the march, at the owner's 128 steps. That is about what 64 steps bought
(REN-SKY-001) without giving up the step count.

## The picture

Sky band (top 55% of the frame) between one run of each arm: 3.8% of pixels differ by more than
16 levels, 0.45% by more than 48 — the cloud deck drifts between two runs. The crops
(`.tmp-veg/cloud_full_coarse.png`) show the same clouds with the same shading; the
self-shadowed undersides keep their gradient.

## Decision

**ON by default** (`WGR_CLOUD_LIGHT_COARSE=0` restores the four-tap light density). The owner
has the picture; if the shading reads flatter to them, the switch is the way back.

## Not measured

One scene, noon. Low sun (long shadow rays through the deck) is where the erosion term would
matter most and was not captured; a sunset crop is the follow-up if the owner sees a difference.

## Reversed 2026-09-03 (same day)

The owner looked at the crops and preferred the four-tap picture. Default is the full light
march again; `WGR_CLOUD_LIGHT_COARSE=1` keeps the 1 ms lever for a slow GPU. The picture check
is the gate, and it said no.
