// Scene-referred underwater compositor plus its frustum-aligned light volume and
// FFT-derived caustic field. The compute work is dispatched only while the camera
// is in the displaced-waterline activation band.

use bytemuck::Zeroable;
use wgpu::util::DeviceExt;

const FROXEL_W: u32 = 32;
const FROXEL_H: u32 = 18;
const FROXEL_D: u32 = 32;
const CAUSTIC_SIZE: u32 = 128;
const VOLUME_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float;

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct Params {
    // x = time, y = camera height over the local surface, z = volume range, w = extinction.
    time_height_range_ext: [f32; 4],
    // xyz = absolute camera position, w = active FFT cascade count.
    camera_pos_layers: [f32; 4],
    // xyz = direction from the surface to the sun, w = water debug-view selector.
    sun_dir_debug: [f32; 4],
    // xyz = scene-linear direct sunlight, w unused.
    sun_radiance: [f32; 4],
    // inv(view) * inv(proj), matching Frame.inv_view_proj.
    inv_view_proj: [f32; 16],
    // Authored water colours are gamma-space.
    shallow_color: [f32; 4],
    deep_color: [f32; 4],
    cascade_lengths: [f32; 4],
    // x = de-tile warp amplitude, y = sea level, z = wave scale, w reserved.
    water_controls: [f32; 4],
    // Live Water-tab underwater tuning: x = absorption density multiplier, y = colour bias
    // (1 = hue from the authored deep swatch, 0 = the old neutral curve), z = caustic gain,
    // w reserved.
    underwater_tuning: [f32; 4],
    // WRL-006: camera body's containment ellipse (cx, cz, rx, rz); rx == 0 = the sea.
    body_ellipse: [f32; 4],
    // (cos, sin, 0, 0) of the body's heading.
    body_frame: [f32; 4],
}

const _: () = assert!(std::mem::size_of::<Params>() == 240);

#[test]
fn underwater_wgsl_validates() {
    // All three passes #import the shared water optics (and the FFT lookup), so they
    // validate through the same composer the runtime builds them with.
    for (name, source) in [
        ("underwater.wgsl", include_str!("underwater.wgsl")),
        (
            "underwater_froxel.wgsl",
            include_str!("underwater_froxel.wgsl"),
        ),
        (
            "underwater_caustics.wgsl",
            include_str!("underwater_caustics.wgsl"),
        ),
    ] {
        let mut composer = crate::shaders::build_composer();
        composer
            .make_naga_module(naga_oil::compose::NagaModuleDescriptor {
                source,
                file_path: name,
                ..Default::default()
            })
            .unwrap_or_else(|e| panic!("{name}: {}", e.emit_to_string(&composer)));
    }
}

#[test]
fn underwater_refraction_rejects_foreground_depth_leaks() {
    let shader = include_str!("underwater.wgsl");
    assert!(shader.contains("let warp_limit = 3.0 / dims_f"));
    assert!(shader.contains("let use_warp = warped_depth <= base_depth + 0.001"));
    assert!(shader.contains("fn water_path_length"));
    // Absorption hue is derived from the authored deep swatch, not a fixed curve, so the
    // volume is the same substance as the surface. The 0.1216 normaliser is the mean of
    // the curve it replaced and is what keeps the density where it was tuned.
    // WRL-002: no private extinction derivation anywhere in the underwater passes. All
    // three go through water_optics::water_extinction, the function the surface uses.
    for (name, text) in [
        ("underwater.wgsl", shader),
        ("underwater_froxel.wgsl", include_str!("underwater_froxel.wgsl")),
    ] {
        assert!(
            text.contains("water_extinction(params.deep_color"),
            "{name} must take its extinction from water_optics"
        );
        assert!(!text.contains("-log("), "{name} carries a private absorption derivation");
        assert!(!text.contains("0.280"), "{name} carries a private neutral curve");
    }
    let froxel = include_str!("underwater_froxel.wgsl");
    assert!(froxel.contains("params.underwater_tuning.y"));
    assert!(shader.contains("underwater_froxel"));
    assert!(!shader.contains("pow(deep_linear"));
}

