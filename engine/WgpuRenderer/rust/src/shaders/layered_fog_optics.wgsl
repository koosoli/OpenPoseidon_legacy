#define_import_path layered_fog_optics

// Finite ASL slab with linear boundary feathers. Extinction is in 1/metre.
// The primitive integrates thin layers even when neither step endpoint lies in them.
fn fog_height_density(y: f32, layer: vec4<f32>) -> f32 {
    let feather = max(layer.z, 0.001);
    return clamp((y - layer.x + feather) / feather, 0.0, 1.0)
        * clamp((layer.y + feather - y) / feather, 0.0, 1.0);
}
fn fog_height_primitive(y: f32, layer: vec4<f32>) -> f32 {
    let f = max(layer.z, 0.001);
    let rise = clamp(y - (layer.x - f), 0.0, f);
    let plateau = clamp(y - layer.x, 0.0, layer.y - layer.x);
    let fall = clamp(y - layer.y, 0.0, f);
    return rise * rise / (2.0 * f) + plateau + fall - fall * fall / (2.0 * f);
}
fn fog_segment_density(y: f32, ray_y: f32, distance: f32, layer: vec4<f32>) -> f32 {
    if (distance <= 0.0 || layer.w <= 0.0) { return 0.0; }
    let delta = ray_y * distance;
    if (abs(delta) < 0.001) { return fog_height_density(y + 0.5 * delta, layer) * distance; }
    return max((fog_height_primitive(y + delta, layer) - fog_height_primitive(y, layer)) / ray_y, 0.0);
}
// A source sample must lie in the participating part of the analytic slab.
// .x is local distance; .y retains the same exact extinction integral.
fn fog_segment_participant(y: f32, ray_y: f32, distance: f32, layer: vec4<f32>) -> vec2<f32> {
    let integral = fog_segment_density(y, ray_y, distance, layer);
    if (integral <= 0.0) { return vec2<f32>(0.0); }
    if (abs(ray_y * distance) < 0.001) { return vec2<f32>(distance * 0.5, integral); }
    let feather = max(layer.z, 0.001);
    let a = (layer.x - feather - y) / ray_y;
    let b = (layer.y + feather - y) / ray_y;
    let start = clamp(min(a,b), 0.0, distance);
    let end = clamp(max(a,b), 0.0, distance);
    return vec2<f32>((start + end) * 0.5, integral);
}
// Clip a linear native-ground interval to air before choosing its density
// participant. A camera-ray step straddling the terrain must retain the air
// above it instead of sampling its buried midpoint and deleting the whole step.
fn fog_air_interval(agl: f32, slope: f32, distance: f32) -> vec2<f32> {
    if (distance <= 0.0) { return vec2<f32>(0.0); }
    if (abs(slope) < 1e-8) {
        return vec2<f32>(0.0, select(0.0, distance, agl > 0.05));
    }
    let crossing = clamp((0.05 - agl) / slope, 0.0, distance);
    if (slope < 0.0) { return vec2<f32>(0.0, crossing); }
    return vec2<f32>(crossing, distance - crossing);
}
// Inverse-CDF samples of the normalized HG density over the FULL sphere.
// The caller keeps equal weights, including zero for downward sky samples.
// Pairing azimuths gives antipodal directions in the isotropic limit.
fn fog_phase_sample(ray: vec3<f32>, g: f32, index: u32) -> vec3<f32> {
    let gg = clamp(g, -0.95, 0.95);
    let u = (f32(index) + 0.5) / 8.0;
    var cosine = 2.0 * u - 1.0;
    if (abs(gg) >= 0.001) {
        let q = (1.0 - gg * gg) / (1.0 - gg + 2.0 * gg * u);
        cosine = clamp((1.0 + gg * gg - q * q) / (2.0 * gg), -1.0, 1.0);
    }
    let pair = min(index, 7u - index);
    let angle = f32(pair) * 2.39996322973 + select(0.0, 3.14159265359, index >= 4u);
    let axis = select(vec3<f32>(0.0,1.0,0.0), vec3<f32>(1.0,0.0,0.0), abs(ray.y) > 0.99);
    let tangent = normalize(cross(axis,ray));
    let bitangent = cross(ray,tangent);
    let radius = sqrt(max(1.0 - cosine * cosine, 0.0));
    return normalize(ray * cosine + radius * (tangent * cos(angle) + bitangent * sin(angle)));
}
// Unit-solid-angle normalized Henyey-Greenstein, not an unnormalized highlight.
fn fog_phase(cosine: f32, g: f32) -> f32 {
    let gg = clamp(g, -0.95, 0.95);
    return (1.0 - gg * gg) / (12.56637061436 * pow(max(1.0 + gg * gg - 2.0 * gg * cosine, 0.0025), 1.5));
}
// HG convolution of raw upper-hemisphere sky radiance SH-9. Its degree-l
// coefficient is g^l, without Lambertian factors or hemisphere renormalization.
// This is a smooth, band-limited ambient approximation; direct sun/local lamps
// retain their full HG phase. No camera-relative sampling basis or horizon test.
fn fog_sky_ambient(ray: vec3<f32>, g: f32, c: array<vec4<f32>, 9>) -> vec3<f32> {
    let gg = clamp(g, -0.95, 0.95);
    let x=ray.x; let y=ray.y; let z=ray.z;
    let l0=c[0].rgb*0.282095;
    let l1=(c[1].rgb*y+c[2].rgb*z+c[3].rgb*x)*0.488603;
    let l2=c[4].rgb*(1.092548*x*y)+c[5].rgb*(1.092548*y*z)
        +c[6].rgb*(0.315392*(3.0*z*z-1.0))+c[7].rgb*(1.092548*x*z)
        +c[8].rgb*(0.546274*(x*x-y*y));
    return max(l0+gg*l1+gg*gg*l2,vec3<f32>(0.0));
}
fn fog_transmittance(tau: f32) -> f32 { return exp(-max(tau, 0.0)); }
// Owner-requested fog chroma policy, applied to radiance in linear light.
// Preserve luminance/HDR energy and original radiance exactly when dormant.
fn fog_neutral_radiance(radiance: vec3<f32>, strength: f32) -> vec3<f32> {
    if (strength <= 0.0) { return radiance; }
    let luminance = dot(radiance,vec3<f32>(0.2126,0.7152,0.0722));
    return mix(radiance,vec3<f32>(luminance),clamp(strength,0.0,1.0));
}
// Shared by visible horizon atmosphere and its synthetic far-fog closure.
// Original ray elevation is required, not the horizon-floored march direction.
fn fog_neutral_horizon(ray_y: f32, strength: f32) -> f32 {
    return strength * (1.0-smoothstep(0.0,0.30,ray_y));
}
// An infinite reversed-Z far point has w=0: its XYZ are a direction.
// Positive finite w changes magnitude only; negative w reverses direction.
// Scale before normalization so a tiny finite w cannot overflow a divide.
fn fog_ray_direction(point: vec4<f32>) -> vec3<f32> {
    let exponent = vec4<u32>(0x7f800000u);
    if (any((bitcast<vec4<u32>>(point) & exponent) == exponent)) { return vec3<f32>(0.0); }
    let scale = max(max(abs(point.x),abs(point.y)),abs(point.z));
    if (scale <= 1e-20) { return vec3<f32>(0.0); }
    return normalize(point.xyz / scale) * select(1.0,-1.0,point.w < 0.0);
}
// Above all finite slabs, a downward ray reaches their actual lower world plane.
// A spherical maximum truncates grazing columns and paints a camera-centred ring.
// Producer and consumers share this mapping; no extra slices or march samples.
fn fog_ray_far(base: f32, eye_y: f32, ray_y: f32, floor_y: f32, aerial: f32) -> f32 {
    if(aerial <= 0.0 || ray_y >= -0.00001){return base;}
    // The final32-slice texel represents (31.5/32)^2 of the range.
    // Place its endpoint beyond the lower plane, not3% before the column ends.
    let complete=max((eye_y-floor_y)/(-ray_y)/0.96899414,base);
    return mix(base,complete,aerial);
}
// CPU projection is forward-Z; raster depth is reversed by frame::reverse_z.
// Sky depth0 and an infinite homogeneous point consume the final fog slice.
fn fog_depth_distance(inverse: mat4x4<f32>, uv: vec2<f32>, depth: f32, far: f32) -> f32 {
    if (depth <= 0.0) { return far; }
    let ndc = uv * vec2<f32>(2.0,-2.0) + vec2<f32>(-1.0,1.0);
    let point = inverse * vec4<f32>(ndc,1.0-depth,1.0);
    let exponent = vec4<u32>(0x7f800000u);
    if (any((bitcast<vec4<u32>>(point) & exponent) == exponent) || abs(point.w) <= 1e-20) { return far; }
    // This conservative component bound avoids an overflowing length/divide.
    if (max(max(abs(point.x),abs(point.y)),abs(point.z)) > far*abs(point.w)) { return far; }
    return min(length(point.xyz / point.w),far);
}
fn fog_transport(background: vec3<f32>, transport: vec4<f32>) -> vec3<f32> {
    return background * transport.a + transport.rgb;
}
// Legacy far-cover is synthetic sky closure, not radiance emitted at the
// object's depth. Transport only the admitted foreground locally; the closing
// sky term uses the same final volume slice as the background sky pass.
fn fog_legacy_closure(scene: vec3<f32>, airlight: vec3<f32>, amount: f32,
    local: vec4<f32>, distant: vec4<f32>, surface_open: bool) -> vec3<f32> {
    let foreground = select(scene, fog_transport(scene, local), surface_open);
    // Explicit endpoints avoid FMix/FMA cancellation exposing a one-ULP edge.
    if (amount >= 1.0) { return fog_transport(airlight, distant); }
    if (amount <= 0.0) { return foreground; }
    return mix(foreground, fog_transport(airlight, distant), amount);
}
// Rain access at a wall, rock or canopy is not access to intervening air.
// Roof/native exclusion belongs to the encoded ray, not its opaque endpoint.
// The receiver may lie beyond the weather-map rectangle while its camera ray
// traverses certified air. Endpoint coverage cannot erase that transport.
// Explicitly excluded cockpit/interior consumers still stay closed.
fn fog_surface_open(coverage: f32, receiver: bool) -> bool {
    return receiver;
}

