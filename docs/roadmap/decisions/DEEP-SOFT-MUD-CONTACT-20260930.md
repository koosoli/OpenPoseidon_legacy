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