#[test]
fn underwater_volume_is_shadowed_and_caustics_follow_fft() {
    let froxel = include_str!("underwater_froxel.wgsl");
    assert!(froxel.contains("terrain_occlusion"));
    assert!(froxel.contains("csm_occlusion"));
    assert!(froxel.contains("froxel_out"));
    let caustics = include_str!("underwater_caustics.wgsl");
    assert!(caustics.contains("fft_dynamics"));
    // The FFT lookup is imported, never copied: a copy is a waterline that drifts.
    for (name, text) in [("underwater.wgsl", include_str!("underwater.wgsl")), ("underwater_caustics.wgsl", caustics)] {
        assert!(text.contains("#import water_fft_sampling::fft_aperiodic_uv") ||
                text.contains("#import water_fft_sampling::fft_scaled_uv"), "{name}");
        assert!(!text.contains("fn fft_aperiodic_uv"), "{name} still carries a private fft_aperiodic_uv");
    }
    assert!(caustics.contains("fft_auxiliary"));
    assert!(caustics.contains("fft_aperiodic_uv"));
}

/// Mirrors of the WGSL constants of the same name. `underwater_waterline_follows_the_wavy_surface`
/// asserts the shader still declares these values, so the two cannot drift apart silently.
#[cfg(test)]
const WATERLINE_PROBE_M: f32 = 48.0;
#[cfg(test)]
const WATERLINE_STEPS: i32 = 12;

#[test]
fn underwater_waterline_follows_the_wavy_surface() {
    let shader = include_str!("underwater.wgsl");
    assert!(shader.contains(&format!(
        "const WATERLINE_PROBE_M: f32 = {WATERLINE_PROBE_M:.1};"
    )));
    assert!(shader.contains(&format!("const WATERLINE_STEPS: i32 = {WATERLINE_STEPS};")));
    // The compositor must locate the surface itself. If these disappear it has gone back
    // to the flat plane at sea_level, which is the bug this pass exists to fix.
    assert!(shader.contains("fn surface_height_at"));
    assert!(shader.contains("fn ray_above_surface"));
    assert!(shader.contains("fn wavy_water_path_length"));
    assert!(shader.contains("fft_displacement"));
    assert!(shader.contains("var path_m = wavy_water_path_length("));
    // WRL-006: the wet path stops at the camera body's side.
    assert!(shader.contains("path_m = min(path_m, max(t_exit, 0.0));"));
    // The stride must stay under the shortest cascade that carries displacement, or a
    // crest can pass between two samples unseen.
    assert!(WATERLINE_PROBE_M / WATERLINE_STEPS as f32 <= 20.0);
    // Detail-only cascades carry no displacement; summing them would lift the compositor's
    // surface above the drawn one.
    assert!(shader.contains("if (raw_length < 20.0)"));
}

/// `water_path_length` from the shader: one flat plane, one answer for the whole screen.
#[cfg(test)]
fn flat_path(ray: [f32; 2], target_distance: f32, cam_above: f32) -> f32 {
    if cam_above >= 0.0 {
        if ray[1] >= -1e-4 {
            return 0.0;
        }
        let entry = cam_above / (-ray[1]).max(1e-4);
        return (target_distance - entry).max(0.0);
    }
    if ray[1] > 1e-4 {
        let exit = -cam_above / ray[1];
        return target_distance.min(exit.max(0.0));
    }
    target_distance
}

