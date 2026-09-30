#import frame::{frame, reverse_z, fog_factor, apply_fog, ground_sky_reflection, interior_rain_reach, interior_rain_coverage}
#import conform::{bare_surface_y, bare_surface_valid}
#import ground_puddles::{ground_puddle_rain_factor, ground_puddle_ripple_normal}

struct RainWaterParams {
    domain: vec4<f32>, // absolute origin x/z, cell spacing, simulation time
    control: vec4<f32>, // width/height, actual liquid rain, enabled
    identity: vec4<u32>, // generation low/high and reserved; CPU-side cache key
};
@group(1) @binding(0) var grid: texture_2d<f32>;
@group(1) @binding(1) var<uniform> rainwater: RainWaterParams;

fn water_at(world_xz: vec2<f32>) -> vec4<f32> {
    let t = (world_xz - rainwater.domain.xy) / rainwater.domain.z;
    let dims = textureDimensions(grid);
    let bounded = clamp(t, vec2<f32>(0.0), vec2<f32>(dims) - vec2<f32>(1.001));
    let b = vec2<i32>(floor(bounded));
    let f = fract(bounded);
    let a = textureLoad(grid,b,0);
    let c = textureLoad(grid,b+vec2<i32>(1,0),0);
    let d = textureLoad(grid,b+vec2<i32>(0,1),0);
    let e = textureLoad(grid,b+vec2<i32>(1,1),0);
    let water = mix(mix(a,c,f.x),mix(d,e,f.x),f.y);
    // Landscape and RainWaterField use the anti-diagonal 00/10/01,10/11/01.
    // Only depth/flow are bilinear: a saddle's bed must not become a floating
    // bilinear surface between the authored terrain triangles.
    let y00 = a.x-a.y;
    let y01 = c.x-c.y;
    let y10 = d.x-d.y;
    let y11 = e.x-e.y;
    var bed: f32;
    if (f.x+f.y <= 1.0) {
        bed = y00+(y01-y00)*f.x+(y10-y00)*f.y;
    } else {
        bed = y11+(y10-y11)*(1.0-f.x)+(y01-y11)*(1.0-f.y);
    }
    return vec4<f32>(bed+water.y,water.y,water.zw);
}

fn rain_water_support_depth(water: vec4<f32>, bed: f32, sea_level: f32) -> f32 {
    if (water.y <= 0.001 || water.x <= sea_level + 0.02 ||
        abs((water.x - water.y) - bed) > 0.15) { return 0.0; }
    return max(0.0,min(water.y,water.x - bed));
}

struct VsOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) world: vec3<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) vertex: u32, @location(0) cell: u32) -> VsOut {
    // Four subquads keep the authoritative anti-diagonal bed and approximate
    // its smoothly interpolated water depth. Every subquad uses that diagonal.
    let corners = array<vec2<f32>,6>(vec2<f32>(0,0),vec2<f32>(0,1),vec2<f32>(1,0),
        vec2<f32>(1,0),vec2<f32>(0,1),vec2<f32>(1,1));
    let width = u32(rainwater.control.x);
    let base = vec2<f32>(f32(cell % width), f32(cell / width));
    let quad = vertex / 6u;
    let local = (vec2<f32>(f32(quad % 2u), f32(quad / 2u)) + corners[vertex % 6u]) * 0.5;
    let world_xz = rainwater.domain.xy + (base + local) * rainwater.domain.z;
    let water = water_at(world_xz);
    var out: VsOut;
    out.world = vec3<f32>(world_xz.x, water.x, world_xz.y);
    out.clip = reverse_z(frame.proj * frame.view * vec4<f32>(out.world - frame.cam_pos.xyz, 1.0));
    return out;
}

@fragment
fn fs_main(in: VsOut, @builtin(front_facing) front: bool) -> @location(0) vec4<f32> {
    let dwx = dpdx(in.world);
    let dwy = dpdy(in.world);
    let footprint = max(length(dwx.xz),length(dwy.xz));
    let water = water_at(in.world.xz);
    if (!front || rainwater.control.w < 0.5 || water.y <= 0.001 ||
        !bare_surface_valid(in.world.xz)) { discard; }
    let bed = bare_surface_y(in.world.xz);
    // A simulation water surface must actually be above the authored ground,
    // not a lowland plane rendered through a hillside or a stale map.
    let local_depth = rain_water_support_depth(water,bed,frame.ground_weather.z);
    if (local_depth <= 0.001) { discard; }
    let cover = ground_puddle_rain_factor(interior_rain_reach(in.world),interior_rain_coverage(in.world));
    let altitude = smoothstep(frame.snow_surface.y,
        frame.snow_surface.y + max(frame.snow_surface.z,0.001), in.world.y) * frame.snow_surface.w;
    let snow = max(frame.snow_surface.x, altitude);
    if (cover <= 0.0 || snow > 0.002) { discard; }

    let step = max(rainwater.domain.z,0.25);
    let hx = (water_at(in.world.xz + vec2<f32>(step,0)).x -
              water_at(in.world.xz - vec2<f32>(step,0)).x) / (2.0*step);
    let hz = (water_at(in.world.xz + vec2<f32>(0,step)).x -
              water_at(in.world.xz - vec2<f32>(0,step)).x) / (2.0*step);
    let ripple = ground_puddle_ripple_normal(in.world.xz,rainwater.domain.w,rainwater.control.z,footprint);
    // Long, gentle flow packets advect with the physical field velocity; fade
    // all fine variation before distant pixels can resolve glittering glints.
    let detail = 1.0 - smoothstep(0.15,0.7,footprint);
    let flow = water.zw;
    let advected = in.world.xz - flow * rainwater.domain.w;
    let flow_slope = vec2<f32>(sin(advected.x*1.7),cos(advected.y*1.4))
        * min(length(flow),2.0) * 0.016 * detail;
    let n = normalize(vec3<f32>(-hx + ripple.x + flow_slope.x, 1.0,
                                -hz + ripple.z + flow_slope.y));
    let view = normalize(frame.cam_pos.xyz-in.world);
    let fresnel = 0.025 + 0.975*pow(1.0-clamp(dot(n,view),0.0,1.0),5.0);
    let reflected = ground_sky_reflection(reflect(-view,n));
    let body = vec3<f32>(0.045,0.060,0.052) * (frame.sun_ambient.rgb + frame.sun_diffuse.rgb*0.2);
    var rgb = mix(body,reflected,clamp(fresnel+0.18,0.0,1.0));
    if (frame.params.fog_enabled >= 1.5) { rgb = apply_fog(rgb,in.world-frame.cam_pos.xyz); }
    else { rgb = mix(frame.fog_color.rgb,rgb,fog_factor(length(in.world-frame.cam_pos.xyz))); }
    let edge = smoothstep(0.001,0.012,min(water.y,local_depth));
    return vec4<f32>(rgb,edge*cover*clamp(0.3+fresnel*0.65,0.0,0.95));
}
