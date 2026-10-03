#import frame::{frame, reverse_z, fog_factor, apply_fog, ground_sky_reflection, interior_rain_reach, interior_rain_coverage, sky_irradiance, terrain_sun_shadow, cloud_sun_shadow}
#import shadow::shadow_strength
#import conform::{bare_surface_y, bare_surface_valid}
#import ground_puddles::ground_puddle_rain_factor
#import rain_water_reflections::rain_water_scene_reflection
#import frame::wet_soil_debug_mode
#import rain_water_body::rain_water_body_radiance
#import terrain_material::TerrainMaterial
#import rain_water_medium::{rain_water_soil_fraction, rain_water_medium_optics}
#import rain_water_optics::{rain_water_surface_optics, rain_water_standing_fraction, rain_water_impact_normal, rain_water_sun_specular}

struct RainWaterParams {
    domain: vec4<f32>, // absolute origin x/z, cell spacing, simulation time
    control: vec4<f32>, // width/height, actual liquid rain, enabled
    identity: vec4<u32>, // generation low/high and reserved; CPU-side cache key
};
@group(1) @binding(0) var grid: texture_2d<f32>;
@group(1) @binding(1) var<uniform> rainwater: RainWaterParams;
struct FineTileMask {
    counts: vec4<u32>,
    rects: array<vec4<f32>,8>,
};
@group(1) @binding(2) var fine_records: texture_2d<f32>;
@group(1) @binding(3) var<uniform> fine_tiles: FineTileMask;

// Prefix of the EXISTING terrain uniform, borrowed without a new public ABI.
struct MediumTerrainParams {
    world_origin:vec2<f32>,land_grid:f32,terrain_grid:f32,
    hm_width:u32,hm_height:u32,land_range:u32,data_scale:f32,
};
@group(3) @binding(0) var<uniform> medium_terrain:MediumTerrainParams;
@group(3) @binding(1) var medium_indices:texture_2d<u32>;
@group(3) @binding(2) var<storage,read> medium_materials:array<TerrainMaterial>;
@group(3) @binding(3) var<uniform> medium_control:vec4<u32>;

fn rain_water_actual_soil(world_xz:vec2<f32>)->f32 {
    // Integer IEEE exponent checks survive driver float fast-math assumptions.
    // Refuse NaN/Inf before arithmetic or float-to-index conversion can select
    // a valid material row from an invalid world coordinate.
    if (any((bitcast<vec2<u32>>(world_xz) & vec2<u32>(0x7f800000u)) ==
            vec2<u32>(0x7f800000u))) {return 0.0;}
    if (medium_control.x != 1u || any(medium_control.yz != rainwater.identity.xy) ||
        !(medium_terrain.land_grid > 0.0) || medium_terrain.land_grid >= 1000000.0) {return 0.0;}
    let cell=(world_xz-medium_terrain.world_origin)/medium_terrain.land_grid;
    let dimensions=textureDimensions(medium_indices);
    if (any((bitcast<vec2<u32>>(cell) & vec2<u32>(0x7f800000u)) ==
            vec2<u32>(0x7f800000u)) || any(cell<vec2<f32>(0.0)) ||
        any(cell>=vec2<f32>(dimensions)) ||
        any(dimensions != vec2<u32>(medium_terrain.land_range))) {return 0.0;}
    let entry=textureLoad(medium_indices,vec2<i32>(floor(cell)),0).x & 0x7fffu;
    if (entry >= arrayLength(&medium_materials)) {return 0.0;}
    let material=medium_materials[entry];
    return rain_water_soil_fraction(material.surface_count,material.puddle_flags,fract(cell));
}

fn fine_record(cell: u32, lane: u32) -> vec4<f32> {
    let i=cell*3u+lane;
    return textureLoad(fine_records,vec2<i32>(i32(i%1024u),i32(i/1024u)),0);
}
fn fine_owned(world_xz: vec2<f32>) -> bool {
    for (var i=0u;i<min(fine_tiles.counts.x,8u);i+=1u) {
        let rect=fine_tiles.rects[i];
        if (all(world_xz>=rect.xy) && all(world_xz<rect.zw)) {return true;}
    }
    return false;
}
fn fine_water_at(cell: u32, world_xz: vec2<f32>) -> vec4<f32> {
    let rect=fine_record(cell,0u);
    let corners=fine_record(cell,1u);
    let flow=fine_record(cell,2u);
    let uv=clamp((world_xz-rect.xy)/rect.z,vec2<f32>(0.0),vec2<f32>(1.0));
    var bed: f32;
    if (uv.x+uv.y<=1.0) {bed=corners.x+(corners.y-corners.x)*uv.x+(corners.z-corners.x)*uv.y;}
    else {bed=corners.w+(corners.z-corners.w)*(1.0-uv.x)+(corners.y-corners.w)*(1.0-uv.y);}
    // Fine head is horizontal. Average volume/area is never a local-depth proxy.
    return vec4<f32>(rect.w,max(rect.w-bed,0.0),flow.xy);
}

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

