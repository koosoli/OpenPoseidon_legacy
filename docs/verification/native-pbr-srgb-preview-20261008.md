# Optional native PBR sRGB color views

The retained opaque native PBR preview can optionally sample original BC7 sRGB BCR sources through hardware sRGB views. This decodes RGB before texture filtering, applies each linear layer tint, and blends layer RGB in linear space. Roughness alpha remains linear. The result is encoded once at the existing object-lighting handoff, whose shader decodes it again. Existing BRDF, normal, lighting and output approximations remain.

Enable `WGR_REFORGER_PBR_PREVIEW=1` and `WGR_REFORGER_SRGB_PREVIEW=1`; the latter defaults off. Optional `WGR_REFORGER_SRGB_MATERIAL` selects a case-insensitive material-path suffix. To isolate this change, keep all other preview switches, material filters, camera, exposure and time identical and toggle only `WGR_REFORGER_SRGB_PREVIEW`. A fresh process is required for each setting.

Admission requires existing opaque native PBR eligibility plus original BC7 sources explicitly marked sRGB in the EDDS DX10 header. Every active BCR layer must qualify and upload to its declared handle. Converted/composited/decoded sources, linear/typeless sources, cutouts and unsupported texture formats are declined. Renderer descriptor exhaustion also declines the entire section's sRGB preview. Source and renderer admission diagnostics distinguish these cases.

Each admitted unique BCR adds one compatible view/descriptor of the same texture allocation: no duplicate pixel or mip payload, no new sample or pass, and no public ABI/layout change. Descriptor allocation shares the existing object-slot cap. Four internal GPU material slots add16 bytes per material (368→384); original slots remain in use for residency feedback, alpha/prepasses and normal/data roles. Logical aliases survive leased-texture eviction, refresh on replacement/re-upload, and retire safely when their source lease ends. An incompatible replacement binds white rather than interpreting linear data as color.

Validation for this change:

- Production Rust source typecheck and retained/direct/shadow composition with native PBR enabled/disabled.
- Internal Rust/WGSL material offset/stride checks.
- Synthetic BC7 GPU readback demonstrating decode-before-filter rather than decode-after-filter, unchanged alpha, and unchanged original UNORM samples.
- Alias tests covering same storage, replacement-before-retire, failed upload, eviction/re-upload, stale handles/generations, lease release, unsupported formats and slot exhaustion.
- Synthetic EDDS metadata/block preservation and truncated-header checks; DDS source tests retain and reset transfer metadata across preparation modes.

Full paired build, installed-game validation and visual acceptance are integration checks. This opt-in is not a claim of complete native material or renderer parity.