/// `wavy_water_path_length` from the shader, over an analytic surface instead of the FFT
/// texture. Kept in step with the WGSL by the string assertions above; this is what pins
/// the behaviour, because a string match cannot tell a working waterline from a broken
/// one. `ray` is (horizontal, vertical) in the XY plane; `surface` is height vs x.
#[cfg(test)]
fn wavy_path(
    eye: [f32; 2],
    ray: [f32; 2],
    target_distance: f32,
    surface: impl Fn(f32) -> f32,
) -> f32 {
    let above_at = |t: f32| (eye[1] + ray[1] * t) - surface(eye[0] + ray[0] * t);
    let eye_above = above_at(0.0);
    if eye_above >= 0.0 {
        return 0.0;
    }
    let probe = target_distance.min(WATERLINE_PROBE_M);
    let step = probe / WATERLINE_STEPS as f32;
    let mut prev_t = 0.0_f32;
    let mut prev_f = eye_above;
    for i in 1..=WATERLINE_STEPS {
        let t = i as f32 * step;
        let f = above_at(t);
        if f >= 0.0 {
            let denom = prev_f - f;
            let safe = if denom.abs() < 1e-5 { -1e-5 } else { denom };
            return prev_t + (t - prev_t) * (prev_f / safe).clamp(0.0, 1.0);
        }
        prev_t = t;
        prev_f = f;
    }
    probe + flat_path(ray, (target_distance - probe).max(0.0), prev_f)
}

#[test]
fn a_crest_overhead_submerges_an_eye_the_flat_plane_calls_dry() {
    // 1 m amplitude swell, 20 m wavelength, sea level 0.
    let amplitude = 1.0_f32;
    let k = std::f32::consts::TAU / 20.0;
    let surface = |x: f32| amplitude * (x * k).sin();

    // Oliver's first requirement. The eye is exactly on the flat datum with a crest a
    // metre overhead, looking up. The flat plane compares 0.0 against 0.0, decides the eye
    // is dry, and returns no water at all for an upward ray.
    let crest_x = 5.0; // sin(pi/2) = 1
    assert!((surface(crest_x) - amplitude).abs() < 1e-3);
    let up = [0.0, 1.0];
    assert_eq!(flat_path(up, 120.0, 0.0), 0.0);

    let wet = wavy_path([crest_x, 0.0], up, 120.0, surface);
    assert!(
        wet > 0.5,
        "an eye under a crest must have water in front of it, got {wet} m"
    );

    // A trough overhead must NOT turn the effect on — otherwise this trades one wrong
    // answer for another.
    let trough_x = 15.0; // sin(3pi/2) = -1
    let dry = wavy_path([trough_x, 0.0], up, 120.0, surface);
    assert_eq!(
        dry, 0.0,
        "an eye above a trough must stay dry looking up, got {dry} m"
    );
}

#[test]
fn a_view_straddling_the_surface_splits_instead_of_answering_once() {
    let amplitude = 1.0_f32;
    let k = std::f32::consts::TAU / 20.0;
    let surface = |x: f32| amplitude * (x * k).sin();

    // Oliver's second requirement. The eye sits 30 cm under the mean surface at a zero
    // crossing, with a crest to one side and a trough to the other. Both rays leave it at
    // the same shallow upward angle, so the flat plane puts the surface the same distance
    // away in every direction: one answer for the whole screen, no waterline possible.
    let eye = [10.0_f32, -0.3];
    let toward_crest = [-1.0_f32, 0.02];
    let toward_trough = [1.0_f32, 0.02];
    let flat_crest = flat_path(toward_crest, 120.0, eye[1]);
    let flat_trough = flat_path(toward_trough, 120.0, eye[1]);
    assert_eq!(flat_crest, flat_trough);

    // Marching the waves, the two directions disagree by most of an order of magnitude —
    // and that disagreement is the waterline running across the frame.
    let into_crest = wavy_path(eye, toward_crest, 120.0, surface);
    let over_trough = wavy_path(eye, toward_trough, 120.0, surface);
    assert!(
        into_crest > 4.0 * over_trough,
        "the ray running under the oncoming crest must stay in water far longer than the \
         one surfacing through the trough, got {into_crest} m against {over_trough} m \
         (the flat plane says {flat_crest} m for both)"
    );
}

