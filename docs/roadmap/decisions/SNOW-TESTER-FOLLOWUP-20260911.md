# Snow and terrain tester follow-up

User-supplied observations, September 11. These are tester evidence, not a claim
that this session independently reproduced every case or knows the tester build.

## Confirmed by tester

- Grenades/artillery form snow depressions.
- Storm precipitation and accumulating cover work.
- Crawling leaves trails; further snowfall fills them.
- Building interiors remain free of deposited snow.
- Terrain painting produced two large holes and a hill on Nogova.

## Defects and implementation status

1. Trails eventually stop and recover after reset. SnowField retained every
   allocated chunk forever, including cells completely filled by later snow.
   At 32768 chunks all new locations were rejected. This is a demonstrated code
   defect consistent with the report; no tester log proves it is the only cause.
   Implemented bounded 128-key reclamation per simulation advance, also checked
   once before stamping at capacity. A per-chunk latest fill deadline ensures
   no live depression is removed. No camera-distance eviction. Key sweep remains
   valid across unordered_map rehash. Zero-strength stamps no longer allocate.
   UI distinguishes current capacity from historical rejected cells.
   Unit tests: 11 snow cases / 9933 assertions, including filling the entire
   storage cap, preserving live tracks, refilling, renewed stamping, refresh of
   a chunk deadline, bounded reclamation and reset. Installed refill/crawl test
   is pending. The finite cap still applies if all stored trails remain live.
2. High snow has inappropriate foot/vehicle tracks. Open: stamping currently
   removes a fraction of global depth with fixed footprint/tyre widths, not a
   body/undercarriage contact volume. Deep snow needs continuous displacement
   appropriate to the contacting body, not merely deeper small stamps.
3. Prone camera can see below high snow. Open: rendered terrain snow height and
   original movement/camera support height differ. Do not label culling changes
   or hiding the near snow as proper physical snow support.
4. Rotor wash does not displace cover. Open: AirflowField already represents
   rotor wash for other effects, but SnowField is not coupled to it. Use this
   existing field with bounded, time-scaled erosion and shelter checks; no new
   helicopter scan per snow cell and no instantaneous full-disc craters.
5. Player invisible after large terrain edits, first and third person. Open,
   not reproduced. PaintTerrain changes heights/collision and FlushCache; no
   direct player-hiding call found in that path. Need installed Nogova sequence
   with actor position, camera mode, alive/hidden state and terrain height before
   and after two pits and a hill, including a view outside the edited footprint.

Preserve working indoor masking, snow refill, crawling and explosion behaviour.
Do not attribute the player visibility issue to snow or DLSS without evidence.
