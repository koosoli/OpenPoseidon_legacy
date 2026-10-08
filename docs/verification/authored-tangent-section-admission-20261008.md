# Authored tangent section admission

The optional authored UV0 tangent importer retains valid corner frames even if another corner in the same LOD is missing or invalid. Valid and missing frames remain distinct during vertex deduplication, so an invalid corner cannot inherit an adjacent authored frame.

ShapeAdapter uses the authored axes only for valid source vertices and generated axes elsewhere. An optional CPU provenance byte per imported vertex records that distinction. Retained rendering admits the authored tangent preview only when every actual face reference of a nonempty section has authored provenance. A different section or unreferenced vertex cannot invalidate it. Missing, empty, out-of-range and mixed sections decline the preview and retain normal-preview fallback.

The producer marker and derived-cache option advance from 2 to 3. Older cached imports cannot silently qualify. GPU layouts and renderer ABI do not change. Existing preview gates remain default off:

```
WGR_REFORGER_PBR_PREVIEW=1
WGR_REFORGER_LAYER_ORDER_PREVIEW=1
WGR_REFORGER_NORMAL_PREVIEW=1
WGR_REFORGER_TANGENT_PREVIEW=1
```

Focused validation covers partial-frame seams, winding, sorting, blob round trips, stale producer decline, cache key invalidation, and section reference admission. Existing transform algebra tests continue to cover nonuniform and mirrored instances. Production importer, adapter, vertex table, mesh builder, retained registration and affected unit translation units are syntax checked. Installed-scene admission and visual acceptance remain an integration check; packed source axes are not claimed numerically equivalent to another renderer.
