#import layered_fog_optics::{FogConsumer}
@group(0) @binding(0) var volume: texture_3d<f32>;
@group(0) @binding(1) var volume_samp: sampler;
@group(0) @binding(2) var<uniform> packet: FogConsumer;
struct Output { @builtin(position) clip: vec4<f32>, @location(0) uv: vec2<f32>, };
@vertex fn vs_main(@builtin(vertex_index) i:u32)->Output {
    let uv=vec2<f32>(f32((i<<1u)&2u),f32(i&2u));
    var out:Output;out.clip=vec4<f32>(uv*vec2<f32>(2.0,-2.0)+vec2<f32>(-1.0,1.0),0.0,1.0);out.uv=uv;return out;
}
@fragment fn fs_main(in:Output)->@location(0) vec4<f32> {
    if(packet.control.x<0.5){return vec4<f32>(0.0,0.0,0.0,1.0);}
    let fog=textureSampleLevel(volume,volume_samp,vec3<f32>(in.uv,1.0),0.0);
    return fog; // RGB source radiance, alpha destination transmittance
}
