struct Globals {
    screen: vec2<f32>,
    _pad: vec2<f32>,
    fog_color: vec4<f32>,
    // SMK-037 soft particles: x = fade distance in metres, y = 1 when the snapshot bound
    // at group(3) is valid this frame, zw = the RENDER target size in pixels. Not
    // `screen`: that is the swapchain size the engine projected its 2D coordinates into,
    // and under render scale the scene target -- which is what @builtin(position) and the
    // depth snapshot are measured in -- is a different size entirely.
    soft: vec4<f32>,
    // Spare, kept so `inv_view_proj` stays 16-byte aligned at a fixed offset.
    _spare: vec4<f32>,
    // Camera-relative (translation-free) inverse view-projection, as the god rays use.
    inv_view_proj: mat4x4<f32>,
};

@group(0) @binding(0) var<uniform> globals: Globals;

@group(1) @binding(0) var tex: texture_2d<f32>;
@group(2) @binding(0) var samp: sampler;

struct VsOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) color: vec4<f32>,
    @location(2) fog: f32,
    @location(3) @interpolate(flat) glow: f32,
};

@vertex
fn vs_main(
    @location(0) pos: vec3<f32>,      // x,y = pixels, z = depth
    @location(1) rhw_fog: vec2<f32>,  // rhw, fog
    @location(2) uv: vec2<f32>,
    @location(3) color: vec4<f32>,
) -> VsOut {
    var out: VsOut;
    // Pixel space (top-left, y down) -> NDC (centre, y up).
    let ndc = vec2<f32>(
        pos.x / globals.screen.x * 2.0 - 1.0,
        1.0 - pos.y / globals.screen.y * 2.0,
    );
    // Pre-multiply by w so the rasteriser interpolates perspective-correctly
    // (matches GL33's VSScreen). Plain 2D passes z=0, rhw=1 -> w=1, depth 0.
    let w = 1.0 / rhw_fog.x;
    // Reversed-Z to match the 3D pipelines (near->1, far->0; GreaterEqual, clear 0).
    // Plain 2D passes z=0 -> reversed 1.0 (nearest), so it always wins the depth test.
    out.clip = vec4<f32>(ndc * w, (1.0 - pos.z) * w, w);
    out.uv = uv;
    // color is in BGRA order -> swizzle to RGBA
    out.color = color.zyxw;
    out.fog = rhw_fog.y;
    out.glow = select(0.0, rhw_fog.y - 1.0, rhw_fog.y >= 2.0);
    if (rhw_fog.y < 0.0) {
        out.color = vec4<f32>(out.color.rgb * -rhw_fog.y, out.color.a);
        out.fog = 1.0;
    }
    return out;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    let base = textureSample(tex, samp, in.uv) * in.color;
    if (in.glow > 0.0) {
        let p = in.uv * 2.0 - 1.0;
        let r2 = dot(p,p);
        let edge = max(1.0-r2,0.0);
        let profile = exp2(-6.0*r2) * edge * edge;
        let gain = select(1.0,8.0,in.glow > 1.5);
        return vec4<f32>(in.color.rgb * gain, in.color.a * profile);
    }
    // Blend toward the scene fog colour (matches GL33's mix(fogColor, r0, vFogTC)).
    let rgb = mix(globals.fog_color.rgb, base.rgb, clamp(in.fog, 0.0, 1.0));
    return vec4<f32>(rgb, base.a);
}

// SMK-037: the soft-particle variant, used only by batches the engine submitted with
// WGR_DEPTH_TEST_SOFT -- which today is exactly the cloudlet decals, and only while the
// Smoke tab's soft-particle lever is on. Everything else, UI included, keeps fs_main and
// its three-bind-group layout untouched.
@group(3) @binding(0) var scene_depth: texture_2d<f32>;

@fragment
fn fs_soft(in: VsOut) -> @location(0) vec4<f32> {
    let base = textureSample(tex, samp, in.uv) * in.color;
    let rgb = mix(globals.fog_color.rgb, base.rgb, clamp(in.fog, 0.0, 1.0));

    var soft: f32 = 1.0;
    if (globals.soft.y > 0.5) {
        let texel = vec2<i32>(i32(in.clip.x), i32(in.clip.y));
        // Raw reversed-Z device depth as the snapshot stored it: 1 = near plane, 0 = far
        // plane and sky.
        let d = textureLoad(scene_depth, texel, 0).r;
        if (d > 1.0e-6) {
            let render = max(globals.soft.zw, vec2<f32>(1.0, 1.0));
            let ndc = vec2<f32>(
                in.clip.x / render.x * 2.0 - 1.0,
                1.0 - in.clip.y / render.y * 2.0,
            );
            // BOTH depths go through the SAME unprojection. `in.clip.z` after the
            // perspective divide is this sprite's own device depth -- the very value the
            // depth test compared against `d` -- so the two come back in one space with no
            // assumption about what units the engine's `rhw` was in. The obvious cheaper
            // route (sprite eye depth = 1/rhw) does make that assumption, and it is wrong
            // here: Object::DrawDecal builds rhw from `ScaledInvTransform`, i.e. a SCALED
            // camera space, so the two sides were metres against something else entirely
            // and every sprite came out fully faded.
            //
            // Reversed-Z: forward NDC z is 1 - stored, exactly as the god-ray march does it.
            let hit = globals.inv_view_proj * vec4<f32>(ndc, 1.0 - d, 1.0);
            let own = globals.inv_view_proj * vec4<f32>(ndc, 1.0 - in.clip.z, 1.0);
            // Distance along the view ray, not eye depth: both points sit on the SAME ray,
            // so their difference is the eye-depth difference over cos(angle from centre) --
            // at most ~15% long at a screen corner, which for a soft fade is nothing, and it
            // costs no camera-forward vector to get wrong the sign of.
            let sep = length(hit.xyz / hit.w) - length(own.xyz / own.w);
            // Full strength `fade` metres in front of the surface, nothing at it. A sprite
            // that has drifted BEHIND the surface clamps to zero rather than letting a
            // negative separation brighten anything.
            soft = clamp(sep / max(globals.soft.x, 0.01), 0.0, 1.0);
        }
    }
    return vec4<f32>(rgb, base.a * soft);
}