fn rain_water_raster_support_depth(water: vec4<f32>, bed: f32, sea_level: f32,
                                  raster_height: f32) -> f32 {
    // Coarse depth is bilinear, but the 2x2 subquad mesh is piecewise planar.
    // Certify the surface actually rasterized, not just a fresh analytic query.
    // 3cm is a conservative presentation approximation budget; legitimate
    // high-curvature coarse water can be clipped. Fine native heads are flat
    // and agree exactly. Never move water or alter conserved simulation mass.
    if (abs(raster_height-water.x) > 0.03 || raster_height <= bed+0.001) { return 0.0; }
    return rain_water_support_depth(water,bed,sea_level);
}

// Return straight-alpha colour for the existing ALPHA_BLENDING pipeline. The
// destination is the actual already-lit bed, so transmission needs no copied
// scene texture. A scalar extinction length of 1/3m gives shallow rainwater a
// visible bed and progressively obscures it in a deep, turbid pool. The existing
// body tint is scattering radiance, not an arbitrary grade on the terrain.
fn rain_water_optics(local_depth: f32, view_cosine: f32, reflected: vec3<f32>,
                     body: vec3<f32>, coverage: f32) -> vec4<f32> {
    return rain_water_surface_optics(local_depth,view_cosine,reflected,body,coverage);
}

struct VsOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) world: vec3<f32>,
    @location(1) @interpolate(flat) fine_cell: u32,
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
    out.fine_cell=0xffffffffu;
    out.world = vec3<f32>(world_xz.x, water.x, world_xz.y);
    out.clip = reverse_z(frame.proj * frame.view * vec4<f32>(out.world - frame.cam_pos.xyz, 1.0));
    return out;
}

@vertex
fn vs_fine(@builtin(vertex_index) vertex: u32, @location(0) cell: u32) -> VsOut {
    let corners=array<vec2<f32>,6>(vec2<f32>(0,0),vec2<f32>(0,1),vec2<f32>(1,0),
        vec2<f32>(1,0),vec2<f32>(0,1),vec2<f32>(1,1));
    let rect=fine_record(cell,0u);
    let world_xz=rect.xy+corners[vertex%6u]*rect.z;
    var out: VsOut;
    out.fine_cell=cell;
    out.world=vec3<f32>(world_xz.x,rect.w,world_xz.y);
    out.clip=reverse_z(frame.proj*frame.view*vec4<f32>(out.world-frame.cam_pos.xyz,1.0));
    return out;
}