#[test]
fn an_eye_in_open_air_is_not_fogged_by_distant_crests() {
    // Reported from the game: "the underwater effect kicks in when I am 3-4 metres over
    // the water level." The first version of the march caused it. A camera above the sea
    // looking out is nearly horizontal, and a near-horizontal ray ALWAYS passes under some
    // crest eventually — so the march found a crossing a few tens of metres out and
    // returned most of the ray as water, fogging the whole sea and everything behind it.
    //
    // Above the surface you are looking AT the water, not through it; the water shader owns
    // those pixels. The march is now reserved for a submerged eye.
    let amplitude = 1.0_f32;
    let k = std::f32::consts::TAU / 20.0;
    let surface = |x: f32| amplitude * (x * k).sin();

    let eye = [0.0_f32, 1.4]; // inside UNDERWATER_NEAR_SURFACE_BAND, so the pass does run
    for ray in [
        [1.0_f32, -0.01_f32],
        [1.0, 0.0],
        [1.0, 0.01],
        [1.0, -0.05],
        [-1.0, -0.02],
    ] {
        let path = wavy_path(eye, ray, 120.0, surface);
        assert_eq!(
            path, 0.0,
            "a camera {} m above the sea looking along {ray:?} must see no water volume, \
             got {path} m",
            eye[1]
        );
    }
}

#[test]
fn a_submerged_eye_on_a_calm_sea_reproduces_the_plane_answer() {
    // The march must not invent or lose water: with the eye under a dead calm sea it has to
    // agree with the plane it replaces, or every existing underwater view changes.
    let calm = |_x: f32| 0.0_f32;
    for ray in [
        [0.0_f32, 1.0_f32],
        [0.9, 0.44],
        [1.0, 0.0],
        [0.0, -1.0],
        [0.7, -0.7],
    ] {
        let wavy = wavy_path([0.0, -5.0], ray, 120.0, calm);
        let flat = flat_path(ray, 120.0, -5.0);
        assert!(
            (wavy - flat).abs() < 0.05,
            "calm sea disagreement under water, ray {ray:?}: {wavy} vs {flat}"
        );
    }

    // Above the surface the two deliberately DISAGREE. The plane fogs everything past its
    // own crossing; the compositor no longer contributes above water at all, because the
    // water shader has already drawn those pixels and the depth target it would fog does
    // not even contain the surface.
    let looking_down = [0.7_f32, -0.7_f32];
    assert!(flat_path(looking_down, 120.0, 2.0) > 100.0);
    assert_eq!(wavy_path([0.0, 2.0], looking_down, 120.0, calm), 0.0);
}

#[test]
fn default_absorption_keeps_useful_midrange_visibility() {
    // Rust mirror of water_optics: sigma_t = hue * max(2.5 * clarity, 0.12) + 1.6 * clarity,
    // at the shipped clarity of 0.16 and the neutral hue (bias 0). The scattering term is
    // what the old surface shader called "seabed visibility"; the compositor now shares it.
    let clarity = 0.16_f32;
    let sigma_t = |hue: f32| hue * (clarity * 2.5).max(0.12) + clarity * 1.6;
    let transmission = |hue: f32, distance: f32| (-sigma_t(hue) * distance).exp();
    // Zero path is exactly one, monotone in distance.
    assert_eq!(transmission(0.280, 0.0), 1.0);
    assert!(transmission(0.065, 5.0) > transmission(0.065, 10.0));
    // Blue-green still carries a useful image at 5 m and red is gone well before green.
    assert!(transmission(0.020, 5.0) > 0.20);
    assert!(transmission(0.280, 5.0) < transmission(0.065, 5.0));
    // And the volume closes down: nothing of the seabed survives 40 m, whatever the channel.
    assert!(transmission(0.020, 40.0) < 0.05);
}

struct Volume {
    froxel_pipeline: wgpu::ComputePipeline,
    froxel_layout: wgpu::BindGroupLayout,
    caustic_pipeline: wgpu::ComputePipeline,
    caustic_layout: wgpu::BindGroupLayout,
    shadow_sampler: wgpu::Sampler,
    csm_sampler: wgpu::Sampler,
    fft_sampler: wgpu::Sampler,
    shadow_mapping: wgpu::Buffer,
    camera_shadow: wgpu::Buffer,
}

