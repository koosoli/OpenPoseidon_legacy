struct Globals {
    screen: vec2<f32>,
    _pad: vec2<f32>,
    fog_color: vec4<f32>,
};

@group(0) @binding(0) var<uniform> globals: Globals;

@group(1) @binding(0) var tex: texture_2d<f32>;
@group(2) @binding(0) var samp: sampler;

struct VsOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) color: vec4<f32>,
    @location(2) fog: f32,
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
    return out;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    let base = textureSample(tex, samp, in.uv) * in.color;
    // Blend toward the scene fog colour (matches GL33's mix(fogColor, r0, vFogTC)).
    let rgb = mix(globals.fog_color.rgb, base.rgb, clamp(in.fog, 0.0, 1.0));
    return vec4<f32>(rgb, base.a);
}
