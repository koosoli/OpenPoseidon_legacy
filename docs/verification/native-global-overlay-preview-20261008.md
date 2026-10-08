# Optional linear global material overlay

This independently implemented material operation applies an opaque Multi material's dedicated global overlay after ordered layer composition and per-layer RGB tint, before overall surface tint and lighting. It does not use the generic macro slot.

For each sampled RGB channel `m`, the remap is `q = clamp(0.5 + (m - 0.5) * power, 0, 1)`, choosing black power below 0.5 and white power otherwise. Hard light combines `q` with composed linear RGB: `2 * base * q` below or at 0.5, and `1 - 2 * (1 - base) * (1 - q)` above 0.5. The raw texture alpha applies the same hard-light operation to composed roughness, independently of the RGB powers. It does not modify ambient occlusion.

The retained preview requires all of the following:

- Explicit default-off `WGR_REFORGER_GLOBAL_OVERLAY_PREVIEW=1`, alongside `WGR_REFORGER_PBR_PREVIEW=1`, `WGR_REFORGER_LAYER_ORDER_PREVIEW=1` and `WGR_REFORGER_SRGB_PREVIEW=1`.
- A supported opaque `MatPBRMulti` standard mask contract, explicit finite black/white powers in 0–16, a dedicated `_MCR.edds` reference, and `UVSrcGlobalMaps "UV set 2"` with an actual second UV stream.
- Identity authored per-layer roughness multipliers and tint alpha. Unknown overlay modes, extra global transforms, malformed values and unsupported mask configurations decline admission.
- An original DDS linear BC7 texture and successful ordered-layer plus sRGB BCR resource admission. Stale/missing handles and failed color-role admission disable the operation.

The default material suffix target is `Pub_Village_E_01_L_Ext.emat`. `WGR_REFORGER_GLOBAL_OVERLAY_MATERIAL` can narrow or change the suffix target; an empty value keeps the default target. An additional target still must pass every metadata/resource check. Source admission is logged separately from renderer binding admission (`native global overlay bindings: admittedSections`).

Keep the camera, lighting, exposure and all existing preview switches identical for A/B. Change only `WGR_REFORGER_GLOBAL_OVERLAY_PREVIEW` between 0 and 1. The existing installed scene harness remains the integration route; both runs must load the same scene and show positive renderer binding admission in the enabled run.

ABI25 appends a source handle and two powers, increasing `WgrModelMaterial` from 440 to 456 bytes. C/Rust assertions, canonical layout checks and the version handshake cover the change; EXE and DLL must be built/deployed together. The internal retained material stride grows from 416 to 432 bytes, mirrored by the shadow shader. Existing callers' zero initialization keeps the operation disabled.

The enabled, admitted color fragment adds one linear texture sample. There are no new varyings, vertices or global simulation. The default shader omits the helper/sample branch. The texture uses the existing bank, upload, source-handle capture and model snapshot lease; no duplicate pixel allocation or sRGB view is created for this data texture. Existing texture and retained-section budgets apply.

Synthetic tests execute the production WGSL helper with neutral/asymmetric/saturated inputs, independent alpha, and overlapping ordered masks followed by overlay. Parser tests cover metadata round trips and malformed/unsupported decline. Composer tests cover legacy/preview/tangent/overlay combinations, shadow layout and stage-interface bounds. The five-case production-function GPU readback passed without validation errors, as did parser roundtrip/decline tests, full Rust test metadata type checking, production C++ syntax and shader/layout composition. Runtime admission and visible acceptance remain integration checks.

The operation is retained-path only. Existing lighting, roughness-to-exponent conversion, reflected F0, global normal reconstruction, environment probes and weather remain the renderer's current implementations. This preview does not establish complete material or renderer equivalence.