// Shared packet layout. Camera matching prevents a cockpit/reflection camera from
// sampling a volume encoded for the main scene. OFF/invalid reads no volume texture.
struct FogConsumer {
    view: mat4x4<f32>,
    proj: mat4x4<f32>,
    inv_vp: mat4x4<f32>,
    camera: vec4<f32>, // xyz, lowest certified fog plane ASL
    control: vec4<f32>, // ready, far distance, physical weather fog, aerial range blend
    solar_domain: vec4<f32>, // world xz origin, inverse width, maximum AGL
    native: vec4<f32>, // native xz origin, grid, ready
    native_size: vec4<u32>, // source texture width/height, reserved
};
// Same ray mapping for the cloud compositor, unprojected once per pixel.
fn fog_depth_fraction(packet: FogConsumer, uv: vec2<f32>, depth: f32) -> f32 {
    let ndc=uv*vec2<f32>(2.0,-2.0)+vec2<f32>(-1.0,1.0);
    let point=packet.inv_vp*vec4<f32>(ndc,1.0-max(depth,0.0),1.0);
    let ray=fog_ray_direction(point);
    let far=fog_ray_far(packet.control.y,packet.camera.y,ray.y,packet.camera.w,packet.control.w);
    if(depth<=0.0 || abs(point.w)<=1e-20 || dot(ray,ray)<0.5){return 1.0;}
    if(max(max(abs(point.x),abs(point.y)),abs(point.z))>far*abs(point.w)){return 1.0;}
    return clamp(length(point.xyz/point.w)/far,0.0,1.0);
}
fn fog_camera_matches(packet: FogConsumer, view: mat4x4<f32>, proj: mat4x4<f32>, camera: vec3<f32>) -> bool {
    if (packet.control.x < 0.5 || packet.control.y <= 0.0) { return false; }
    if (any(packet.camera.xyz != camera)) { return false; }
    for (var i = 0u; i < 4u; i = i + 1u) {
        if (any(packet.view[i] != view[i]) || any(packet.proj[i] != proj[i])) { return false; }
    }
    return true;
}

