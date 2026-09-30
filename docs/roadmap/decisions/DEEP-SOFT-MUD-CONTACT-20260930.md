# Decimetre soft-mud contact geometry

The owner requested deeper volumetric rainy mud. The default compressible soil
layer is now 0.24 metres, with a 0.35-metre hard storage limit. Rain increases
capacity through the existing simulation-time wetness state; it does not lower
untouched terrain. Only real admitted animation boot contacts deform the field.

`MudField` remains header-only. `POSEIDON_MUD_LAYER_DEPTH` accepts a finite exact
number between 0.05 and 0.35 metres at first singleton creation; invalid values
keep 0.24. `SetLayerDepth` applies the same bounds. Configuration and drying never
erase or clip an earlier imprint. The unchanged packed header carries the hard
0.35 limit, independently of current contact capacity, so CPU and GPU can retain
older deeper samples after a capacity reduction.

Each toe/heel loading edge applies pressure 0.65 toward the local wet-layer
capacity. Further contacts compress the remaining space rather than adding a
fixed depth indefinitely. The depth-scaled smooth shoulder has radius at least
0.35 metres and at most 1.4 metres. It represents supporting soil compression
around a boot, rather than a narrow vertical hole. Four bounded alternating
relaxation sweeps enforce adjacent-cell depth differences at most 0.45 times
cell spacing. Consequently both components of the actual bilinear physical
gradient remain bounded by 0.45 even across displaced overlapping contacts.
The sweeps use fixed scratch storage and apply pressure once per contact.

The nine source admission probes now cover the maximum shoulder, toe/heel
offset and interpolation support (1.6 metres). Existing authoritative stock
soil checks, native/A3 conservative refusal, roadway, shore, snow, actor-height
and upward ViewGeometry roof gates remain. Slope admission subtracts the
already-applied mud gradient from `Landscape::SurfaceY` gradients: existing mud
must not reject its own next contact as a newly steep natural slope. Contact
height still uses the actual depressed physical support.

Focused Clang C++20 assertions pass, including dry/no-contact geometry, partial
wet capacity, a first heavy-wet contact over 0.10 metres, repeat loading over
0.20 and at most 0.24 metres, configurable 0.35 capacity, overlapping gradients,
signed packed-height parity, invalid input, bounded allocation, persistence
through drying/configuration changes, and stock source negatives.

Root must raise the renderer `valid_mud_view` maximum header bound from 0.06
to 0.35 and run actual production GPU deep-height tests before integration is
complete. No shader, Landscape, renderer, boot producer, harness or CMake file
is edited by this change. The existing 2-cm installed proof establishes earlier
contact plumbing only; decimetre installed boot support, covered negative,
rendered wet appearance, repeat compression and runtime cost remain pending.
No full-map muddy lowering, fluid transport or arbitrary native-source support
is claimed.

### Integrated and installed deep-layer proof

Root integrates the0.35m renderer header, actual GPU terrain/conform deep-height
probes and physical Landscape heights/gradients in matching06a7c41e. The full
engine build and609 serial Rust checks pass (two existing ignored). Installed
`build/mud-footprints/noe-deep-slowcovered-20260930-214412-f119f5` passes:
real stock Nogova soil walking4.310m,364 negative physical samples with minimum
-0.199584m, exact physical height/gradient agreement, actual boot trace pressure
at wetness0.92, paused state and exact camera-return grid, normal exit0 and
unchanged installed pair. The actor's final physical support gap is2.45mm while
standing in the deformed lane. Under the actual lifted stock roof,0.750m real
walking leaves0 chunks and uniform wetness0; after roof removal actual uniform
wetness reaches0.081998 under visible liquid particles.

Root inspected dry and wet-close captures: broad supporting soil compression
and darker wet material remain visible beneath boot details. These are
descriptive appearance checks; actor position and wet shading differ, so they
do not isolate a material-only pixel measurement. Exact byte-identical wet
camera-return images additionally confirm persistence in this paused control.
The earlier fast-poll covered run remains failed. Repeat installed compression,
hard-ground/border/snow negatives, other maps, general cost and fluid transport
remain open; the successful supported0.24m layer is not whole-map lowering.