@fragment
fn fs_main(in: VsOut, @builtin(front_facing) front: bool) -> @location(0) vec4<f32> {
    let dwx = dpdx(in.world);
    let dwy = dpdy(in.world);
    let footprint = max(length(dwx.xz),length(dwy.xz));
    var water: vec4<f32>;
    let is_fine=in.fine_cell!=0xffffffffu;
    if (is_fine) {
        if (in.fine_cell>=fine_tiles.counts.y) {discard;}
        water=fine_water_at(in.fine_cell,in.world.xz);
    } else {
        if (fine_owned(in.world.xz)) {discard;}
        water=water_at(in.world.xz);
    }
    if (!front || rainwater.control.w < 0.5 || water.y <= 0.001 ||
        !bare_surface_valid(in.world.xz)) { discard; }
    let bed = bare_surface_y(in.world.xz);
    // A simulation water surface must actually be above the authored ground,
    // not a lowland plane rendered through a hillside or a stale map.
    let local_depth = rain_water_raster_support_depth(water,bed,frame.ground_weather.z,in.world.y);
    if (local_depth <= 0.001) { discard; }
    let cover = ground_puddle_rain_factor(interior_rain_reach(in.world),interior_rain_coverage(in.world));
    let altitude = smoothstep(frame.snow_surface.y,
        frame.snow_surface.y + max(frame.snow_surface.z,0.001), in.world.y) * frame.snow_surface.w;
    let snow = max(frame.snow_surface.x, altitude);
    if (cover <= 0.0 || snow > 0.002) { discard; }

    let step = max(rainwater.domain.z,0.25);
    var hx=0.0;
    var hz=0.0;
    if (!is_fine) {
        hx=(water_at(in.world.xz + vec2<f32>(step,0)).x -
            water_at(in.world.xz - vec2<f32>(step,0)).x)/(2.0*step);
        hz=(water_at(in.world.xz + vec2<f32>(0,step)).x -
            water_at(in.world.xz - vec2<f32>(0,step)).x)/(2.0*step);
    }
    let ripple = rain_water_impact_normal(in.world.xz,rainwater.domain.w,rainwater.control.z,footprint);
    let standing = rain_water_standing_fraction(local_depth);
    // Long, gentle flow packets advect with the physical field velocity; fade
    // all fine variation before distant pixels can resolve glittering glints.
    let detail = 1.0 - smoothstep(0.15,0.7,footprint);
    let flow = water.zw;
    let advected = in.world.xz - flow * rainwater.domain.w;
    let flow_slope = vec2<f32>(sin(advected.x*1.7),cos(advected.y*1.4))
        * min(length(flow),2.0) * 0.016 * detail;
    let n = normalize(vec3<f32>(-hx + (ripple.x + flow_slope.x)*standing, 1.0,
                                -hz + (ripple.z + flow_slope.y)*standing));
    let view = normalize(frame.cam_pos.xyz-in.world);
    let normal_variation=max(length(dpdx(n)),length(dpdy(n)));
    var scene=vec4<f32>(0.0);
    if (standing > 0.0) {
        scene=rain_water_scene_reflection(in.world-frame.cam_pos.xyz,n,normal_variation,footprint);
    }
    let reflected = mix(ground_sky_reflection(reflect(-view,n)),scene.rgb,scene.a);
    let rel = in.world-frame.cam_pos.xyz;
    // Share actual visibility between direct body illumination and the solar
    // interface. Do not sample the shadow maps twice or dim the sky/reflection.
    var sun_visibility = 0.0;
    if (any(frame.sun_diffuse.rgb > vec3<f32>(0.0)) &&
        (frame.sun_diffuse.w <= 0.5 || dot(n,-frame.sun_dir_world.xyz) > 0.0)) {
        let fog = fog_factor(length(rel));
        let csm = shadow_strength(rel,vec3<f32>(0.0,1.0,0.0),fog,dwx,dwy);
        let terrain_shadow = terrain_sun_shadow(in.world.xz,in.world.y) * fog;
        sun_visibility = (1.0-max(csm,terrain_shadow)) * cloud_sun_shadow(in.world.xz);
    }
    var sky_body = vec3<f32>(0.0);
    if (frame.sun_diffuse.w > 0.5) { sky_body = sky_irradiance(n); }
    let body = rain_water_body_radiance(frame.sun_diffuse.w, sky_body, frame.sun_ambient.w,
        frame.sun_ambient.rgb, frame.sun_diffuse.rgb, max(dot(n,-frame.sun_dir_world.xyz),0.0), sun_visibility);
    let edge = smoothstep(0.001,0.012,min(water.y,local_depth));
    var optics = rain_water_optics(local_depth, dot(n,view), reflected, body, edge*cover);
    let soil=rain_water_actual_soil(in.world.xz);
    if (soil > 0.0) {
        optics=rain_water_medium_optics(local_depth,dot(n,view),reflected,body,edge*cover,soil);
    }
    var rgb = optics.rgb;
    // The shared atmosphere reflection bake excludes the sun disc. As in
    // Tidewater, add direct sun irradiance through a separately shadowed GGX
    // interface, not a brighter sky/body tint or a second Fresnel multiplier.
    let light = normalize(-frame.sun_dir_world.xyz);
    let half_vector = (light + view) / max(length(light + view), 0.0001);
    let sun_lobe = rain_water_sun_specular(max(dot(n,view),0.0), max(dot(n,light),0.0),
        max(dot(n,half_vector),0.0), max(dot(view,half_vector),0.0), normal_variation) * standing;
    if (sun_lobe > 0.0 && optics.a > 0.0) {
        let sun_light = frame.sun_diffuse.rgb * 3.14159265359
            * sun_visibility;
        rgb += sun_light * sun_lobe * edge * cover / max(optics.a,0.000001);
    }
    if (frame.params.fog_enabled >= 1.5) { rgb = apply_fog(rgb,in.world-frame.cam_pos.xyz); }
    else { rgb = mix(frame.fog_color.rgb,rgb,fog_factor(length(in.world-frame.cam_pos.xyz))); }
    // Actual surviving overlay only: retain all source/depth/shelter/snow,
    // front-face and alpha calculations. No witness replaces a discarded body.
    if (wet_soil_debug_mode() > 0u) { return vec4<f32>(1.0, 0.0, 1.0, optics.a); }
    return vec4<f32>(rgb,optics.a);
}