// Smooth fixed world-space value noise. No camera position, frame or time enters
// the field; adjacent cells have matching values and zero boundary derivatives.
fn fog_noise_hash(cell: vec3<i32>) -> f32 {
    let c = bitcast<vec3<u32>>(cell);
    var h = c.x * 1597334677u ^ c.y * 3812015801u ^ c.z * 2798796415u;
    h = (h ^ (h >> 16u)) * 2246822519u;
    h = (h ^ (h >> 13u)) * 3266489917u;
    return f32((h ^ (h >> 16u)) & 0x00ffffffu) / 16777215.0;
}
fn fog_world_noise(p: vec3<f32>) -> f32 {
    let cell = vec3<i32>(floor(p));
    let q = fract(p); let f = q*q*(vec3<f32>(3.0)-2.0*q);
    let a = mix(fog_noise_hash(cell),fog_noise_hash(cell+vec3<i32>(1,0,0)),f.x);
    let b = mix(fog_noise_hash(cell+vec3<i32>(0,1,0)),fog_noise_hash(cell+vec3<i32>(1,1,0)),f.x);
    let c = mix(fog_noise_hash(cell+vec3<i32>(0,0,1)),fog_noise_hash(cell+vec3<i32>(1,0,1)),f.x);
    let d = mix(fog_noise_hash(cell+vec3<i32>(0,1,1)),fog_noise_hash(cell+vec3<i32>(1,1,1)),f.x);
    return mix(mix(a,b,f.y),mix(c,d,f.y),f.z);
}
fn fog_patch_density(p: vec3<f32>, patchiness: vec4<f32>) -> f32 {
    if (patchiness.x <= 0.0) { return 1.0; }
    let q = p / vec3<f32>(patchiness.y,patchiness.z,patchiness.y);
    let noise = (2.0*fog_world_noise(q)+fog_world_noise(q*2.0+vec3<f32>(17.0,31.0,43.0)))/3.0;
    return 1.0 + patchiness.x*(2.0*noise-1.0);
}
// Concavity is a bounded pooling proxy, not a fluid simulation. A broad native
// bowl deepens fog; flat ground and convex ridges do not gain density.
fn fog_valley_density(ground: f32, neighbours: vec4<f32>, strength: f32, radius: f32) -> f32 {
    if (strength <= 0.0) { return 1.0; }
    let concavity = max(dot(neighbours,vec4<f32>(0.25))-ground,0.0);
    return 1.0 + strength*clamp(concavity/max(radius*0.1,1.0),0.0,1.0);
}
fn fog_domain_feather(distance: f32, width: f32) -> f32 {
    if (width <= 0.0) { return 1.0; }
    return smoothstep(0.0,width,max(distance,0.0));
}