pub struct Underwater {
    pipeline: wgpu::RenderPipeline,
    layout: wgpu::BindGroupLayout,
    sampler: wgpu::Sampler,
    params: wgpu::Buffer,
    _froxel_tex: wgpu::Texture,
    froxel_view: wgpu::TextureView,
    _caustic_tex: wgpu::Texture,
    caustic_view: wgpu::TextureView,
    volume: Option<Volume>,
}

impl Underwater {
    pub fn new(
        device: &wgpu::Device,
        format: wgpu::TextureFormat,
        composer: &mut naga_oil::compose::Composer,
        volume_storage_supported: bool,
    ) -> Self {
        // WRL-002: composed through the shared module set so the compositor can #import the
        // water optics and the FFT lookup instead of carrying private copies of both.
        let shader = crate::shaders::make_module(
            device,
            composer,
            "wgr_underwater_shader",
            include_str!("underwater.wgsl"),
            "underwater.wgsl",
        );
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_underwater_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Depth,
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 4,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D3,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 5,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                // Vertical FFT displacement: the compositor locates the waterline on the
                // wavy surface rather than on a flat plane at sea_level.
                wgpu::BindGroupLayoutEntry {
                    binding: 6,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2Array,
                        multisampled: false,
                    },
                    count: None,
                },
            ],
        });
        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_underwater_pipeline_layout"),
            bind_group_layouts: &[Some(&layout)],
            immediate_size: 0,
        });
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("wgr_underwater_pipeline"),
            layout: Some(&pipeline_layout),
            vertex: wgpu::VertexState {
                module: &shader,
                entry_point: Some("vs_main"),
                buffers: &[],
                compilation_options: Default::default(),
            },
            primitive: wgpu::PrimitiveState::default(),
            depth_stencil: None,
            multisample: wgpu::MultisampleState::default(),
            fragment: Some(wgpu::FragmentState {
                module: &shader,
                entry_point: Some("fs_main"),
                targets: &[Some(wgpu::ColorTargetState {
                    format,
                    blend: None,
                    write_mask: wgpu::ColorWrites::ALL,
                })],
                compilation_options: Default::default(),
            }),
            multiview_mask: None,
            cache: None,
        });
        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_underwater_sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            address_mode_w: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });
        let params = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("wgr_underwater_params"),
            contents: bytemuck::bytes_of(&Params::zeroed()),
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
        });

        let volume_usage = wgpu::TextureUsages::TEXTURE_BINDING
            | if volume_storage_supported {
                wgpu::TextureUsages::STORAGE_BINDING
            } else {
                wgpu::TextureUsages::empty()
            };
        let froxel_tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_underwater_froxel"),
            size: wgpu::Extent3d {
                width: FROXEL_W,
                height: FROXEL_H,
                depth_or_array_layers: FROXEL_D,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D3,
            format: VOLUME_FORMAT,
            usage: volume_usage,
            view_formats: &[],
        });
        let froxel_view = froxel_tex.create_view(&wgpu::TextureViewDescriptor {
            label: Some("wgr_underwater_froxel_view"),
            dimension: Some(wgpu::TextureViewDimension::D3),
            ..Default::default()
        });
        let caustic_tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_underwater_caustics"),
            size: wgpu::Extent3d {
                width: CAUSTIC_SIZE,
                height: CAUSTIC_SIZE,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: VOLUME_FORMAT,
            usage: volume_usage,
            view_formats: &[],
        });
        let caustic_view = caustic_tex.create_view(&wgpu::TextureViewDescriptor::default());

        let volume = volume_storage_supported.then(|| {
            let froxel_shader = crate::shaders::make_module(
                device,
                composer,
                "wgr_underwater_froxel_shader",
                include_str!("underwater_froxel.wgsl"),
                "underwater_froxel.wgsl",
            );
            let uniform = |binding| wgpu::BindGroupLayoutEntry {
                binding,
                visibility: wgpu::ShaderStages::COMPUTE,
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Uniform,
                    has_dynamic_offset: false,
                    min_binding_size: None,
                },
                count: None,
            };
            let froxel_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("wgr_underwater_froxel_layout"),
                entries: &[
                    uniform(0),
                    wgpu::BindGroupLayoutEntry {
                        binding: 1,
                        visibility: wgpu::ShaderStages::COMPUTE,
                        ty: wgpu::BindingType::StorageTexture {
                            access: wgpu::StorageTextureAccess::WriteOnly,
                            format: VOLUME_FORMAT,
                            view_dimension: wgpu::TextureViewDimension::D3,
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 2,
                        visibility: wgpu::ShaderStages::COMPUTE,
                        ty: wgpu::BindingType::Texture {
                            sample_type: wgpu::TextureSampleType::Float { filterable: true },
                            view_dimension: wgpu::TextureViewDimension::D2,
                            multisampled: false,
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 3,
                        visibility: wgpu::ShaderStages::COMPUTE,
                        ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                        count: None,
                    },
                    uniform(4),
                    wgpu::BindGroupLayoutEntry {
                        binding: 5,
                        visibility: wgpu::ShaderStages::COMPUTE,
                        ty: wgpu::BindingType::Texture {
                            sample_type: wgpu::TextureSampleType::Depth,
                            view_dimension: wgpu::TextureViewDimension::D2Array,
                            multisampled: false,
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 6,
                        visibility: wgpu::ShaderStages::COMPUTE,
                        ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Comparison),
                        count: None,
                    },
                    uniform(7),
                ],
            });
            let froxel_pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("wgr_underwater_froxel_pipeline_layout"),
                bind_group_layouts: &[Some(&froxel_layout)],
                immediate_size: 0,
            });
            let froxel_pipeline =
                device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
                    label: Some("wgr_underwater_froxel_pipeline"),
                    layout: Some(&froxel_pl),
                    module: &froxel_shader,
                    entry_point: Some("cs_underwater_froxel"),
                    compilation_options: Default::default(),
                    cache: None,
                });

            let caustic_shader = crate::shaders::make_module(
                device,
                composer,
                "wgr_underwater_caustic_shader",
                include_str!("underwater_caustics.wgsl"),
                "underwater_caustics.wgsl",
            );
            let caustic_layout =
                device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                    label: Some("wgr_underwater_caustic_layout"),
                    entries: &[
                        uniform(0),
                        wgpu::BindGroupLayoutEntry {
                            binding: 1,
                            visibility: wgpu::ShaderStages::COMPUTE,
                            ty: wgpu::BindingType::Texture {
                                sample_type: wgpu::TextureSampleType::Float { filterable: true },
                                view_dimension: wgpu::TextureViewDimension::D2Array,
                                multisampled: false,
                            },
                            count: None,
                        },
                        wgpu::BindGroupLayoutEntry {
                            binding: 2,
                            visibility: wgpu::ShaderStages::COMPUTE,
                            ty: wgpu::BindingType::Texture {
                                sample_type: wgpu::TextureSampleType::Float { filterable: true },
                                view_dimension: wgpu::TextureViewDimension::D2Array,
                                multisampled: false,
                            },
                            count: None,
                        },
                        wgpu::BindGroupLayoutEntry {
                            binding: 3,
                            visibility: wgpu::ShaderStages::COMPUTE,
                            ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                            count: None,
                        },
                        wgpu::BindGroupLayoutEntry {
                            binding: 4,
                            visibility: wgpu::ShaderStages::COMPUTE,
                            ty: wgpu::BindingType::StorageTexture {
                                access: wgpu::StorageTextureAccess::WriteOnly,
                                format: VOLUME_FORMAT,
                                view_dimension: wgpu::TextureViewDimension::D2,
                            },
                            count: None,
                        },
                    ],
                });
            let caustic_pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("wgr_underwater_caustic_pipeline_layout"),
                bind_group_layouts: &[Some(&caustic_layout)],
                immediate_size: 0,
            });
            let caustic_pipeline =
                device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
                    label: Some("wgr_underwater_caustic_pipeline"),
                    layout: Some(&caustic_pl),
                    module: &caustic_shader,
                    entry_point: Some("cs_underwater_caustics"),
                    compilation_options: Default::default(),
                    cache: None,
                });
            let shadow_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
                label: Some("wgr_underwater_shadow_sampler"),
                address_mode_u: wgpu::AddressMode::ClampToEdge,
                address_mode_v: wgpu::AddressMode::ClampToEdge,
                mag_filter: wgpu::FilterMode::Linear,
                min_filter: wgpu::FilterMode::Linear,
                ..Default::default()
            });
            let csm_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
                label: Some("wgr_underwater_csm_sampler"),
                compare: Some(wgpu::CompareFunction::LessEqual),
                ..Default::default()
            });
            let fft_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
                label: Some("wgr_underwater_fft_sampler"),
                address_mode_u: wgpu::AddressMode::Repeat,
                address_mode_v: wgpu::AddressMode::Repeat,
                mag_filter: wgpu::FilterMode::Linear,
                min_filter: wgpu::FilterMode::Linear,
                ..Default::default()
            });
            let shadow_mapping = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                label: Some("wgr_underwater_shadow_mapping"),
                contents: bytemuck::bytes_of(&crate::terrain::TerrainShadowMap::zeroed()),
                usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            });
            let camera_shadow = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                label: Some("wgr_underwater_camera_shadow"),
                contents: bytemuck::bytes_of(&crate::ffi::WgrCameraShadow::zeroed()),
                usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            });
            Volume {
                froxel_pipeline,
                froxel_layout,
                caustic_pipeline,
                caustic_layout,
                shadow_sampler,
                csm_sampler,
                fft_sampler,
                shadow_mapping,
                camera_shadow,
            }
        });

        Self {
            pipeline,
            layout,
            sampler,
            params,
            _froxel_tex: froxel_tex,
            froxel_view,
            _caustic_tex: caustic_tex,
            caustic_view,
            volume,
        }
    }

    #[allow(clippy::too_many_arguments)]
    pub fn upload(
        &self,
        queue: &wgpu::Queue,
        time: f32,
        cam_above: f32,
        camera_pos: [f32; 3],
        inv_view_proj: [f32; 16],
        shallow_color: [f32; 4],
        deep_color: [f32; 4],
        sun_dir: [f32; 3],
        sun_radiance: [f32; 3],
        cascade_lengths: [f32; 4],
        active_layers: u32,
        warp_amp: f32,
        sea_level: f32,
        debug_view: f32,
        wave_scale: f32,
        body_wave_scale: f32,
        body_ellipse: [f32; 4],
        body_frame: [f32; 2],
        density: f32,
        color_bias: f32,
        caustic_gain: f32,
        shadow_mapping: &crate::terrain::TerrainShadowMap,
        camera_shadow: &crate::ffi::WgrCameraShadow,
    ) {
        queue.write_buffer(
            &self.params,
            0,
            bytemuck::bytes_of(&Params {
                time_height_range_ext: [time, cam_above, 120.0, shallow_color[3]],
                camera_pos_layers: [
                    camera_pos[0],
                    camera_pos[1],
                    camera_pos[2],
                    active_layers as f32,
                ],
                sun_dir_debug: [sun_dir[0], sun_dir[1], sun_dir[2], debug_view],
                sun_radiance: [sun_radiance[0], sun_radiance[1], sun_radiance[2], 0.0],
                inv_view_proj,
                shallow_color,
                deep_color,
                cascade_lengths,
                water_controls: [warp_amp, sea_level, wave_scale, body_wave_scale],
                underwater_tuning: [density, color_bias, caustic_gain, 0.0],
                body_ellipse,
                body_frame: [body_frame[0], body_frame[1], 0.0, 0.0],
            }),
        );
        if let Some(volume) = &self.volume {
            queue.write_buffer(
                &volume.shadow_mapping,
                0,
                bytemuck::bytes_of(shadow_mapping),
            );
            queue.write_buffer(&volume.camera_shadow, 0, bytemuck::bytes_of(camera_shadow));
        }
    }

    pub fn render_froxel(
        &self,
        device: &wgpu::Device,
        encoder: &mut wgpu::CommandEncoder,
        shadow_mask: &wgpu::TextureView,
        csm_view: &wgpu::TextureView,
    ) {
        let Some(volume) = &self.volume else {
            return;
        };
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_underwater_froxel_bind"),
            layout: &volume.froxel_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: self.params.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::TextureView(&self.froxel_view),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::TextureView(shadow_mask),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: wgpu::BindingResource::Sampler(&volume.shadow_sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 4,
                    resource: volume.shadow_mapping.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 5,
                    resource: wgpu::BindingResource::TextureView(csm_view),
                },
                wgpu::BindGroupEntry {
                    binding: 6,
                    resource: wgpu::BindingResource::Sampler(&volume.csm_sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 7,
                    resource: volume.camera_shadow.as_entire_binding(),
                },
            ],
        });
        encoder.push_debug_group("wgr_underwater_froxel");
        let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("wgr_underwater_froxel"),
            timestamp_writes: None,
        });
        pass.set_pipeline(&volume.froxel_pipeline);
        pass.set_bind_group(0, &bind, &[]);
        pass.dispatch_workgroups(FROXEL_W.div_ceil(8), FROXEL_H.div_ceil(8), 1);
        drop(pass);
        encoder.pop_debug_group();
    }

    pub fn render_caustics(
        &self,
        device: &wgpu::Device,
        encoder: &mut wgpu::CommandEncoder,
        fft_dynamics: &wgpu::TextureView,
        fft_auxiliary: &wgpu::TextureView,
    ) {
        let Some(volume) = &self.volume else {
            return;
        };
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_underwater_caustic_bind"),
            layout: &volume.caustic_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: self.params.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::TextureView(fft_dynamics),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::TextureView(fft_auxiliary),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: wgpu::BindingResource::Sampler(&volume.fft_sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 4,
                    resource: wgpu::BindingResource::TextureView(&self.caustic_view),
                },
            ],
        });
        encoder.push_debug_group("wgr_underwater_caustics");
        let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("wgr_underwater_caustics"),
            timestamp_writes: None,
        });
        pass.set_pipeline(&volume.caustic_pipeline);
        pass.set_bind_group(0, &bind, &[]);
        pass.dispatch_workgroups(CAUSTIC_SIZE.div_ceil(8), CAUSTIC_SIZE.div_ceil(8), 1);
        drop(pass);
        encoder.pop_debug_group();
    }

    pub fn render(
        &self,
        device: &wgpu::Device,
        encoder: &mut wgpu::CommandEncoder,
        source: &wgpu::TextureView,
        depth: &wgpu::TextureView,
        destination: &wgpu::TextureView,
        fft_displacement: &wgpu::TextureView,
    ) {
        let bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_underwater_bind"),
            layout: &self.layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(source),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::Sampler(&self.sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::TextureView(depth),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: self.params.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 4,
                    resource: wgpu::BindingResource::TextureView(&self.froxel_view),
                },
                wgpu::BindGroupEntry {
                    binding: 5,
                    resource: wgpu::BindingResource::TextureView(&self.caustic_view),
                },
                wgpu::BindGroupEntry {
                    binding: 6,
                    resource: wgpu::BindingResource::TextureView(fft_displacement),
                },
            ],
        });
        encoder.push_debug_group("wgr_underwater");
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("wgr_underwater"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: destination,
                depth_slice: None,
                resolve_target: None,
                ops: wgpu::Operations {
                    load: wgpu::LoadOp::Clear(wgpu::Color::BLACK),
                    store: wgpu::StoreOp::Store,
                },
            })],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        pass.set_pipeline(&self.pipeline);
        pass.set_bind_group(0, &bind, &[]);
        pass.draw(0..3, 0..1);
        drop(pass);
        encoder.pop_debug_group();
    }
}
