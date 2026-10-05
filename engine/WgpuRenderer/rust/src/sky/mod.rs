// Procedural atmospheric sky (plan Stage 2a): a fullscreen pass that marches the
// view ray through a Hillaire-style LUT atmosphere and writes the scene target
// (HDR texture when the HDR path is on, else the swapchain) BEFORE geometry, so
// terrain/objects overdraw it; depth is neither tested nor written. See
// docs/procedural-sky-plan.md.
//
// Two small LUTs feed the main march — a transmittance LUT and an isotropic
// multi-scattering LUT — both depending only on the atmosphere parameters (the sun
// is a LUT axis), so they rebuild only when those params change (dirty-flagged),
// not every frame. Celestial + authored params arrive from C++ via wgr_set_sky.

use bytemuck::Zeroable;
use wgpu::util::DeviceExt;

use crate::ffi::WgrSky;

// Production and offline validation must resolve exactly the same imports.
// Passing source text directly to wgpu cannot parse naga_oil's #import syntax.
fn compose_sky(composer:&mut naga_oil::compose::Composer)->naga::Module {
    composer.make_naga_module(naga_oil::compose::NagaModuleDescriptor {
        source:include_str!("sky.wgsl"),file_path:"sky/sky.wgsl",
        shader_defs:crate::shaders::shader_defs(),..Default::default()
    }).unwrap_or_else(|e|panic!("compose wgr_sky_shader: {}",e.emit_to_string(composer)))
}

// CPU reference port of the atmosphere math, for objective colour unit tests.
#[cfg(test)]
mod reference;
pub mod milkyway;
pub mod starcat;

// Guards every sky.wgsl edit: parse + validate the module offline so a WGSL error
// surfaces in CI/tests instead of as a pipeline-creation panic at runtime.
#[test]
fn sky_wgsl_validates() {
    let mut composer=crate::shaders::build_composer();
    let module=compose_sky(&mut composer);
    naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::all(),
    )
    .validate(&module)
    .expect("sky.wgsl validate");
}

// SKY-002. The stars were already added to the sky BEFORE the cloud composite, and they still
// came through an opaque overcast at ~0.65 of their clear-sky brightness, because the cloud march
// is lit by `bg` and `bg` was the full sky radiance AT THAT PIXEL -- so a star lit the cloud in
// front of itself and the composite's `bg*trans + inscatter` handed it back as inscatter.
//
// This is a source-text assertion rather than a rendered one because the cloud march is a separate
// depth-aware pass over the scene, which the headless single-pass sky harness cannot record; the
// rendered proof is the night clear-vs-overcast capture pair. What it CAN pin is the thing that
// regressed: that neither cloud caller is handed the star-carrying lane again.
#[test]
fn the_cloud_march_is_not_lit_by_the_stars() {
    let src = include_str!("sky.wgsl");
    // The split exists and the weight actually gates the lookup (weight 0 must not merely scale
    // star_field by zero -- it must skip it, or the cloud pass pays for a term it discards, with
    // a g_px_angle that was never set in that entry point).
    assert!(
        src.contains("fn sky_radiance_weighted(dir: vec3<f32>, star_weight: f32)")
            && src.contains("if (star_weight > 0.001) {"),
        "sky_radiance must take a star weight and skip star_field when it is zero"
    );
    // Cloud/env callers take the star-free lane. The visible sky now extracts
    // atmosphere from the same weighted source so fog chroma cannot recolour
    // cloud illumination or add a second sky/star evaluation.
    let cloud_bg = src.matches("let bg = sky_radiance_cloud_bg(dir);").count();
    let visible = &src[src.find("fn fs_sky(in:").unwrap()
        ..src.find("fn fs_sky_env(in:").unwrap()];
    assert!(
        cloud_bg >= 2
            && visible.contains("let lanes=sky_radiance_lanes(dir,clamp(1.0-sky.night_sky.x,0.0,1.0));")
            && visible.contains("let bg = lanes.original;")
            && visible.contains("let t = disc_cloud_cover(pos, dir, sun, sky.sun_dir.w * sky.params.y, bg);")
            && src.contains("return sky_radiance_lanes(dir,star_weight).original;"),
        "visible sky/cloud/env must retain the original star-controlled illumination lane \
         (found {cloud_bg} explicit cloud/env callers)"
    );
    // ...and nothing marches clouds against the full sky radiance any more.
    assert!(
        !src.contains("march_clouds(pos, dir, sun, radiance, color,")
            && !src.contains("radiance, color, 0.0, 1e12)")
            && !src.contains("sky.params.y, color)"),
        "a cloud march is still being handed the star-carrying sky radiance"
    );
    // And the strength is a real lane, not a hard-coded 1 -- WGR_STAR_OCCLUSION=0 has to restore
    // the old look exactly, or there is no A/B for the next person who doubts this.
    assert!(
        src.contains("night_sky: vec4<f32>") && src.contains("1.0 - sky.night_sky.x"),
        "the star cloud-occlusion strength must ride a UBO lane"
    );
}

// SKY-004. The cloud decks now have a lunar light source. Three things about it are load-bearing
// and none of them is visible in a compile: that daylight cannot move, that a MOONLESS night stays
// black, and that the whole thing is switchable. All three live in `lunar_cloud_light`'s gates.
//
// Source-text, for the same reason `the_cloud_march_is_not_lit_by_the_stars` is: the deck march is
// a separate depth-aware pass over the scene (fs_cloud) that the headless single-pass sky harness
// cannot record, and fs_sky only ever asks it for a scalar disc cover. The rendered proof is the
// four-way capture set (overcast+moon / overcast+no moon / clear night / overcast day).
#[test]
fn the_cloud_decks_have_a_gated_lunar_light() {
    let src = include_str!("sky.wgsl");
    assert!(
        src.contains("fn lunar_cloud_light(pos: vec3<f32>, sun: vec3<f32>) -> LunarLight"),
        "the lunar cloud light source is gone"
    );
    // 1. SWITCHABLE, on a real UBO lane -- not a hard-coded 1. WGR_MOON_CLOUDS=0 must restore the
    //    old look, or there is no A/B for the next person who doubts this.
    assert!(
        src.contains("let gain = sky.night_sky.y;") && src.contains("if (gain <= 0.0) {"),
        "the moonlight gain must ride a UBO lane and 0 must skip the term entirely"
    );
    // 2. PHASE is the ephemeris', not a fake: irradiance is the disc's radiance scale times the
    //    illuminated fraction, which reconstructs the empirical lunar phase function C++ divided
    //    out. A constant here would be a moon that lights the deck the same at new and full.
    assert!(
        src.contains("max(sky.moon_params.z, 0.0) * clamp(sky.moon_params.y, 0.0, 1.0)"),
        "lunar irradiance must be the disc scale x the illuminated fraction (phase included)"
    );
    // 3. THE TWO GATES. `up` keeps a moonless night black -- the owner asked for that explicitly --
    //    and `night` makes every daylight frame take the early-out, so daylight is bit-identical
    //    rather than merely close.
    assert!(
        src.contains("let up = smoothstep(-0.05, 0.02, moon.y);")
            && src.contains("let night = 1.0 - smoothstep(-0.035, 0.0, sun.y);")
            && src.contains("let k = up * night * gain;"),
        "the moon-up and sun-down gates must both be present and multiplied together"
    );
    // 4. The colour is the SUN's. Moonlight is reflected sunlight; the blue is scotopic vision, not
    //    photometry. If a blue constant ever becomes the default, this fails and says why.
    assert!(
        src.contains("let blue = clamp(sky.night_sky.z, 0.0, 1.0);")
            && src.contains("mix(vec3<f32>(1.0, 0.97, 0.92), vec3<f32>(0.60, 0.76, 1.05), blue)"),
        "moonlight on clouds must default to the sun's colour, with the blue shift switchable"
    );
    // 5. Both decks take it: the cumulus march (with a real light march, so tops light and bases
    //    shadow) and the thin cirrus sheet (no march needed).
    assert_eq!(
        src.matches("lunar_cloud_light(pos, sun)").count(),
        2,
        "both march_clouds and cirrus_layer must take the lunar light"
    );
    assert!(
        src.contains("direct = direct + ml.col * exp(-od_m) * phase_m * beer;"),
        "the deck's lunar term must be self-shadowed by its own light march"
    );
    // 6. The night cost stays flat: the solar light march is skipped when the sun contributes
    //    nothing, which is exactly the frames where the lunar one runs.
    assert!(
        src.contains("let sun_lit = dot(sun_col, vec3<f32>(0.2126, 0.7152, 0.0722)) > 1.0e-6;")
            && src.contains("if (sun_lit) {"),
        "the solar light march must be skipped when the sun is below the horizon"
    );
}

// Same offline guard for the standalone SH-projection compute (create_shader_module'd, so it is
// not covered by the naga_oil entry-shader compose test).
#[test]
fn sky_sh_wgsl_validates() {
    let module =
        naga::front::wgsl::parse_str(include_str!("sky_sh.wgsl")).expect("sky_sh.wgsl parse");
    naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::all(),
    )
    .validate(&module)
    .expect("sky_sh.wgsl validate");
}

// Offline guard for the over-scene cloud composite shader (separate module, own binding namespace).
#[test]
fn cloud_composite_wgsl_validates() {
    let mut composer=crate::shaders::build_composer();
    let module=composer.make_naga_module(naga_oil::compose::NagaModuleDescriptor {
        source:include_str!("cloud_composite.wgsl"),file_path:"cloud_composite.wgsl",
        shader_defs:crate::shaders::shader_defs(),..Default::default()
    }).unwrap_or_else(|e|panic!("{}",e.emit_to_string(&composer)));
    naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::all(),
    )
    .validate(&module)
    .expect("cloud_composite.wgsl validate");
}

// The three checks above are naga-ONLY, which is necessary but not sufficient: naga never
// sees a bind group layout, so a binding whose declared type disagrees with the layout — or
// a uniform whose size no longer matches what the pipeline expects — parses and validates
// cleanly and then panics at pipeline creation. That panic is what the project's startup
// diagnostics describe as "the prompt returns instantly with nothing on screen".
//
// Building the real Sky against a real device is the test that catches it, because
// Sky::new creates every sky pipeline (fs_sky included, which is where moon_disc lives)
// against the actual bind group layouts and writes the actual uniform buffer.
#[cfg(test)]
mod sky_device_tests {
    // Best-effort headless device; the test skips when no adapter is available (CI without
    // a GPU). Same shape as godrays.rs::tests::headless.
    fn headless() -> Option<(wgpu::Device, wgpu::Queue)> {
        let instance =
            wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::default(),
            compatible_surface: None,
            force_fallback_adapter: false,
        }))
        .ok()?;
        pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor::default())).ok()
    }

    #[test]
    fn sky_pipelines_build_on_a_real_device() {
        let Some((device, queue)) = headless() else {
            return;
        };
        let mut sky = super::Sky::new(&device, &queue, crate::HDR_FORMAT, 1);
        // Also exercise the upload path, so a SkyUniform whose size drifted away from
        // sky.wgsl's `struct Sky` fails HERE (as a bind/size validation error) rather than
        // as "buffer bound with size N where the shader expects M" on every 3D draw.
        let params = crate::ffi::WgrSky::default();
        let ident = [
            [1.0f32, 0.0, 0.0, 0.0],
            [0.0, 1.0, 0.0, 0.0],
            [0.0, 0.0, 1.0, 0.0],
            [0.0, 0.0, 0.0, 1.0],
        ];
        sky.upload(
            &queue,
            &params,
            ident,
            [0.0, 0.0, 0.0, 0.0],
            &bytemuck::Zeroable::zeroed(),
            &bytemuck::Zeroable::zeroed(),
        );
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
    }

    // ---- The moon disc actually reaches pixels -------------------------------------
    //
    // Building the pipeline proves the shader COMPILES. It does not prove moon_disc()
    // returns anything, and "the disc is never drawn" is exactly the failure that
    // survives a clean compile. This renders the real fs_sky pass headlessly with a moon
    // placed dead-centre and reads the pixels back, so the question "is the shader
    // drawing the disc" is answered here rather than from a screenshot.
    //
    // It also pins the two things a screenshot could not separate: whether the disc is
    // absent (gate closed / shader broken) or merely somewhere else (direction wrong).

    fn f16_to_f32(h: u16) -> f32 {
        let sign = ((h >> 15) & 1) as u32;
        let exp = ((h >> 10) & 0x1f) as u32;
        let man = (h & 0x3ff) as u32;
        let bits = match exp {
            0 if man == 0 => sign << 31,
            0 => {
                // Subnormal: normalise it into a f32 exponent.
                let mut e = -1i32;
                let mut m = man;
                while m & 0x400 == 0 {
                    m <<= 1;
                    e -= 1;
                }
                let exp32 = (127 - 15 + e) as u32;
                (sign << 31) | (exp32 << 23) | ((m & 0x3ff) << 13)
            }
            0x1f => (sign << 31) | (0xff << 23) | (man << 13),
            _ => (sign << 31) | ((exp + 127 - 15) << 23) | (man << 13),
        };
        f32::from_bits(bits)
    }

    fn cross(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
        [
            a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0],
        ]
    }

    fn normalize(v: [f32; 3]) -> [f32; 3] {
        let l = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt();
        [v[0] / l, v[1] / l, v[2] / l]
    }

    const TW: u32 = 128;
    const TH: u32 = 128;

    // Render fs_sky once into an HDR target and return the decoded RGBA f32 pixels.
    fn render_sky_rgba(
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        sky: &mut super::Sky,
        params: &crate::ffi::WgrSky,
        ivp: [[f32; 4]; 4],
    ) -> Vec<[f32; 4]> {
        let tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("moon_test_target"),
            size: wgpu::Extent3d {
                width: TW,
                height: TH,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: crate::HDR_FORMAT,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        let view = tex.create_view(&Default::default());
        // 128 px * 8 bytes = 1024, already a multiple of the 256-byte row alignment.
        let row_bytes = TW * 8;
        let buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("moon_test_readback"),
            size: (row_bytes * TH) as u64,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });

        // The camera sits at the origin (the sky vs has no translation), so cam_pos only
        // feeds the cloud-shadow lookup; sea level keeps the raymarch origin sane.
        sky.upload(
            queue,
            params,
            ivp,
            [0.0, 0.0, 0.0, 0.0],
            &bytemuck::Zeroable::zeroed(),
            &bytemuck::Zeroable::zeroed(),
        );
        let mut encoder = device.create_command_encoder(&Default::default());
        // WITHOUT this the transmittance LUT is an uninitialised texture, moon_disc's
        // sample_transmittance returns 0 and the disc is multiplied to black — a way to
        // "lose" the moon that has nothing to do with the moon code.
        sky.render_luts(&mut encoder);
        {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("moon_test_sky"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &view,
                    resolve_target: None,
                    depth_slice: None,
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
            sky.render(&mut pass);
        }
        encoder.copy_texture_to_buffer(
            wgpu::TexelCopyTextureInfo {
                texture: &tex,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            wgpu::TexelCopyBufferInfo {
                buffer: &buf,
                layout: wgpu::TexelCopyBufferLayout {
                    offset: 0,
                    bytes_per_row: Some(row_bytes),
                    rows_per_image: Some(TH),
                },
            },
            wgpu::Extent3d {
                width: TW,
                height: TH,
                depth_or_array_layers: 1,
            },
        );
        queue.submit(Some(encoder.finish()));
        buf.slice(..).map_async(wgpu::MapMode::Read, |_| {});
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
        let data = buf.slice(..).get_mapped_range();
        let halves: &[u16] = bytemuck::cast_slice(&data);
        let out = halves
            .chunks_exact(4)
            .map(|c| {
                [
                    f16_to_f32(c[0]),
                    f16_to_f32(c[1]),
                    f16_to_f32(c[2]),
                    f16_to_f32(c[3]),
                ]
            })
            .collect::<Vec<_>>();
        drop(data);
        buf.unmap();
        out
    }

    #[test]
    fn the_moon_disc_actually_renders() {
        let Some((device, queue)) = headless() else {
            return;
        };
        let mut sky = super::Sky::new(&device, &queue, crate::HDR_FORMAT, 1);

        // The coordinator's exact capture geometry: azimuth 120, elevation 30, in the
        // engine frame (x = east, y = up, z = north).
        let az = 120.0f32.to_radians();
        let el = 30.0f32.to_radians();
        let fwd = [el.cos() * az.sin(), el.sin(), el.cos() * az.cos()];
        let right = normalize(cross([0.0, 1.0, 0.0], fwd));
        let upv = cross(fwd, right);
        // inv_view_proj mapping ndc(x,y,0,1) -> right*x*s + up*y*s + fwd, w = 1. Columns,
        // because WGSL mat4x4 built from a [[f32;4];4] is column-major. s = tan(fov/2).
        let s = 0.5f32; // ~53 deg horizontal fov
        let ivp = [
            [right[0] * s, right[1] * s, right[2] * s, 0.0],
            [upv[0] * s, upv[1] * s, upv[2] * s, 0.0],
            [0.0, 0.0, 0.0, 0.0],
            [fwd[0], fwd[1], fwd[2], 1.0],
        ];

        let mut params = crate::ffi::WgrSky::default();
        // Night: sun well below the horizon, so the sky behind the moon is dark and any
        // bright pixel is unambiguously the disc.
        params.sun_dir = [0.0, -1.0, 0.0, 22.0];
        params.ground_albedo[3] = 1.0; // night factor
        params.cloud0[0] = 0.0; // no cloud deck over the disc
        // Moon dead-centre, full, and large enough to cover many pixels at this fov.
        params.moon_dir = [fwd[0], fwd[1], fwd[2], 1.0];
        let radius = 0.05f32; // ~2.9 deg -> ~14 px across at 128 px / 53 deg
        params.moon_params = [radius, 1.0, 1.0e-2, 1.0];
        // Full phase = sun behind the viewer, i.e. the lit face points back at us.
        params.moon_sun = [-fwd[0], -fwd[1], -fwd[2], 0.0];

        let lit = render_sky_rgba(&device, &queue, &mut sky, &params, ivp);

        // CONTROL: identical frame with the draw flag cleared. Without this the assertions
        // below would also pass on a frame whose centre is bright for some other reason.
        let mut off = params;
        off.moon_params[3] = 0.0;
        let dark = render_sky_rgba(&device, &queue, &mut sky, &off, ivp);

        let idx = |x: u32, y: u32| (y * TW + x) as usize;
        let centre = idx(TW / 2, TH / 2);
        let corner = idx(2, 2);

        let lum = |p: [f32; 4]| p[0] + p[1] + p[2];

        // 1. The disc exists: the centre is far brighter with the moon on than off.
        assert!(
            lum(lit[centre]) > lum(dark[centre]) * 10.0 + 1.0e-3,
            "moon_disc drew nothing at the centre: on={:?} off={:?}",
            lit[centre],
            dark[centre]
        );
        // 2. It is a DISC, not a full-screen wash: the corner must be untouched.
        assert!(
            (lum(lit[corner]) - lum(dark[corner])).abs() < 1.0e-4,
            "the moon changed a corner pixel — that is not a disc: on={:?} off={:?}",
            lit[corner],
            dark[corner]
        );
        // 3. The control is not vacuous: turning the moon off really did change the centre.
        assert!(
            lum(dark[centre]) < lum(lit[centre]),
            "the draw flag did nothing — the control frame is identical"
        );
        // 4. The disc covers a plausible number of pixels for its angular size, so a
        //    single stray bright texel cannot satisfy (1).
        let changed = lit
            .iter()
            .zip(dark.iter())
            .filter(|(a, b)| (lum(**a) - lum(**b)).abs() > 1.0e-4)
            .count();
        assert!(
            changed > 60 && changed < 4000,
            "disc covered {changed} px; expected a few hundred for a 2.9 deg radius at this fov"
        );
    }

    // A crescent must differ from a full moon. This is the assertion the coordinator's
    // capture made and could not satisfy: PHASE=1.0 and PHASE=0.35 came back
    // pixel-identical, which is only possible if the phase never reaches the shading.
    #[test]
    fn phase_changes_the_lit_fraction_of_the_disc() {
        let Some((device, queue)) = headless() else {
            return;
        };
        let mut sky = super::Sky::new(&device, &queue, crate::HDR_FORMAT, 1);

        let fwd = [0.0f32, 0.5, 0.8660254];
        let fwd = normalize(fwd);
        let right = normalize(cross([0.0, 1.0, 0.0], fwd));
        let upv = cross(fwd, right);
        let s = 0.5f32;
        let ivp = [
            [right[0] * s, right[1] * s, right[2] * s, 0.0],
            [upv[0] * s, upv[1] * s, upv[2] * s, 0.0],
            [0.0, 0.0, 0.0, 0.0],
            [fwd[0], fwd[1], fwd[2], 1.0],
        ];

        let mut base = crate::ffi::WgrSky::default();
        base.sun_dir = [0.0, -1.0, 0.0, 22.0];
        base.ground_albedo[3] = 1.0;
        base.cloud0[0] = 0.0;
        base.moon_dir = [fwd[0], fwd[1], fwd[2], 1.0];
        base.moon_params = [0.05, 1.0, 1.0e-2, 1.0];

        // Full: sub-solar direction points back at the viewer.
        let mut full = base;
        full.moon_sun = [-fwd[0], -fwd[1], -fwd[2], 0.0];
        let full_px = render_sky_rgba(&device, &queue, &mut sky, &full, ivp);

        // Side-lit: the sub-solar direction is perpendicular to the view, so exactly half
        // the visible disc is lit and the terminator runs through the centre.
        let mut half = base;
        half.moon_sun = [right[0], right[1], right[2], 0.0];
        let half_px = render_sky_rgba(&device, &queue, &mut sky, &half, ivp);

        // Difference against a moon-OFF frame so the measurement is the disc alone. Summing
        // the raw frame instead measures the night sky and the star field, which swamp a
        // 14-px disc and hide exactly the asymmetry this test exists to find.
        let mut off = base;
        off.moon_params[3] = 0.0;
        let off_px = render_sky_rgba(&device, &queue, &mut sky, &off, ivp);

        let lum = |p: [f32; 4]| p[0] + p[1] + p[2];
        let delta = |v: &[[f32; 4]], i: usize| (lum(v[i]) - lum(off_px[i])).max(0.0);
        let total = |v: &[[f32; 4]]| (0..(TW * TH) as usize).map(|i| delta(v, i)).sum::<f32>();
        // Disc-only light in the left vs right half. The disc is centred, so a full moon must
        // be symmetric and a side-lit moon must go dark on one side.
        let halves = |v: &[[f32; 4]]| {
            let mut l = 0.0f32;
            let mut r = 0.0f32;
            for y in 0..TH {
                for x in 0..TW {
                    let d = delta(v, (y * TW + x) as usize);
                    if x < TW / 2 { l += d } else { r += d }
                }
            }
            (l, r)
        };

        assert!(total(&full_px) > 0.1, "the full moon drew nothing");

        // The BRDF alone only accounts for the SHAPE of the phase. Its total falls modestly
        // (Lommel-Seeliger brightens the limb, so a lit half is brighter per unit area than a
        // full disc's centre); the order-of-magnitude falloff between full and quarter is
        // carried by the CPU's moon_params.z, which this test deliberately holds fixed so the
        // shading is measured on its own.
        assert!(
            total(&half_px) < total(&full_px) * 0.95,
            "phase did not dim the disc at all: full={} half={}",
            total(&full_px),
            total(&half_px)
        );

        // THE TERMINATOR. This is the assertion that matters: a side-lit moon must be dark on
        // one side and lit on the other. A phase that changes brightness but not WHICH PART is
        // lit is exactly the "phase right, terminator wrong" failure that looks worse than no
        // phase at all.
        let (fl, fr) = halves(&full_px);
        let (hl, hr) = halves(&half_px);
        assert!(
            (fl - fr).abs() < fl.max(fr) * 0.10,
            "a full moon must be left/right symmetric, got {fl} vs {fr}"
        );
        assert!(
            hr > hl * 3.0,
            "no terminator: sub-solar direction is +right, so the right half must be much \
             brighter, got left={hl} right={hr}"
        );
    }
}

// LUT resolutions. Transmittance is smooth in both axes; multiscatter is very
// low-frequency, so a tiny map suffices (its build is the expensive one).
const TRANSMITTANCE_W: u32 = 256;
const TRANSMITTANCE_H: u32 = 64;
const MULTISCATTER_SIZE: u32 = 32;
const LUT_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float;

// Aerial-perspective froxel volume: XY = screen, Z = distance (squared distribution).
// 32^3 is Hillaire's classic size — cheap to fill, soft enough for god-ray shafts.
const FROXEL_W: u32 = 32;
const FROXEL_H: u32 = 32;
const FROXEL_D: u32 = 32;
const FROXEL_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float;

// Reflection environment map: an equirectangular (lat-long) bake of the disc-free sky radiance,
// sampled by the water surface in its reflected direction (water look plan Stage 4a). The sky is
// low-frequency, so a small map is plenty; 2:1 for the full sphere. Linear radiance (Rgba16Float).
const ENV_W: u32 = 256;
const ENV_H: u32 = 128;
const ENV_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float;

// Cloud shape/detail noise: a tileable 3D texture sampled (Repeat) by the cloud march instead of
// evaluating analytic fBm per step. This is BOTH the perf fix (one texture tap vs ~dozens of hash
// evals) AND the moire fix (the old fract()-based hash lost precision at planet-scale / unbounded
// wind*time coordinates; a Repeat-sampled texture wraps at full precision). R = low-frequency
// shape fBm, G = higher-frequency detail fBm; all octave frequencies are powers of two that divide
// the texture size so the volume tiles seamlessly. 128^3 RGBA8 = 8 MB, generated once at startup.
const NOISE_N: u32 = 128;
const NOISE_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba8Unorm;

// Small integer hash -> u32 (Wang-style), for the tileable value-noise lattice.
fn cloud_hash(mut a: u32) -> u32 {
    a = (a ^ 61) ^ (a >> 16);
    a = a.wrapping_add(a << 3);
    a ^= a >> 4;
    a = a.wrapping_mul(0x27d4_eb2d);
    a ^= a >> 15;
    a
}

// Lattice value in [0,1) at integer cell (x,y,z), wrapped by `period` so the noise tiles.
fn cloud_lattice(x: i32, y: i32, z: i32, period: i32) -> f32 {
    let xi = x.rem_euclid(period) as u32;
    let yi = y.rem_euclid(period) as u32;
    let zi = z.rem_euclid(period) as u32;
    let h = cloud_hash(xi.wrapping_mul(1619) ^ yi.wrapping_mul(31337) ^ zi.wrapping_mul(6971));
    (h as f32) / (u32::MAX as f32)
}

// Tileable value noise at (u,v,w) in [0,1) with `freq` cells across the volume (freq must divide
// NOISE_N). Smoothstep-weighted trilinear interpolation of the wrapped lattice.
fn cloud_vnoise(u: f32, v: f32, w: f32, freq: i32) -> f32 {
    let f = freq as f32;
    let (px, py, pz) = (u * f, v * f, w * f);
    let (ix, iy, iz) = (px.floor(), py.floor(), pz.floor());
    let (fx, fy, fz) = (px - ix, py - iy, pz - iz);
    let sx = fx * fx * (3.0 - 2.0 * fx);
    let sy = fy * fy * (3.0 - 2.0 * fy);
    let sz = fz * fz * (3.0 - 2.0 * fz);
    let (x0, y0, z0) = (ix as i32, iy as i32, iz as i32);
    let c = |dx: i32, dy: i32, dz: i32| cloud_lattice(x0 + dx, y0 + dy, z0 + dz, freq);
    let lerp = |a: f32, b: f32, t: f32| a + (b - a) * t;
    let x00 = lerp(c(0, 0, 0), c(1, 0, 0), sx);
    let x10 = lerp(c(0, 1, 0), c(1, 1, 0), sx);
    let x01 = lerp(c(0, 0, 1), c(1, 0, 1), sx);
    let x11 = lerp(c(0, 1, 1), c(1, 1, 1), sx);
    lerp(lerp(x00, x10, sy), lerp(x01, x11, sy), sz)
}

// Tileable fBm normalised to [0,1]. base_freq and each octave (x2) must divide NOISE_N.
fn cloud_fbm(u: f32, v: f32, w: f32, base_freq: i32, octaves: i32) -> f32 {
    let mut sum = 0.0;
    let mut amp = 0.5;
    let mut freq = base_freq;
    let mut norm = 0.0;
    for _ in 0..octaves {
        sum += amp * cloud_vnoise(u, v, w, freq);
        norm += amp;
        freq *= 2;
        amp *= 0.5;
    }
    sum / norm
}

// Bake the RGBA8 cloud noise volume: R = shape (freq 4,8,16), G = detail (freq 8,16,32).
fn generate_cloud_noise() -> Vec<u8> {
    let n = NOISE_N as usize;
    let mut data = vec![0u8; n * n * n * 4];
    let inv = 1.0 / NOISE_N as f32;
    for z in 0..n {
        for y in 0..n {
            for x in 0..n {
                let u = (x as f32 + 0.5) * inv;
                let v = (y as f32 + 0.5) * inv;
                let w = (z as f32 + 0.5) * inv;
                let shape = cloud_fbm(u, v, w, 4, 3);
                let detail = cloud_fbm(u, v, w, 8, 3);
                let i = (z * n * n + y * n + x) * 4;
                data[i] = (shape.clamp(0.0, 1.0) * 255.0) as u8;
                data[i + 1] = (detail.clamp(0.0, 1.0) * 255.0) as u8;
            }
        }
    }
    data
}

// GPU uniform for the sky pass: the pushed WgrSky (8 vec4) plus the reconstructed
// inverse view-projection and an output-mode block. Must match `Sky` in sky.wgsl.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct SkyUniform {
    inv_view_proj: [[f32; 4]; 4],
    sun_dir: [f32; 4],
    moon_dir: [f32; 4],
    rayleigh: [f32; 4],
    mie: [f32; 4],
    ground_albedo: [f32; 4],
    params: [f32; 4],
    control: [f32; 4],
    fog_color: [f32; 4],
    night_zenith: [f32; 4],
    night_horizon: [f32; 4],
    night_params: [f32; 4],
    // Cloud shell params (mirror WgrSky::cloud0/1/2/3). See sky.wgsl's Sky struct.
    cloud0: [f32; 4],
    cloud1: [f32; 4],
    cloud2: [f32; 4],
    cloud3: [f32; 4],
    // Cloud EVOLUTION offsets (runtime): x = shape, y = detail, z = weather drift, w = pad.
    // Position matters — sky.wgsl declares cloud4 between cloud3 and output, and a mismatch here
    // is a silent layout shift, not a compile error. It surfaces as "buffer bound with size N
    // where the shader expects M" on every 3D draw at once.
    cloud4: [f32; 4],
    // x = linear output (1 = write linear radiance for the tonemap resolve; 0 =
    // self-tonemap for the LDR-direct path). y = star intensity, z = lens-flare gain,
    // w = second cloud layer (cirrus) mode: 0 off, 1 flat sheet, 2 volumetric shell march.
    // (The full-vs-cheap cloud split is by entry point — fs_sky vs fs_sky_env — not a
    // runtime flag.)
    output: [f32; 4],
    // xyz = absolute world camera position, so cs_froxel can turn a marched camera-
    // relative offset into a world position for the terrain sun-shadow mask lookup.
    // w = height above the flat ocean (0 disables clipping, including reflected views).
    cam_pos: [f32; 4],
    // CLD-020 cloud sun-transmittance map. xy = world-xz of the map's min corner, SNAPPED to the
    // texel grid (an unsnapped origin makes the shadow pattern crawl whenever the camera moves,
    // which reads as the clouds sliding across the ground). z = 1/span in metres, w = strength,
    // where 0 skips the pass and leaves every surface fully lit.
    cloud_shadow: [f32; 4],
    // Moon disc. x = angular RADIUS (rad), y = illuminated fraction, z = disc radiance
    // scale (irradiance — the shader divides by the solid angle, so inflating the disc for
    // visibility lowers its radiance and total power holds), w = draw (0 = skip).
    moon_params: [f32; 4],
    // xyz = unit dir TO the sun as seen from the moon (shades the lunar sphere);
    // w = earthshine reflectance floor on the dark side.
    moon_sun: [f32; 4],
    // Second cloud layer LOOK, written by the RENDERER (there is no C++ lane for it, the same
    // arrangement as cloud4.w's puffiness): x = AMOUNT (0..1, 0.5 = shipped), y = MATCH toward the
    // cumulus deck (0..1, 0 = shipped), zw reserved. Both arrive here already eased. See
    // sky.wgsl's `cirrus` lane and its AMOUNT / MATCH blocks.
    cirrus: [f32; 4],
    // SKY-002. x = STAR CLOUD-OCCLUSION strength [0,1]. Renderer-written, like `cirrus` and
    // cloud4.w, so no FFI struct changes size. See sky.wgsl's `night_sky` lane for what it does
    // and why the "stars are added before the cloud composite" arrangement was not enough on its
    // own. SKY-004: y = moonlight gain on the cloud decks, z = the stylistic blue shift on that
    // light. w = current admitted weather-fog chroma neutralization (private).
    night_sky: [f32; 4],
}

// The atmosphere-only fields that determine the LUTs (sun/night/exposure excluded,
// since the sun is a LUT axis and those don't change the tables). A change here
// dirties the LUTs; per-frame celestial pushes do not.
type LutKey = [f32; 14];

fn lut_key(sky: &WgrSky) -> LutKey {
    [
        sky.rayleigh[0],
        sky.rayleigh[1],
        sky.rayleigh[2],
        sky.rayleigh[3],
        sky.mie[0],
        sky.mie[1],
        sky.mie[2],
        sky.mie[3],
        sky.ground_albedo[0],
        sky.ground_albedo[1],
        sky.ground_albedo[2],
        sky.params[2],  // planet radius
        sky.params[3],  // atmosphere thickness
        sky.control[3], // ozone strength
    ]
}

/// Cloud sun-transmittance map (CLD-020). One texel per `span / CLOUD_SHADOW_DIM` metres of
/// world, centred on the camera; `span` is chosen per frame by `select_cloud_shadow_span`.
///
/// 512 over 4 km is ~7.8 m per texel. That is coarse for a shadow and exactly right for THIS
/// shadow: a cloud deck edge is tens of metres of penumbra anyway, so finer texels would cost
/// march work to resolve detail the phenomenon does not have.
///
/// 4 km was NOT enough range, though, and the claim that it "covers the view distance that
/// matters" was only true on foot. The map is centred on the camera, so it reached 2048 m; a
/// pilot with the view distance wound out sees ground several times further than that, and
/// `cloud_sun_shadow` returns fully lit outside the map. The result was a dead-straight line
/// across the landscape with everything beyond it in full sun, moving with the camera.
///
/// The span is therefore sized from the camera's own draw distance instead of pinned, and the
/// DIM stays at 512: the march is per texel, so growing the span is free (same dispatch, same
/// 12 steps per texel, same 1 MB texture) and is paid for in metres per texel rather than in
/// GPU time. See `select_cloud_shadow_span` for the levels and why they are powers of two.
const CLOUD_SHADOW_DIM: u32 = 512;

/// Fixed capacity of the smoke -> ground-shadow blob buffer. The froxel bind group is built
/// once, so the buffer is fixed-size and rewritten per frame; the C++ side caps to this.
pub const SMOKE_SHADOW_MAX_BLOBS: usize = 512;

/// One smoke particle as the ground-shadow pass sees it. Matches sky.wgsl `SmokeBlob`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, bytemuck::Pod, bytemuck::Zeroable)]
pub struct SmokeShadowBlob {
    /// xyz world position, w radius (m)
    pub pos_radius: [f32; 4],
    /// x optical density, y height above ground (m), zw unused
    pub density: [f32; 4],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, bytemuck::Pod, bytemuck::Zeroable)]
struct SmokeShadowHeader {
    count: u32,
    strength: f32,
    _pad0: f32,
    _pad1: f32,
}
/// Shortest span, and the one on-foot play keeps: 4096 m / 512 = 8 m per texel, i.e. exactly
/// the quality this feature shipped with, for every draw distance up to 2048 m.
const CLOUD_SHADOW_SPAN_MIN: f32 = 4096.0;
/// Longest span: 16384 m / 512 = 32 m per texel, covering an 8192 m draw distance. Past that
/// the map stops growing and `cloud_sun_shadow`'s edge fade takes over, which degrades to
/// "cloud shadows thin out at extreme range" instead of to a seam.
const CLOUD_SHADOW_SPAN_MAX: f32 = 16384.0;

/// The span level for a camera that draws ground out to `draw_distance` metres, given the level
/// currently in use. Free function so it is testable without a `wgpu::Device`; see
/// `Sky::select_cloud_shadow_span` for the reasoning behind every clause.
fn cloud_shadow_span_for(current: f32, draw_distance: f32) -> f32 {
    if !draw_distance.is_finite() || draw_distance <= 0.0 {
        // No usable far plane this frame (fog off, or a camera that never filled it). Keep the
        // level we had rather than guessing: the alternative is snapping back to the minimum for
        // one frame and re-rasterising the whole field for nothing.
        return current;
    }
    let needed = (2.0 * draw_distance).clamp(CLOUD_SHADOW_SPAN_MIN, CLOUD_SHADOW_SPAN_MAX);
    let mut span = CLOUD_SHADOW_SPAN_MIN;
    while span < needed && span < CLOUD_SHADOW_SPAN_MAX {
        span *= 2.0;
    }
    if span > current {
        return span;
    }
    if span < current && 2.0 * draw_distance < current * 0.45 {
        return span;
    }
    current
}

// CLD-021's adaptive span was uncommitted work in the codex checkout and is merged here. The
// first attempt at this conflict took the base's fixed 4096 m instead, on the theory that the
// smoke-shadow blob buffer above was sized from the span; it is not -- SMOKE_SHADOW_MAX_BLOBS is
// a blob COUNT and is independent of how far the map reaches. The two are orthogonal and both
// are in.

/// Initial second-cloud-layer (cirrus) mode: 0 = off, 1 = flat sheet, 2 = thin-shell volumetric.
///
/// Default 2, matching `Engine::SkySettings`. `WGR_CIRRUS` overrides it, and exists because the
/// only honest way to price the volumetric path is a `--benchmark` run — which runs `--no-dev`,
/// so the Sky tab's toggle is not reachable. It prints what it selected so a benchmark log is
/// self-describing rather than something you have to take on trust.
fn cirrus_mode_from_env() -> f32 {
    match std::env::var("WGR_CIRRUS")
        .ok()
        .and_then(|v| v.parse::<f32>().ok())
    {
        Some(v) => {
            let m = v.clamp(0.0, 2.0);
            eprintln!(
                "[wgr] sky: cirrus mode {} from WGR_CIRRUS (0 off, 1 flat, 2 volumetric)",
                m
            );
            m
        }
        None => 2.0,
    }
}

/// Cirrus PUFFINESS defaults, matching `Engine::SkySettings`.
///
/// 0.5 is the middle of the slider AND exactly the look the layer shipped with: sky.wgsl maps the
/// slider through a middle knot that IS the old constant (see `cirrus_mix3`), so the control is
/// opt-in tuning in both directions rather than a new look imposed on everyone.
const CIRRUS_PUFF_DEFAULT: f32 = 1.0;
/// How far the layer is allowed to wander from that on its own, as a fraction of the full slider.
/// Owner-tuned 2026-08-26 (with PUFF and MATCH below): full wander, so over the better part of an
/// hour the layer ranges the whole fibrous-to-lumpy span. 0 pins it exactly; the old shipped
/// default was 0.15.
const CIRRUS_PUFF_VAR_DEFAULT: f32 = 1.0;
/// Cirrus AMOUNT default, matching `Engine::SkySettings`. 0.5 is the middle of the slider AND
/// exactly the coverage the layer shipped with (sky.wgsl maps it through a middle knot that IS the
/// old `CIRRUS_COVER`), so the control is opt-in in both directions.
const CIRRUS_AMOUNT_DEFAULT: f32 = 0.5;
/// Cirrus MATCH default. 0 = high ice cloud, which is what the layer shipped as; 1 moves it as
/// close to the cumulus deck as the shell march gets. Owner-tuned 2026-08-26 to 1: the second
/// layer reads as more of the main deck's weather rather than a separate ice sheet. Costs the
/// deeper march (see the volumetric note above); measured on the devtest sky before defaulting.
const CIRRUS_MATCH_DEFAULT: f32 = 1.0;

/// Second-layer EDGE SOFTNESS default, matching `Engine::SkySettings`.
///
/// Unlike the other second-layer sliders this default is NOT the old look — 0 is. The owner asked
/// for the upper layer's bigger clouds to read fuzzier than they did, so the shipped value is a
/// real setting. Kept below the middle of the range because softening is far easier to over-apply
/// than to under-apply: past about 0.7 the banks start reading as haze rather than as cloud.
const CIRRUS_SOFT_DEFAULT: f32 = 0.45;

/// Metres of weather drift the C++ side wraps `cloud_evolve.z` at (`kWindWrap` in EngineWgpu.cpp).
/// The variation waveform is built to be exactly periodic over this distance, so the wrap reseat
/// — which is a real discontinuity in the drift value — is invisible in the puffiness.
const CIRRUS_VAR_WRAP: f32 = 100000.0;
/// Per-upload ease toward the target puffiness (the `rate` pattern auto-exposure uses). This is
/// what makes the sliders unable to POP: moving either of them, or the cloud-evolution speed the
/// clock is derived from, changes a TARGET that the rendered value then walks to — roughly half a
/// second to settle at 60 fps.
///
/// Frame-rate dependent by design (there is no dt on this path), and stepped once per `upload`,
/// which a frame drawing a planar reflection calls twice. Both are harmless for something this
/// small: the two calls differ by 8% of a residual that is already invisible, and it converges in
/// well under a second at any playable rate, so a delayed capture is deterministic to ~1e-5.
const CIRRUS_PUFF_EASE: f32 = 0.08;

/// The layer's self-variation: a bounded [-1,1] signal driven by the world's own weather-drift
/// clock, so it advances with the sim (and freezes with it, and with the Water tab's cloud freeze).
///
/// `drift_m` is `cloud_evolve.z` — metres, and therefore already scaled by the Sky tab's cloud
/// evolution speed: a world whose weather changes faster also breathes faster, which is the right
/// coupling. `phase_turns` shifts the whole signal (see `cirrus_puff_from_env`).
///
/// Two sines rather than one so it does not read as a metronome, and their frequencies are the
/// INTEGERS 7 and 11 in units of the wrap period. That is deliberate and is the opposite of the
/// choice made everywhere else in this file: here the signal is a function of TIME, not of space,
/// and it must be continuous where the clock jumps. Integer frequencies (and coprime, so the
/// combined period is the full wrap rather than a seventh of it) make the value at the wrap
/// identical to the value at 0. Amplitudes sum to exactly 1, so the output is bounded.
fn cirrus_variation(drift_m: f32, phase_turns: f32) -> f32 {
    let z = drift_m / CIRRUS_VAR_WRAP + phase_turns;
    let tau = std::f32::consts::TAU;
    0.6 * (tau * 7.0 * z + 1.7).sin() + 0.4 * (tau * 11.0 * z + 4.1).sin()
}

/// Where the puffiness is headed on a given clock reading: the authored base plus the variation,
/// clamped to the slider's own range — so a base near an end simply has less room to wander that
/// way, rather than wrapping or saturating oddly. `variation` = 0 returns `base` exactly.
fn cirrus_puff_target(base: f32, variation: f32, phase_turns: f32, drift_m: f32) -> f32 {
    (base + variation * cirrus_variation(drift_m, phase_turns)).clamp(0.0, 1.0)
}

/// `WGR_CIRRUS_PUFF=<puffiness>[,<variation>[,<phase>]]` — startup override for the two Sky-tab
/// values plus a phase offset, in turns of the variation's full period.
///
/// The phase exists for the same reason `WGR_CIRRUS` does: the variation is on the SIM clock and a
/// screenshot capture is a few seconds into a mission, so every capture would otherwise sample the
/// same instant of an hour-long cycle and the control could not be photographed at all. `--dev` is
/// not available under `--test-type screenshot` / `--benchmark`, so there is no other way in.
/// Returns None when the variable is absent, so the caller keeps the compiled defaults.
/// `WGR_CIRRUS_LOOK=<amount>[,<match>]` — startup override for the two Sky-tab look sliders.
///
/// Exists for the same reason `WGR_CIRRUS` and `WGR_CIRRUS_PUFF` do: `--benchmark` and
/// `--test-type screenshot` both run `--no-dev`, so the Sky tab cannot be reached, and without an
/// env door neither the cost nor the appearance of these two can be captured at all. Prints what
/// it selected so a benchmark log is self-describing.
fn cirrus_look_from_env() -> Option<(f32, f32)> {
    let raw = std::env::var("WGR_CIRRUS_LOOK").ok()?;
    let mut it = raw.split(',').map(|f| f.trim().parse::<f32>().ok());
    let amount = it.next().flatten().unwrap_or(CIRRUS_AMOUNT_DEFAULT);
    let matched = it.next().flatten().unwrap_or(CIRRUS_MATCH_DEFAULT);
    let v = (amount.clamp(0.0, 1.0), matched.clamp(0.0, 1.0));
    eprintln!(
        "[wgr] sky: cirrus amount {} match-deck {} from WGR_CIRRUS_LOOK",
        v.0, v.1
    );
    Some(v)
}

/// `WGR_CIRRUS_SOFT=<0..1>` — startup override for the second layer's edge softness, for the same
/// reason the other sky overrides exist: `--benchmark` and `--test-type screenshot` run `--no-dev`,
/// so the Sky tab cannot be reached and the control could not otherwise be captured at all.
fn cirrus_soft_from_env() -> Option<f32> {
    let v = std::env::var("WGR_CIRRUS_SOFT")
        .ok()?
        .trim()
        .parse::<f32>()
        .ok()?
        .clamp(0.0, 1.0);
    eprintln!("[wgr] sky: cirrus edge softness {} from WGR_CIRRUS_SOFT", v);
    Some(v)
}

fn cirrus_puff_from_env() -> Option<(f32, f32, f32)> {
    let raw = std::env::var("WGR_CIRRUS_PUFF").ok()?;
    let mut it = raw.split(',').map(|f| f.trim().parse::<f32>().ok());
    let puff = it.next().flatten().unwrap_or(CIRRUS_PUFF_DEFAULT);
    let var = it.next().flatten().unwrap_or(CIRRUS_PUFF_VAR_DEFAULT);
    let phase = it.next().flatten().unwrap_or(0.0);
    let v = (
        puff.clamp(0.0, 1.0),
        var.clamp(0.0, 1.0),
        phase.rem_euclid(1.0),
    );
    eprintln!(
        "[wgr] sky: cirrus puffiness {} variation {} phase {} from WGR_CIRRUS_PUFF",
        v.0, v.1, v.2
    );
    Some(v)
}

/// `WGR_MILKYWAY=<0..3>` — startup override for the Milky Way band's weight. 1 is the shipped
/// look, 0 leaves only the point stars. It exists so a capture run (which has no dev panel) can
/// A/B the band against the catalogue stars, and so the band can be turned off on the spot if a
/// world's authored night floor is bright enough to make it read as haze.
fn milky_way_from_env() -> f32 {
    let Ok(raw) = std::env::var("WGR_MILKYWAY") else {
        return 1.0;
    };
    let Ok(v) = raw.trim().parse::<f32>() else {
        eprintln!("[wgr] sky: WGR_MILKYWAY='{raw}' is not a number - ignoring");
        return 1.0;
    };
    let v = v.clamp(0.0, 3.0);
    eprintln!("[wgr] sky: milky way gain {v} from WGR_MILKYWAY");
    v
}

/// `WGR_STAR_OCCLUSION=<0..1>` — how completely a cloud deck hides the stars behind it.
///
/// 1 (default) is the correct answer and the shipped look: the cloud march is lit by the
/// STAR-FREE sky, so an overcast night has no stars in it and a broken deck keeps only the ones
/// in its gaps. 0 restores the pre-SKY-002 behaviour exactly — the deck's ambient source is the
/// full sky radiance at that pixel, stars included, so every star re-lights the cloud standing in
/// front of it and returns at roughly the cloud's ambient fraction (measured: 0.65 of the clear-sky
/// value under a 0.95-coverage overcast, which is why "clouds don't hide the stars" was the
/// report). Anything between the two is a straight lerp on the star term alone.
///
/// An env knob rather than a dev slider because the whole path from a Dev Tools control to this
/// value runs through EngineWgpu.cpp, which this change was not allowed to touch. The master gain
/// (Dev Tools -> Sky -> Stars -> "Star brightness") still turns the night sky off entirely, and
/// `WGR_ABLATE=clouds` still turns the deck off, so both ends of the A/B are reachable from the
/// panel; only the middle needs this.
fn star_cloud_occlusion_from_env() -> f32 {
    let Ok(raw) = std::env::var("WGR_STAR_OCCLUSION") else {
        return 1.0;
    };
    let Ok(v) = raw.trim().parse::<f32>() else {
        eprintln!("[wgr] sky: WGR_STAR_OCCLUSION='{raw}' is not a number - ignoring");
        return 1.0;
    };
    let v = v.clamp(0.0, 1.0);
    eprintln!("[wgr] sky: star cloud-occlusion {v} from WGR_STAR_OCCLUSION");
    v
}

/// `WGR_MOON_CLOUDS=<0..4>` — gain on the MOONLIGHT that reaches the cloud decks. 1 (default) is
/// the physical answer: the deck is lit by exactly the lunar irradiance the moon disc already
/// carries — `moon_params.z * illuminated_fraction`, which is the sun's irradiance times the
/// renderer's moon-to-sun ratio times the ephemeris' own phase function — so cloud and ground
/// agree about how bright the night is, and one Moon-panel slider still moves both.
///
/// 0 restores the pre-SKY-004 behaviour bit for bit: the decks see the sun alone, the sun is below
/// the horizon, and an overcast night is black. That is the A/B, and it is the switch the standing
/// "anything on by default must be switchable" rule asks for.
///
/// An env knob and not a dev slider for the reason `WGR_STAR_OCCLUSION` gives above and no other:
/// every path from a Dev Tools control down to this value runs through EngineWgpu.cpp, which this
/// change was not allowed to touch. Both ENDS remain reachable from the panel today — Dev Tools ->
/// Sky -> Moon -> "Moon brightness scale" at 0 turns the moon off everywhere including here, and
/// "Moonlight intensity" scales the irradiance this reads.
fn moon_cloud_light_from_env() -> f32 {
    let Ok(raw) = std::env::var("WGR_MOON_CLOUDS") else {
        return 1.0;
    };
    let Ok(v) = raw.trim().parse::<f32>() else {
        eprintln!("[wgr] sky: WGR_MOON_CLOUDS='{raw}' is not a number - ignoring");
        return 1.0;
    };
    let v = v.clamp(0.0, 4.0);
    eprintln!("[wgr] sky: moonlight on clouds x{v} from WGR_MOON_CLOUDS");
    v
}

/// `WGR_MOON_CLOUD_BLUE=<0..1>` — a deliberate, declared STYLISTIC choice, default 0 (off).
///
/// Moonlight is reflected sunlight. Its spectrum is the sun's, very slightly reddened by the
/// regolith, and the blue "moonlight" everyone recognises is an artefact of human scotopic vision
/// (rods peak bluer than cones and carry no colour of their own) — it is in the observer, not in
/// the light. So the default tint on the cloud decks is the moon disc's own 1.0/0.97/0.92, and this
/// exists so that anyone who wants the cinematic look can ask for it explicitly and know that is
/// what they asked for, rather than finding a blue constant baked into a "physical" path.
fn moon_cloud_blue_from_env() -> f32 {
    let Ok(raw) = std::env::var("WGR_MOON_CLOUD_BLUE") else {
        return 0.0;
    };
    let Ok(v) = raw.trim().parse::<f32>() else {
        eprintln!("[wgr] sky: WGR_MOON_CLOUD_BLUE='{raw}' is not a number - ignoring");
        return 0.0;
    };
    let v = v.clamp(0.0, 1.0);
    eprintln!("[wgr] sky: stylistic blue shift {v} on moonlit clouds from WGR_MOON_CLOUD_BLUE");
    v
}

pub struct Sky {
    pub fog: crate::layered_fog::LayeredFog,
    // Main fullscreen sky pipeline (targets the scene format).
    sky_pipeline: wgpu::RenderPipeline,
    // Single-sample twin for the resolution-scaled planar reflection.
    planar_sky_pipeline: wgpu::RenderPipeline,
    sky_bind: wgpu::BindGroup,
    // Reflection environment map: same group(0) as the sky pass (reuses sky_bind), fs_sky_env
    // entry, baked into env_view each frame. `_tex` keeps the texture alive.
    env_pipeline: wgpu::RenderPipeline,
    #[allow(dead_code)]
    env_tex: wgpu::Texture,
    env_view: wgpu::TextureView,
    // Tileable 3D cloud noise, sampled by the cloud march (bound into sky_bind). Held to keep the
    // texture alive (the view/sampler are owned by the bind group).
    #[allow(dead_code)]
    cloud_noise_tex: wgpu::Texture,
    // SH-9 projection of the env map into diffuse sky irradiance (sky_sh.wgsl). Computed each frame
    // after the env bake; the buffer is lent to the camera group so lit meshes + terrain read it.
    sh_pipeline: wgpu::ComputePipeline,
    sh_bind: wgpu::BindGroup,
    sh_buf: wgpu::Buffer,
    // LUT build pipelines (target LUT_FORMAT) + their target views/binds.
    transmittance_pipeline: wgpu::RenderPipeline,
    transmittance_bind: wgpu::BindGroup,
    transmittance_view: wgpu::TextureView,
    multiscatter_pipeline: wgpu::RenderPipeline,
    multiscatter_bind: wgpu::BindGroup,
    multiscatter_view: wgpu::TextureView,
    // Aerial-perspective froxel volume + its compute fill (see cs_froxel in sky.wgsl).
    // The `_tex` handle is held only to keep the texture alive; both bind groups view it.
    froxel_pipeline: wgpu::ComputePipeline,
    froxel_bind: wgpu::BindGroup,
    #[allow(dead_code)]
    froxel_tex: wgpu::Texture,
    froxel_view: wgpu::TextureView,
    cloud_shadow_tex: wgpu::Texture,
    cloud_shadow_view: wgpu::TextureView,
    cloud_shadow_pipeline: wgpu::ComputePipeline,
    smoke_shadow_header_buf: wgpu::Buffer,
    smoke_shadow_blob_buf: wgpu::Buffer,
    smoke_shadow_blobs: Vec<SmokeShadowBlob>,
    smoke_shadow_strength: f32,
    cloud_shadow_strength: f32,
    /// World span (metres) of the cloud shadow map's square, chosen per frame from the camera's
    /// draw distance. Always a power-of-two multiple of `CLOUD_SHADOW_SPAN_MIN`, so every level's
    /// texel grid is a subset of every finer level's — see `select_cloud_shadow_span`.
    cloud_shadow_span: f32,
    cloud_shadow_map_params: [f32; 4],
    star_intensity: f32,
    /// Milky Way band gain, written to the sky UBO's `cirrus.w`. That lane was documented
    /// "reserved (0)" and read by nobody -- the same spare-lane trick as `cirrus_mode` on
    /// `output.w`, taken for the same reason: WgrSkyLook's size is part of the ABI handshake and
    /// growing it moves the assert. 0 turns the band off and leaves the point stars alone.
    /// Startup override: `WGR_MILKYWAY=<0..3>`.
    milky_way: f32,
    /// SKY-002: how much of the star field is withheld from the sky radiance the CLOUD march uses
    /// as its ambient source, written to the sky UBO's `night_sky.x`. 1 = a deck hides the stars
    /// behind it; 0 = the pre-SKY-002 behaviour. `WGR_STAR_OCCLUSION=<0..1>`.
    star_cloud_occlusion: f32,
    /// SKY-004: gain on the moonlight reaching the cloud decks, written to `night_sky.y`.
    /// 1 = the physical lunar irradiance the moon disc already carries; 0 = the pre-SKY-004
    /// behaviour (sun-only decks, black overcast night). `WGR_MOON_CLOUDS=<0..4>`.
    moon_cloud_light: f32,
    /// SKY-004: stylistic blue shift on that light only, written to `night_sky.z`. 0 (default) is
    /// physical — moonlight is reflected sunlight. `WGR_MOON_CLOUD_BLUE=<0..1>`.
    moon_cloud_blue: f32,
    /// The star-catalogue buffers. Only ever read through `sky_bind`; held here so the buffers
    /// outlive the bind group that references them.
    _star_cell_buf: wgpu::Buffer,
    _star_list_buf: wgpu::Buffer,
    /// Lens-flare gain. 0 disables the effect outright (the shader early-outs).
    lens_flare: f32,
    ocean_level: Option<f32>,
    /// Second cloud layer (high cirrus) mode, written to the sky UBO's `output.w`:
    /// 0 = off, 1 = flat sheet, 2 = thin-shell volumetric march. Rides the spare lane of an
    /// existing vec4 rather than growing WgrSkyLook, for the reason stated on
    /// `wgr_set_cloud_shadow_strength`: the look struct's size is part of the ABI handshake.
    cirrus_mode: f32,
    /// Cirrus PUFFINESS: the authored base (Sky tab), 0 = drawn-out fibrous veil, 1 = lumpy
    /// cirrocumulus. See sky.wgsl's PUFFINESS block for what the shader does with it.
    cirrus_puff: f32,
    /// How far the layer drifts either side of that on the weather clock. 0 = perfectly steady.
    cirrus_puff_var: f32,
    /// Phase offset (turns) for the variation — capture/diagnostic only, see `cirrus_puff_from_env`.
    cirrus_puff_phase: f32,
    /// The value actually uploaded (cloud4.w): the target eased frame to frame, so no slider can
    /// step the sky's shape. Seeded at the base so the first frame does not fade in from nowhere.
    cirrus_puff_now: f32,
    /// Cirrus AMOUNT (Sky tab): 0 = a few separated wisps, 0.5 = the shipped coverage, 1 = a
    /// continuous veil. Coverage first, optical depth a little — see sky.wgsl's AMOUNT block.
    cirrus_amount: f32,
    /// Cirrus MATCH (Sky tab): 0 = high ice cloud, 1 = as close to the cumulus deck's altitude,
    /// feature size, depth, opacity and phase as a shell march gets. See sky.wgsl's MATCH block.
    cirrus_match: f32,
    /// The two values actually uploaded (`cirrus.xy`), eased exactly as `cirrus_puff_now` is and
    /// for the same reason: MATCH moves the layer's ALTITUDE by kilometres, so an un-eased drag
    /// would teleport a whole cloud deck across the sky rather than sliding it.
    cirrus_amount_now: f32,
    cirrus_match_now: f32,
    /// Second-layer EDGE SOFTNESS (Sky tab): 0 = the layer as it rendered before this existed,
    /// 1 = big masses carry a deep soft fringe instead of a defined silhouette. Weighted by local
    /// coverage in the shader, so it reaches the banks and not the isolated wisps.
    cirrus_soft: f32,
    /// Eased, for the same reason the others are: this changes every cloud edge on screen at once.
    cirrus_soft_now: f32,
    // Group(1) of cs_froxel: the terrain sun-shadow mask, rebuilt each frame (the mask
    // texture is Terrain-owned and regenerated) from the lent view + these two.
    froxel_shadow_layout: wgpu::BindGroupLayout,
    mask_sampler: wgpu::Sampler,
    mapping_buf: wgpu::Buffer,
    csm_cmp_sampler: wgpu::Sampler,
    csm_ubo: wgpu::Buffer,
    uniform_buf: wgpu::Buffer,
    // Phase 1 depth-aware over-scene clouds: fs_cloud marches at LOW RES into cloud_lo (bounding at
    // the resolved scene depth via group(1)), then cloud_composite blends it over the lit scene.
    // cloud_lo resizes with the scene. Only used on the HDR (linear) path.
    cloud_pipeline: wgpu::RenderPipeline,
    cloud_temporal_view: Option<wgpu::TextureView>,
    /// REN-SKY-004: the same march at a low step count, for the planar reflection only.
    cloud_pipeline_reflection: wgpu::RenderPipeline,
    cloud_depth_layout: wgpu::BindGroupLayout,
    // PERF: a SIZE-KEYED CACHE, not one slot. render_cloud is called twice per frame at two
    // different sizes — once for the planar reflection (half of the half-res planar target,
    // i.e. a quarter of the screen) and once for the over-scene march (half the screen). A
    // single slot keyed on the requested size therefore missed on EVERY call and destroyed +
    // reallocated a multi-megabyte HDR texture twice per frame, forever. Keyed storage makes
    // both calls hit after the first frame; the cache is capped because only a handful of
    // distinct sizes can ever be asked for (screen/2 and planar/2, plus their post-resize
    // predecessors).
    cloud_lo: Vec<CloudLoTarget>,
    cloud_composite_pipeline: wgpu::RenderPipeline,
    cloud_composite_planar_pipeline: wgpu::RenderPipeline,
    cloud_composite_layout: wgpu::BindGroupLayout,
    cloud_composite_bind: Option<wgpu::BindGroup>,
    cloud_composite_sampler: wgpu::Sampler,
    fog_cloud_controls: [wgpu::Buffer;2],
    // 1 = scene target is the linear HDR texture (tonemap resolves later); 0 =
    // LDR-direct, so the sky self-tonemaps. Fixed at construction with the format.
    linear: f32,
    // LUTs are rebuilt only when the atmosphere key changes (None = never built).
    last_lut: Option<LutKey>,
    lut_dirty: bool,
}

// One cached low-res cloud march target. `_tex` keeps the allocation alive for `view`.
struct CloudLoTarget {
    size: (u32, u32),
    _tex: wgpu::Texture,
    view: wgpu::TextureView,
    // The depth view the two cached binds below were built against; identity-compared so a
    // resize (which is the only thing that replaces it) rebuilds them and nothing else does.
    depth_src: Option<wgpu::TextureView>,
    layer_fog: bool,
    composite_bind: Option<wgpu::BindGroup>,
    depth_bind: Option<wgpu::BindGroup>,
}

// How many distinct low-res cloud sizes to keep. Two are live every frame (screen/2 and
// planar/2); the spare slots absorb a resize without evicting a live one mid-frame.
const CLOUD_LO_CACHE: usize = 4;

impl Sky {
    pub fn new(
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        color_format: wgpu::TextureFormat,
        sample_count: u32,
    ) -> Self {
        let mut fog_composer=crate::shaders::build_composer();
        let fog=crate::layered_fog::LayeredFog::new(device,queue,&mut fog_composer,color_format,sample_count);
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_sky_shader"),
            source: wgpu::ShaderSource::Naga(std::borrow::Cow::Owned(compose_sky(&mut fog_composer))),
        });

        // Bind-group entry templates (binding numbers match sky.wgsl's globals).
        let uniform_entry = |binding| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::VERTEX | wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Buffer {
                ty: wgpu::BufferBindingType::Uniform,
                has_dynamic_offset: false,
                min_binding_size: None,
            },
            count: None,
        };
        let sampler_entry = wgpu::BindGroupLayoutEntry {
            binding: 1,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
            count: None,
        };
        let tex_entry = |binding| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Texture {
                sample_type: wgpu::TextureSampleType::Float { filterable: true },
                view_dimension: wgpu::TextureViewDimension::D2,
                multisampled: false,
            },
            count: None,
        };

        // Per-pass layouts: transmittance reads only the uniform; multiscatter also
        // reads the transmittance LUT; the main pass reads both LUTs.
        let transmittance_layout =
            device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("wgr_sky_transmittance_layout"),
                entries: &[uniform_entry(0)],
            });
        let multiscatter_layout =
            device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("wgr_sky_multiscatter_layout"),
                entries: &[uniform_entry(0), sampler_entry, tex_entry(2)],
            });
        // Cloud noise: a 3D texture at binding 4 + its own Repeat sampler at binding 6 (binding 5 is
        // the froxel storage image in the shared module). Only fs_sky / fs_sky_env reference these.
        let cloud_tex_entry = wgpu::BindGroupLayoutEntry {
            binding: 4,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Texture {
                sample_type: wgpu::TextureSampleType::Float { filterable: true },
                view_dimension: wgpu::TextureViewDimension::D3,
                multisampled: false,
            },
            count: None,
        };
        let cloud_sampler_entry = wgpu::BindGroupLayoutEntry {
            binding: 6,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
            count: None,
        };
        // Bright-star catalogue (see starcat.rs). Two read-only storage buffers at 10/11: the
        // cube-grid cell index and the star records themselves. Fragment-only -- fs_sky and
        // fs_sky_env are the only entry points that reference them, and the froxel/cloud-shadow
        // compute passes use their own layouts, so nothing else pays for them.
        let star_buf_entry = |binding: u32| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Buffer {
                ty: wgpu::BufferBindingType::Storage { read_only: true },
                has_dynamic_offset: false,
                min_binding_size: None,
            },
            count: None,
        };
        let sky_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_sky_layout"),
            entries: &[
                uniform_entry(0),
                sampler_entry,
                tex_entry(2),
                tex_entry(3),
                cloud_tex_entry,
                cloud_sampler_entry,
                star_buf_entry(10),
                star_buf_entry(11),
                tex_entry(12),
            ],
        });

        // Built once at device creation. The catalogue is a few hundred KB and never changes at
        // runtime, so there is no upload path and no dirty flag: swapping in a different
        // catalogue is a restart, which is what a data file swap is anyway.
        let star_cat = starcat::StarCatalogue::load();
        eprintln!(
            "[wgr] sky: star catalogue {} -- {} cell entries in {} cells",
            star_cat.source,
            star_cat.stars.len(),
            starcat::CELL_COUNT
        );
        let star_cell_buf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("wgr_sky_star_cells"),
            contents: bytemuck::cast_slice(&star_cat.cells),
            usage: wgpu::BufferUsages::STORAGE,
        });
        let star_list_buf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("wgr_sky_star_list"),
            contents: bytemuck::cast_slice(&star_cat.stars),
            usage: wgpu::BufferUsages::STORAGE,
        });
        // SKY-005: the Milky Way, as the NASA SVS Deep Star Maps 2020 band. Compiled in for the
        // same reason the catalogue is -- 4 MB that must never go missing -- and decoded by
        // `milkyway::load`, which is a header read and a memcpy because tools/sky/make_milkyway.py
        // did the real work offline. The renderer deliberately has no image decoder.
        let mw = milkyway::load();
        eprintln!("[wgr] sky: milky way texture {} ({}x{})", mw.source, mw.width, mw.height);
        let mw_tex = device.create_texture_with_data(
            queue,
            &wgpu::TextureDescriptor {
                label: Some("wgr_sky_milkyway"),
                size: wgpu::Extent3d {
                    width: mw.width,
                    height: mw.height,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba16Float,
                usage: wgpu::TextureUsages::TEXTURE_BINDING,
                view_formats: &[],
            },
            wgpu::util::TextureDataOrder::LayerMajor,
            &mw.texels,
        );
        let mw_view = mw_tex.create_view(&wgpu::TextureViewDescriptor::default());
        // Only the main sky pass draws into the (MSAA) scene target; the transmittance +
        // multiscatter LUTs are single-sample offscreen renders, so each pipeline takes its
        // own sample count.
        let make_pipeline = |label: &str,
                             layout: &wgpu::BindGroupLayout,
                             fs: &str,
                             format: wgpu::TextureFormat,
                             samples: u32| {
            let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some(label),
                bind_group_layouts: &[Some(layout)],
                immediate_size: 0,
            });
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some(label),
                layout: Some(&pl),
                vertex: wgpu::VertexState {
                    module: &shader,
                    entry_point: Some("vs_main"),
                    buffers: &[],
                    compilation_options: Default::default(),
                },
                primitive: wgpu::PrimitiveState::default(),
                depth_stencil: None,
                multisample: wgpu::MultisampleState {
                    count: samples,
                    ..Default::default()
                },
                fragment: Some(wgpu::FragmentState {
                    module: &shader,
                    entry_point: Some(fs),
                    targets: &[Some(wgpu::ColorTargetState {
                        format,
                        blend: None,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                    compilation_options: Default::default(),
                }),
                multiview_mask: None,
                cache: None,
            })
        };

        let transmittance_pipeline = make_pipeline(
            "wgr_sky_transmittance",
            &transmittance_layout,
            "fs_transmittance",
            LUT_FORMAT,
            1,
        );
        let multiscatter_pipeline = make_pipeline(
            "wgr_sky_multiscatter",
            &multiscatter_layout,
            "fs_multiscatter",
            LUT_FORMAT,
            1,
        );
        let sky_pipeline =
            make_pipeline("wgr_sky", &sky_layout, "fs_sky", color_format, sample_count);
        let planar_sky_pipeline =
            make_pipeline("wgr_sky_planar", &sky_layout, "fs_sky", color_format, 1);
        // Env-map bake: same group(0) layout as the sky pass, single-sample, LUT_FORMAT target.
        let env_pipeline = make_pipeline("wgr_sky_env", &sky_layout, "fs_sky_env", ENV_FORMAT, 1);
        let env_tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_sky_env"),
            size: wgpu::Extent3d {
                width: ENV_W,
                height: ENV_H,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: ENV_FORMAT,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let env_view = env_tex.create_view(&wgpu::TextureViewDescriptor::default());

        // SH-9 sky-irradiance projection: reads the env map (textureLoad, non-filtering) and writes
        // 9 vec4 RGB coefficients into sh_buf (also bound UNIFORM into the camera group). Zero-init
        // so a read before the first bake (or on the non-sky-lit path, where it's unread) is defined.
        let sh_shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("wgr_sky_sh_shader"),
            source: wgpu::ShaderSource::Wgsl(include_str!("sky_sh.wgsl").into()),
        });
        let sh_buf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("wgr_sky_sh"),
            contents: &[0u8; 18 * 16],
            usage: wgpu::BufferUsages::STORAGE
                | wgpu::BufferUsages::UNIFORM
                | wgpu::BufferUsages::COPY_DST,
        });
        let sh_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_sky_sh_layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: false },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: false },
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
            ],
        });
        let sh_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_sky_sh_bind"),
            layout: &sh_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(&env_view),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: sh_buf.as_entire_binding(),
                },
            ],
        });
        let sh_pipeline = {
            let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("wgr_sky_sh"),
                bind_group_layouts: &[Some(&sh_layout)],
                immediate_size: 0,
            });
            device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
                label: Some("wgr_sky_sh"),
                layout: Some(&pl),
                module: &sh_shader,
                entry_point: Some("cs_sky_sh"),
                compilation_options: Default::default(),
                cache: None,
            })
        };

        let make_lut = |label: &str, w: u32, h: u32| {
            let tex = device.create_texture(&wgpu::TextureDescriptor {
                label: Some(label),
                size: wgpu::Extent3d {
                    width: w,
                    height: h,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: LUT_FORMAT,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                    | wgpu::TextureUsages::TEXTURE_BINDING,
                view_formats: &[],
            });
            tex.create_view(&wgpu::TextureViewDescriptor::default())
        };
        let transmittance_view = make_lut(
            "wgr_sky_transmittance_lut",
            TRANSMITTANCE_W,
            TRANSMITTANCE_H,
        );
        let multiscatter_view = make_lut(
            "wgr_sky_multiscatter_lut",
            MULTISCATTER_SIZE,
            MULTISCATTER_SIZE,
        );

        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_sky_lut_sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });

        // Tileable 3D cloud noise, baked once and uploaded. Repeat sampler so the march can sample
        // arbitrarily far / wind-scrolled coordinates without the analytic-hash precision moire.
        let cloud_noise_tex = device.create_texture_with_data(
            queue,
            &wgpu::TextureDescriptor {
                label: Some("wgr_sky_cloud_noise"),
                size: wgpu::Extent3d {
                    width: NOISE_N,
                    height: NOISE_N,
                    depth_or_array_layers: NOISE_N,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D3,
                format: NOISE_FORMAT,
                usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
                view_formats: &[],
            },
            wgpu::util::TextureDataOrder::LayerMajor,
            &generate_cloud_noise(),
        );
        let cloud_noise_view = cloud_noise_tex.create_view(&wgpu::TextureViewDescriptor {
            label: Some("wgr_sky_cloud_noise_view"),
            dimension: Some(wgpu::TextureViewDimension::D3),
            ..Default::default()
        });
        let cloud_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_sky_cloud_sampler"),
            address_mode_u: wgpu::AddressMode::Repeat,
            address_mode_v: wgpu::AddressMode::Repeat,
            address_mode_w: wgpu::AddressMode::Repeat,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });

        let uniform_buf = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("wgr_sky_uniform"),
            contents: bytemuck::bytes_of(&SkyUniform::zeroed()),
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
        });

        let transmittance_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_sky_transmittance_bind"),
            layout: &transmittance_layout,
            entries: &[wgpu::BindGroupEntry {
                binding: 0,
                resource: uniform_buf.as_entire_binding(),
            }],
        });
        let multiscatter_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_sky_multiscatter_bind"),
            layout: &multiscatter_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: uniform_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::Sampler(&sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::TextureView(&transmittance_view),
                },
            ],
        });
        let sky_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_sky_bind"),
            layout: &sky_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: uniform_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::Sampler(&sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::TextureView(&transmittance_view),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: wgpu::BindingResource::TextureView(&multiscatter_view),
                },
                wgpu::BindGroupEntry {
                    binding: 4,
                    resource: wgpu::BindingResource::TextureView(&cloud_noise_view),
                },
                wgpu::BindGroupEntry {
                    binding: 6,
                    resource: wgpu::BindingResource::Sampler(&cloud_sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 10,
                    resource: star_cell_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 11,
                    resource: star_list_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 12,
                    resource: wgpu::BindingResource::TextureView(&mw_view),
                },
            ],
        });

        // Froxel volume + compute fill. Same atmosphere inputs as the render passes
        // (uniform + both LUTs + sampler) at COMPUTE visibility, plus the 3D volume as a
        // write storage texture at binding 5. The volume is frustum-parameterised (not
        // screen-sized), so it is a fixed 32^3 built once here.
        let froxel_tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_sky_froxel"),
            size: wgpu::Extent3d {
                width: FROXEL_W,
                height: FROXEL_H,
                depth_or_array_layers: FROXEL_D,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D3,
            format: FROXEL_FORMAT,
            usage: wgpu::TextureUsages::STORAGE_BINDING | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let froxel_view = froxel_tex.create_view(&wgpu::TextureViewDescriptor {
            label: Some("wgr_sky_froxel_view"),
            dimension: Some(wgpu::TextureViewDimension::D3),
            ..Default::default()
        });

        // rgba8unorm, not r8unorm: R8Unorm is NOT a core WebGPU storage format, and asking for it
        // produced an invalid texture whose invalid view then failed BOTH the sky and the camera
        // bind groups -- a cascade whose first error names a bind group and never mentions the
        // format. Only .r is used; 512x512x4 = 1 MB. Also filterable when sampled, which the
        // single-channel float formats are not guaranteed to be.
        let cloud_shadow_tex = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("wgr_cloud_shadow"),
            size: wgpu::Extent3d {
                width: CLOUD_SHADOW_DIM,
                height: CLOUD_SHADOW_DIM,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba8Unorm,
            usage: wgpu::TextureUsages::STORAGE_BINDING | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let cloud_shadow_view = cloud_shadow_tex.create_view(&Default::default());

        let compute_uniform = wgpu::BindGroupLayoutEntry {
            binding: 0,
            visibility: wgpu::ShaderStages::COMPUTE,
            ty: wgpu::BindingType::Buffer {
                ty: wgpu::BufferBindingType::Uniform,
                has_dynamic_offset: false,
                min_binding_size: None,
            },
            count: None,
        };
        let compute_sampler = wgpu::BindGroupLayoutEntry {
            binding: 1,
            visibility: wgpu::ShaderStages::COMPUTE,
            ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
            count: None,
        };
        let compute_tex = |binding| wgpu::BindGroupLayoutEntry {
            binding,
            visibility: wgpu::ShaderStages::COMPUTE,
            ty: wgpu::BindingType::Texture {
                sample_type: wgpu::TextureSampleType::Float { filterable: true },
                view_dimension: wgpu::TextureViewDimension::D2,
                multisampled: false,
            },
            count: None,
        };
        let froxel_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("wgr_sky_froxel_layout"),
            entries: &[
                compute_uniform,
                compute_sampler,
                compute_tex(2),
                compute_tex(3),
                wgpu::BindGroupLayoutEntry {
                    binding: 5,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::StorageTexture {
                        access: wgpu::StorageTextureAccess::WriteOnly,
                        format: FROXEL_FORMAT,
                        view_dimension: wgpu::TextureViewDimension::D3,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 7,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::StorageTexture {
                        access: wgpu::StorageTextureAccess::WriteOnly,
                        format: wgpu::TextureFormat::Rgba8Unorm,
                        view_dimension: wgpu::TextureViewDimension::D2,
                    },
                    count: None,
                },
                // cs_cloud_shadow marches the same cloud field the sky raymarch uses, so this
                // compute layout needs the noise volume and its Repeat sampler too. cs_froxel does
                // not touch them, but a layout is per-group and not per-entry-point.
                wgpu::BindGroupLayoutEntry {
                    binding: 4,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D3,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 6,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                // Smoke -> ground shadow: header + blob array, read by cs_cloud_shadow.
                wgpu::BindGroupLayoutEntry {
                    binding: 8,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: true },
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 9,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Storage { read_only: true },
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
            ],
        });
        // Group(1): the terrain sun-shadow mask (texture + sampler) + its world->UV mapping
        // uniform, so cs_froxel can occlude the fog by terrain. The mask is Terrain-owned and
        // lent by view (regenerated on heightmap change), so this bind rebuilds each frame in
        // render_froxel; the sampler + mapping buffer are owned here and created once.
        let froxel_shadow_layout =
            device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("wgr_sky_froxel_shadow_layout"),
                entries: &[
                    compute_tex(0),
                    wgpu::BindGroupLayoutEntry {
                        binding: 1,
                        visibility: wgpu::ShaderStages::COMPUTE,
                        ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 2,
                        visibility: wgpu::ShaderStages::COMPUTE,
                        ty: wgpu::BindingType::Buffer {
                            ty: wgpu::BufferBindingType::Uniform,
                            has_dynamic_offset: false,
                            min_binding_size: wgpu::BufferSize::new(std::mem::size_of::<
                                crate::terrain::TerrainShadowMap,
                            >()
                                as u64),
                        },
                        count: None,
                    },
                    // Cascade shadow depth (D2Array) + comparison sampler + the cascade matrices,
                    // so cs_froxel occludes the fog by objects/terrain casters for crisp shafts.
                    wgpu::BindGroupLayoutEntry {
                        binding: 3,
                        visibility: wgpu::ShaderStages::COMPUTE,
                        ty: wgpu::BindingType::Texture {
                            sample_type: wgpu::TextureSampleType::Depth,
                            view_dimension: wgpu::TextureViewDimension::D2Array,
                            multisampled: false,
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 4,
                        visibility: wgpu::ShaderStages::COMPUTE,
                        ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Comparison),
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 5,
                        visibility: wgpu::ShaderStages::COMPUTE,
                        ty: wgpu::BindingType::Buffer {
                            ty: wgpu::BufferBindingType::Uniform,
                            has_dynamic_offset: false,
                            min_binding_size: wgpu::BufferSize::new(std::mem::size_of::<
                                crate::ffi::WgrCameraShadow,
                            >()
                                as u64),
                        },
                        count: None,
                    },
                ],
            });
        let csm_cmp_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_sky_froxel_csm_sampler"),
            compare: Some(wgpu::CompareFunction::LessEqual),
            ..Default::default()
        });
        let csm_ubo = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_sky_froxel_csm"),
            size: std::mem::size_of::<crate::ffi::WgrCameraShadow>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let mask_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_sky_froxel_mask_sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            address_mode_w: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });
        let mapping_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_sky_froxel_mapping"),
            size: std::mem::size_of::<crate::terrain::TerrainShadowMap>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let froxel_pipeline = {
            let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("wgr_sky_froxel"),
                bind_group_layouts: &[Some(&froxel_layout), Some(&froxel_shadow_layout)],
                immediate_size: 0,
            });
            device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
                label: Some("wgr_sky_froxel"),
                layout: Some(&pl),
                module: &shader,
                entry_point: Some("cs_froxel"),
                compilation_options: Default::default(),
                cache: None,
            })
        };
        // Same group(0) as the froxel fill -- it needs the sky uniform and the cloud noise, and
        // reusing the layout avoids a second bind group holding the same four resources. It takes
        // only group(0), so the pipeline layout stops there.
        let cloud_shadow_pipeline = {
            let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("wgr_cloud_shadow"),
                bind_group_layouts: &[Some(&froxel_layout)],
                immediate_size: 0,
            });
            device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
                label: Some("wgr_cloud_shadow"),
                layout: Some(&pl),
                module: &shader,
                entry_point: Some("cs_cloud_shadow"),
                compilation_options: Default::default(),
                cache: None,
            })
        };
        let smoke_shadow_header_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_smoke_shadow_header"),
            size: std::mem::size_of::<SmokeShadowHeader>() as u64,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let smoke_shadow_blob_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_smoke_shadow_blobs"),
            size: (std::mem::size_of::<SmokeShadowBlob>() * SMOKE_SHADOW_MAX_BLOBS) as u64,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let froxel_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_sky_froxel_bind"),
            layout: &froxel_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 8,
                    resource: smoke_shadow_header_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 9,
                    resource: smoke_shadow_blob_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: uniform_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::Sampler(&sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::TextureView(&transmittance_view),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: wgpu::BindingResource::TextureView(&multiscatter_view),
                },
                wgpu::BindGroupEntry {
                    binding: 5,
                    resource: wgpu::BindingResource::TextureView(&froxel_view),
                },
                wgpu::BindGroupEntry {
                    binding: 7,
                    resource: wgpu::BindingResource::TextureView(&cloud_shadow_view),
                },
                wgpu::BindGroupEntry {
                    binding: 4,
                    resource: wgpu::BindingResource::TextureView(&cloud_noise_view),
                },
                wgpu::BindGroupEntry {
                    binding: 6,
                    resource: wgpu::BindingResource::Sampler(&cloud_sampler),
                },
            ],
        });

        // ---- Phase 1: depth-aware over-scene cloud pass + composite ----
        // fs_cloud: low-res march (group(0) = the sky bind; group(1) = the resolved scene depth).
        let cloud_depth_layout =
            device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("wgr_cloud_depth_layout"),
                entries: &[wgpu::BindGroupLayoutEntry {
                    binding: 6,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Depth,
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                }],
            });
        // WGR_CLOUD_STEPS / WGR_CLOUD_LIGHT_STEPS: primary and light-march sample caps for the
        // volumetric cloud pass. Defaults are the shader's own 128 / 4, so unset changes nothing.
        // REN-SKY-001 measured 128 -> 64 -> 32 at 2.92 / 1.85 / 1.24 ms on perf_combat; 64 shipped
        // as the default for one build and the owner preferred the 128 picture on sight, so 128
        // it stays. WGR_CLOUD_STEPS=64 is the lever for a slow GPU (~1 ms), 32 shows grain.
        // These exist because the pass measured 7.97 ms -- 23% of the GPU frame, the largest
        // single item in the renderer, and until now ungated by anything: no tier, no env var.
        // Clamped rather than trusted: 0 primary steps is a divide-by-zero shaped question in
        // the march and a negative light count is an infinite loop.
        let cloud_steps = std::env::var("WGR_CLOUD_STEPS")
            .ok()
            .and_then(|v| v.parse::<f64>().ok())
            .unwrap_or(128.0)
            .clamp(8.0, 256.0);
        let cloud_light_steps = std::env::var("WGR_CLOUD_LIGHT_STEPS")
            .ok()
            .and_then(|v| v.parse::<f64>().ok())
            .unwrap_or(4.0)
            .clamp(1.0, 16.0);
        // REN-SKY-002: WGR_CLOUD_LIGHT_COARSE=1 makes the light march sample a two-tap density
        // (no warp, no detail erosion): 2.89 -> 1.84 ms at 128 steps. OFF by default -- the
        // owner preferred the four-tap picture on sight (2026-09-03), so this is the lever for
        // a slow GPU, like WGR_CLOUD_STEPS=64, not the look.
        let cloud_light_coarse = std::env::var("WGR_CLOUD_LIGHT_COARSE")
            .map(|v| v.trim() == "1")
            .unwrap_or(false);
        let cloud_overrides: [(&str, f64); 3] = [
            ("CLOUD_STEPS", cloud_steps),
            ("CLOUD_LIGHT_STEPS", cloud_light_steps),
            ("CLOUD_LIGHT_COARSE", if cloud_light_coarse { 1.0 } else { 0.0 }),
        ];
        // REN-SKY-004: the PLANAR REFLECTION marches the clouds a second time, at 1.31 ms of a
        // 30.98 ms `perf_water` frame -- more than half that scene's whole reflection block. It
        // used the same 128-step pipeline as the sky, which the reflection cannot show: the
        // result is written to a half-resolution buffer, reflected off a displaced FFT surface,
        // and then read through the roughness mip chain. Its own step count is separate, and low.
        //
        // This does NOT touch the sky the owner chose at 128 steps (REN-SKY-001/002) -- it is a
        // second pipeline, used only by the reflection pass. `WGR_CLOUD_STEPS_REFLECTION`
        // overrides it; setting it equal to WGR_CLOUD_STEPS restores the old behaviour.
        let cloud_steps_reflection = std::env::var("WGR_CLOUD_STEPS_REFLECTION")
            .ok()
            .and_then(|v| v.parse::<f64>().ok())
            .unwrap_or(32.0)
            .clamp(8.0, 256.0)
            .min(cloud_steps);
        let cloud_overrides_reflection: [(&str, f64); 3] = [
            ("CLOUD_STEPS", cloud_steps_reflection),
            ("CLOUD_LIGHT_STEPS", cloud_light_steps),
            ("CLOUD_LIGHT_COARSE", if cloud_light_coarse { 1.0 } else { 0.0 }),
        ];
        let mut build_cloud_pipeline = |label: &str, overrides: &[(&str, f64)]| {
            let pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some(label),
                bind_group_layouts: &[Some(&sky_layout), Some(&cloud_depth_layout)],
                immediate_size: 0,
            });
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some(label),
                layout: Some(&pl),
                vertex: wgpu::VertexState {
                    module: &shader,
                    entry_point: Some("vs_main"),
                    buffers: &[],
                    compilation_options: Default::default(),
                },
                primitive: wgpu::PrimitiveState::default(),
                depth_stencil: None,
                multisample: wgpu::MultisampleState::default(), // low-res buffer is single-sample
                fragment: Some(wgpu::FragmentState {
                    module: &shader,
                    entry_point: Some("fs_cloud"),
                    targets: &[Some(wgpu::ColorTargetState {
                        format: crate::HDR_FORMAT,
                        blend: None,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                    compilation_options: wgpu::PipelineCompilationOptions {
                        constants: overrides,
                        ..Default::default()
                    },
                }),
                multiview_mask: None,
                cache: None,
            })
        };
        let cloud_pipeline = build_cloud_pipeline("wgr_cloud", &cloud_overrides);
        let cloud_pipeline_reflection =
            build_cloud_pipeline("wgr_cloud_reflection", &cloud_overrides_reflection);
        drop(build_cloud_pipeline);

        // cloud_composite: upsample the low-res buffer + premultiplied blend over the MSAA scene.
        let composite_shader = crate::shaders::make_module(device,&mut fog_composer,
            "wgr_cloud_composite_shader",include_str!("cloud_composite.wgsl"),"cloud_composite.wgsl");
        let fog_cloud_controls = [0.0f32,1.0].map(|on|device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label:Some("wgr_cloud_layer_fog_control"),contents:bytemuck::cast_slice(&[on,0.0,0.0,0.0]),
            usage:wgpu::BufferUsages::UNIFORM }));
        let cloud_composite_layout =
            device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("wgr_cloud_composite_layout"),
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
                    // Full-res resolved scene depth, for the depth-aware (bilateral) upsample.
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
                    wgpu::BindGroupLayoutEntry { binding:3,visibility:wgpu::ShaderStages::FRAGMENT,
                        ty:wgpu::BindingType::Texture {sample_type:wgpu::TextureSampleType::Float {filterable:true},view_dimension:wgpu::TextureViewDimension::D3,multisampled:false},count:None },
                    wgpu::BindGroupLayoutEntry { binding:4,visibility:wgpu::ShaderStages::FRAGMENT,
                        ty:wgpu::BindingType::Buffer {ty:wgpu::BufferBindingType::Uniform,has_dynamic_offset:false,min_binding_size:None},count:None },
                    wgpu::BindGroupLayoutEntry { binding:5,visibility:wgpu::ShaderStages::FRAGMENT,
                        ty:wgpu::BindingType::Buffer {ty:wgpu::BufferBindingType::Uniform,has_dynamic_offset:false,min_binding_size:None},count:None },
                ],
            });
        let cloud_composite_pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("wgr_cloud_composite"),
            bind_group_layouts: &[Some(&cloud_composite_layout)],
            immediate_size: 0,
        });
        let make_cloud_composite = |label: &str, samples: u32| {
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some(label),
                layout: Some(&cloud_composite_pl),
                vertex: wgpu::VertexState {
                    module: &composite_shader,
                    entry_point: Some("vs_main"),
                    buffers: &[],
                    compilation_options: Default::default(),
                },
                primitive: wgpu::PrimitiveState::default(),
                depth_stencil: None,
                multisample: wgpu::MultisampleState {
                    count: samples,
                    ..Default::default()
                },
                fragment: Some(wgpu::FragmentState {
                    module: &composite_shader,
                    entry_point: Some("fs_main"),
                    targets: &[Some(wgpu::ColorTargetState {
                        format: color_format,
                        // out = inscatter*1 + scene*src.a (src.a = cloud transmittance).
                        blend: Some(wgpu::BlendState {
                            color: wgpu::BlendComponent {
                                src_factor: wgpu::BlendFactor::One,
                                dst_factor: wgpu::BlendFactor::SrcAlpha,
                                operation: wgpu::BlendOperation::Add,
                            },
                            alpha: wgpu::BlendComponent {
                                src_factor: wgpu::BlendFactor::Zero,
                                dst_factor: wgpu::BlendFactor::One,
                                operation: wgpu::BlendOperation::Add,
                            },
                        }),
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                    compilation_options: Default::default(),
                }),
                multiview_mask: None,
                cache: None,
            })
        };
        let cloud_composite_pipeline = make_cloud_composite("wgr_cloud_composite", sample_count);
        let cloud_composite_planar_pipeline = make_cloud_composite("wgr_cloud_composite_planar", 1);
        let cloud_composite_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("wgr_cloud_composite_sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });

        let linear = if color_format == crate::HDR_FORMAT {
            1.0
        } else {
            0.0
        };
        // Read once — it logs, and the three fields must agree with each other.
        let puff_env = cirrus_puff_from_env();
        let look_env = cirrus_look_from_env();
        let soft_env = cirrus_soft_from_env();

        Self {
            fog,
            sky_pipeline,
            planar_sky_pipeline,
            sky_bind,
            env_pipeline,
            env_tex,
            env_view,
            cloud_noise_tex,
            cloud_pipeline,
            cloud_temporal_view: None,
            cloud_pipeline_reflection,
            cloud_depth_layout,
            cloud_lo: Vec::new(),
            cloud_composite_pipeline,
            cloud_composite_planar_pipeline,
            cloud_composite_layout,
            cloud_composite_bind: None,
            cloud_composite_sampler,
            fog_cloud_controls,
            sh_pipeline,
            sh_bind,
            sh_buf,
            transmittance_pipeline,
            transmittance_bind,
            transmittance_view,
            multiscatter_pipeline,
            multiscatter_bind,
            multiscatter_view,
            froxel_pipeline,
            froxel_bind,
            froxel_tex,
            cloud_shadow_tex,
            cloud_shadow_view,
            cloud_shadow_pipeline,
            smoke_shadow_header_buf,
            smoke_shadow_blob_buf,
            smoke_shadow_blobs: Vec::with_capacity(SMOKE_SHADOW_MAX_BLOBS),
            smoke_shadow_strength: 1.0,
            // Default ON at a moderate strength: clouds that do not shade the ground are the
            // thing a player notices, and the pass is one 512x512 dispatch.
            cloud_shadow_strength: 0.85,
            // Stars on by default: the night sky had nothing in it at all, which is the actual
            // complaint. Additive on top of the authored night floor, so it brightens nothing
            // during the day -- night_blend gates it to zero while the sun is up.
            star_intensity: 1.0,
            // Milky Way on by default at its authored strength. It is scaled by the same
            // "Star brightness" slider as the point stars, so the dev panel can still take the
            // whole night sky to zero; this lane only sets the band's weight against the stars.
            milky_way: milky_way_from_env(),
            star_cloud_occlusion: star_cloud_occlusion_from_env(),
            moon_cloud_light: moon_cloud_light_from_env(),
            moon_cloud_blue: moon_cloud_blue_from_env(),
            _star_cell_buf: star_cell_buf,
            _star_list_buf: star_list_buf,
            // Matches Engine::SkySettings' safe default.  The C++ side normally pushes this on
            // the first frame, but keep the renderer's own initial state safe as well.
            lens_flare: 0.0,
            ocean_level: None,
            // Second cloud layer on, volumetric, matching Engine::SkySettings' defaults. The env
            // override exists so the two modes can be A/B'd from a --benchmark run, which has no
            // dev panel: WGR_CIRRUS=0 off, 1 flat, 2 volumetric.
            cirrus_mode: cirrus_mode_from_env(),
            // Cirrus puffiness + its self-variation, matching Engine::SkySettings' defaults. The
            // env override exists so a capture (which has no dev panel) can select a puffiness AND
            // a point in the variation cycle: WGR_CIRRUS_PUFF=puff[,variation[,phase]].
            cirrus_puff: puff_env.map_or(CIRRUS_PUFF_DEFAULT, |v| v.0),
            cirrus_puff_var: puff_env.map_or(CIRRUS_PUFF_VAR_DEFAULT, |v| v.1),
            cirrus_puff_phase: puff_env.map_or(0.0, |v| v.2),
            cirrus_puff_now: puff_env.map_or(CIRRUS_PUFF_DEFAULT, |v| v.0),
            cirrus_amount: look_env.map_or(CIRRUS_AMOUNT_DEFAULT, |v| v.0),
            cirrus_match: look_env.map_or(CIRRUS_MATCH_DEFAULT, |v| v.1),
            cirrus_amount_now: look_env.map_or(CIRRUS_AMOUNT_DEFAULT, |v| v.0),
            cirrus_match_now: look_env.map_or(CIRRUS_MATCH_DEFAULT, |v| v.1),
            cirrus_soft: soft_env.unwrap_or(CIRRUS_SOFT_DEFAULT),
            cirrus_soft_now: soft_env.unwrap_or(CIRRUS_SOFT_DEFAULT),
            cloud_shadow_span: CLOUD_SHADOW_SPAN_MIN,
            cloud_shadow_map_params: [0.0, 0.0, 1.0 / CLOUD_SHADOW_SPAN_MIN, 0.85],
            froxel_view,
            froxel_shadow_layout,
            mask_sampler,
            mapping_buf,
            csm_cmp_sampler,
            csm_ubo,
            uniform_buf,
            linear,
            last_lut: None,
            lut_dirty: true,
        }
    }

    /// World mapping for the cloud sun-transmittance map: a square of `cloud_shadow_span`
    /// metres centred on the camera, with the min corner SNAPPED to the map's own texel grid.
    ///
    /// The snap is the whole trick. An origin that tracks the camera continuously
    /// re-rasterises the same clouds into different texels every frame, and the eye reads
    /// that as the shadows crawling over the ground independently of the wind. Snapping
    /// means a texel keeps covering the same square of world until it leaves the map.
    fn cloud_shadow_mapping(&self, cam_pos: [f32; 4]) -> [f32; 4] {
        let span = self.cloud_shadow_span;
        let texel = span / CLOUD_SHADOW_DIM as f32;
        let snap = |v: f32| (v / texel).floor() * texel;
        [
            snap(cam_pos[0] - span * 0.5),
            snap(cam_pos[2] - span * 0.5),
            1.0 / span,
            self.cloud_shadow_strength,
        ]
    }

    /// How wide the map's square has to be for a camera that can draw ground `draw_distance`
    /// metres away.
    ///
    /// The map is camera-centred and the ground lookup is straight down (`cloud_sun_shadow`),
    /// so the requirement is purely horizontal: reach the furthest ground that can exist on
    /// screen, in every direction. Altitude adds nothing to it — the ground below a climbing
    /// aircraft stays at the map's centre — which is why this is driven by draw distance and
    /// not by height, even though flying is where the seam was reported. Flying is simply
    /// where people wind the view distance out.
    ///
    /// **Powers of two, and only powers of two.** `cloud_shadow_mapping` snaps the origin to
    /// `span / 512`, so doubling the span doubles the texel: every coarser grid line is also a
    /// finer grid line, and a level change re-rasterises the field ONCE rather than sliding it.
    /// A span that tracked the draw distance continuously would re-snap to a different grid
    /// every frame and reintroduce exactly the crawl the snapping exists to prevent.
    ///
    /// **Hysteresis on the way down.** The draw distance comes from the scene fog range, which
    /// the engine widens and narrows with the weather, so it drifts across a level boundary on
    /// its own. Growing is immediate (a seam is the failure being fixed); shrinking waits until
    /// the requirement is comfortably inside the smaller level, so a drifting fog range cannot
    /// flap the map between two rasterisations every few frames.
    fn select_cloud_shadow_span(&self, draw_distance: f32) -> f32 {
        cloud_shadow_span_for(self.cloud_shadow_span, draw_distance)
    }

    /// Fix this frame's cloud-shadow world square BEFORE anything publishes or samples it.
    ///
    /// `upload` used to derive the mapping, but `upload` runs late in `render_frame` -- long
    /// after `gfx3d::prepare` has already baked `cloud_shadow_mapping_current()` into the
    /// camera UBO. Every sampler therefore ran a whole frame behind the compute pass that
    /// FILLED the map. Snapping makes that lag invisible while the camera is still (the
    /// square does not move) and maximally visible while it flies: on every frame that
    /// crosses one of the 8 m snap lines the two squares differ by exactly one texel, so the
    /// entire shadow field translates 8 m and returns on the next frame -- several times a
    /// second in flight, never at a standstill. That is the reported flicker.
    ///
    /// Call once per frame, from the same camera the sky pass will use. `draw_distance` is that
    /// camera's furthest drawable ground in metres (0 = unknown, keep the current span).
    ///
    /// Returns a line to log when the span LEVEL changed, and nothing otherwise — the level is
    /// invisible from outside and silently halving the map's resolution is the kind of thing a
    /// later quality complaint has no way to trace.
    pub fn begin_cloud_shadow_frame(
        &mut self,
        cam_pos: [f32; 4],
        draw_distance: f32,
    ) -> Option<String> {
        self.cloud_temporal_view = None;
        let span = self.select_cloud_shadow_span(draw_distance);
        let changed = span != self.cloud_shadow_span;
        self.cloud_shadow_span = span;
        self.cloud_shadow_map_params = self.cloud_shadow_mapping(cam_pos);
        changed.then(|| {
            format!(
                "Wgpu: cloud shadow map span {:.0} m ({:.1} m/texel at {}^2) for a {:.0} m draw distance",
                span,
                span / CLOUD_SHADOW_DIM as f32,
                CLOUD_SHADOW_DIM,
                draw_distance
            )
        })
    }

    /// The mapping last uploaded, so the frame bind group can publish the same numbers to
    /// the shaders that SAMPLE the map. Two independently computed mappings would drift.
    pub fn cloud_shadow_mapping_current(&self) -> [f32; 4] {
        self.cloud_shadow_map_params
    }

    pub fn set_ocean_level(&mut self, level: Option<f32>) {
        static ENABLED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        let enabled = *ENABLED.get_or_init(|| std::env::var("WGR_CLOUD_OCEAN_CLIP").as_deref() != Ok("0"));
        self.ocean_level = level.filter(|level| enabled && level.is_finite());
    }

    pub fn set_lens_flare(&mut self, intensity: f32) {
        self.lens_flare = intensity.clamp(0.0, 4.0);
    }

    pub fn set_star_intensity(&mut self, intensity: f32) {
        self.star_intensity = intensity.clamp(0.0, 4.0);
    }

    pub fn set_cloud_shadow_strength(&mut self, strength: f32) {
        self.cloud_shadow_strength = strength.clamp(0.0, 1.0);
    }

    /// Replace this frame's smoke ground-shadow blobs. Called from the FFI once per frame
    /// with whatever the smoke system chose to publish (already capped to
    /// SMOKE_SHADOW_MAX_BLOBS by the caller; truncated here regardless).
    pub fn set_smoke_shadow(&mut self, blobs: &[SmokeShadowBlob], strength: f32) {
        self.smoke_shadow_blobs.clear();
        self.smoke_shadow_blobs
            .extend_from_slice(&blobs[..blobs.len().min(SMOKE_SHADOW_MAX_BLOBS)]);
        self.smoke_shadow_strength = strength.max(0.0);
    }

    /// Second cloud layer (high cirrus): 0 = off, 1 = flat sheet, 2 = thin-shell volumetric march.
    pub fn set_cirrus_mode(&mut self, mode: u32) {
        self.cirrus_mode = (mode.min(2)) as f32;
    }

    /// Cirrus shape: `puffiness` 0 = drawn-out fibrous veil .. 1 = lumpy cirrocumulus (0.5 is the
    /// shipped look), `variation` = how far it wanders from that on its own (0 = perfectly steady).
    ///
    /// Neither takes effect immediately: `upload` eases toward the value they imply, so a slider
    /// drag cannot step the sky. The env override, if present, wins until this is called.
    pub fn set_cirrus_puffiness(&mut self, puffiness: f32, variation: f32) {
        self.cirrus_puff = puffiness.clamp(0.0, 1.0);
        self.cirrus_puff_var = variation.clamp(0.0, 1.0);
    }

    /// Second-layer LOOK. `amount` 0 = a few wisps .. 0.5 = the shipped coverage .. 1 = a
    /// continuous veil; `match_deck` 0 = high ice cloud (shipped) .. 1 = as close to the cumulus
    /// deck below as the shell march gets.
    ///
    /// Neither takes effect immediately — `upload` eases toward both, which matters more here than
    /// it does for puffiness: `match_deck` moves the layer's altitude by kilometres. The env
    /// override, if present, wins until this is called.
    pub fn set_cirrus_look(&mut self, amount: f32, match_deck: f32) {
        self.cirrus_amount = amount.clamp(0.0, 1.0);
        self.cirrus_match = match_deck.clamp(0.0, 1.0);
    }

    /// Second-layer EDGE SOFTNESS: 0 = the layer as it rendered before this existed, 1 = the big
    /// masses carry a deep soft fringe instead of a defined silhouette. Eased in `upload`.
    pub fn set_cirrus_softness(&mut self, softness: f32) {
        self.cirrus_soft = softness.clamp(0.0, 1.0);
    }

    pub fn cloud_shadow_view(&self) -> &wgpu::TextureView {
        &self.cloud_shadow_view
    }

    // Rebuild the sky uniform for this frame and flag the LUTs dirty if the
    // atmosphere parameters changed.
    // Called after this frame's LayeredFog::render and before encoder submit.
    // Queue writes precede the entire encoder, including its earlier froxel
    // dispatch. upload() resets this lane for every camera; unknown/unencoded
    // weather and Legacy mode therefore leave both sky and froxels unchanged.
    pub fn update_fog_chroma(&self,queue:&wgpu::Queue,ready:bool,weather:f32) {
        let strength=if ready && weather.is_finite() && (0.0..=1.0).contains(&weather) {
            0.85*(weather*4.0).min(1.0)
        } else {0.0};
        let offset=std::mem::offset_of!(SkyUniform,night_sky) as u64+12;
        queue.write_buffer(&self.uniform_buf,offset,bytemuck::bytes_of(&strength));
    }
    pub fn upload(
        &mut self,
        queue: &wgpu::Queue,
        sky: &WgrSky,
        inv_view_proj: [[f32; 4]; 4],
        cam_pos: [f32; 4],
        shadow_map: &crate::terrain::TerrainShadowMap,
        csm: &crate::ffi::WgrCameraShadow,
    ) {
        // Smoke ground-shadow blobs for cs_cloud_shadow. Written every frame, even when
        // empty (count = 0), so a plume that has just died stops shadowing at once.
        {
            let header = SmokeShadowHeader {
                count: self.smoke_shadow_blobs.len() as u32,
                strength: self.smoke_shadow_strength,
                _pad0: 0.0,
                _pad1: 0.0,
            };
            queue.write_buffer(
                &self.smoke_shadow_header_buf,
                0,
                bytemuck::bytes_of(&header),
            );
            if !self.smoke_shadow_blobs.is_empty() {
                queue.write_buffer(
                    &self.smoke_shadow_blob_buf,
                    0,
                    bytemuck::cast_slice(&self.smoke_shadow_blobs),
                );
            }
        }

        let key = lut_key(sky);
        if self.last_lut != Some(key) {
            self.last_lut = Some(key);
            self.lut_dirty = true;
        }
        // Cirrus puffiness for this upload. cloud_evolve.z (cloud4.z) is the world's weather-drift
        // clock in metres, which is the same clock the cloud shapes themselves evolve on, so the
        // layer breathes with the weather and freezes when the sim does.
        let target = cirrus_puff_target(
            self.cirrus_puff,
            self.cirrus_puff_var,
            self.cirrus_puff_phase,
            sky.cloud4[2],
        );
        self.cirrus_puff_now += (target - self.cirrus_puff_now) * CIRRUS_PUFF_EASE;
        // Same ease, same rate, for the two look sliders. They have no time variation of their own
        // (they are authored settings, not weather), so the target IS the setting.
        self.cirrus_amount_now += (self.cirrus_amount - self.cirrus_amount_now) * CIRRUS_PUFF_EASE;
        self.cirrus_match_now += (self.cirrus_match - self.cirrus_match_now) * CIRRUS_PUFF_EASE;
        self.cirrus_soft_now += (self.cirrus_soft - self.cirrus_soft_now) * CIRRUS_PUFF_EASE;
        let u = SkyUniform {
            inv_view_proj,
            sun_dir: sky.sun_dir,
            moon_dir: sky.moon_dir,
            rayleigh: sky.rayleigh,
            mie: sky.mie,
            ground_albedo: sky.ground_albedo,
            params: sky.params,
            control: sky.control,
            fog_color: sky.fog_color,
            night_zenith: sky.night_zenith,
            night_horizon: sky.night_horizon,
            night_params: sky.night_params,
            cloud0: sky.cloud0,
            cloud1: sky.cloud1,
            cloud2: sky.cloud2,
            cloud3: sky.cloud3,
            // cloud4.w is the evolution vec4's pad lane (C++ sends 0) — the renderer writes the
            // live cirrus puffiness into it. Same "spare lane" trick as output.w's cirrus mode,
            // and for the same reason: no FFI struct changes size, so no ABI assert moves.
            cloud4: [
                sky.cloud4[0],
                sky.cloud4[1],
                sky.cloud4[2],
                self.cirrus_puff_now,
            ],
            output: [
                self.linear,
                self.star_intensity,
                self.lens_flare,
                self.cirrus_mode,
            ],
            cam_pos: [
                cam_pos[0], cam_pos[1], cam_pos[2],
                self.ocean_level.map_or(0.0, |level| (cam_pos[1] - level).max(0.0)),
            ],
            // Whatever begin_cloud_shadow_frame fixed for this frame -- NOT a fresh
            // derivation from this call's cam_pos. This is the identical number the camera
            // bind group has already published to every shader that samples the map, so
            // cs_cloud_shadow fills the exact square those shaders will read. It also stops
            // the reflected-camera upload (which runs first, with a mirrored cam_pos) from
            // moving the square out from under the main pass.
            cloud_shadow: self.cloud_shadow_map_params,
            moon_params: sky.moon_params,
            moon_sun: sky.moon_sun,
            cirrus: [
                self.cirrus_amount_now,
                self.cirrus_match_now,
                self.cirrus_soft_now,
                // w was "reserved (0)" and read by nobody -- now the Milky Way band's gain.
                // See `milky_way` in the struct above for why it rides here.
                self.milky_way,
            ],
            night_sky: [
                self.star_cloud_occlusion,
                self.moon_cloud_light,
                self.moon_cloud_blue,
                0.0,
            ],
        };
        queue.write_buffer(&self.uniform_buf, 0, bytemuck::bytes_of(&u));
        // Terrain sun-shadow mask mapping for cs_froxel's occlusion lookup (own copy so
        // the froxel fill doesn't depend on the graphics camera group's buffer).
        queue.write_buffer(&self.mapping_buf, 0, bytemuck::bytes_of(shadow_map));
        // The main camera's cascade matrices, for the froxel's near-field CSM occlusion.
        queue.write_buffer(&self.csm_ubo, 0, bytemuck::bytes_of(csm));
    }

    // Record the transmittance + multiscatter LUT passes when the atmosphere changed.
    // Must run before the main sky pass on the same encoder. `upload` must precede it.
    pub fn render_luts(&mut self, encoder: &mut wgpu::CommandEncoder) {
        if !self.lut_dirty {
            return;
        }
        self.lut_dirty = false;
        encoder.push_debug_group("wgr_sky_luts");
        self.lut_pass(
            encoder,
            "wgr_sky_transmittance",
            &self.transmittance_pipeline,
            &self.transmittance_bind,
            &self.transmittance_view,
        );
        // Multiscatter samples the transmittance LUT just rendered, so it runs after.
        self.lut_pass(
            encoder,
            "wgr_sky_multiscatter",
            &self.multiscatter_pipeline,
            &self.multiscatter_bind,
            &self.multiscatter_view,
        );
        encoder.pop_debug_group();
    }

    fn lut_pass(
        &self,
        encoder: &mut wgpu::CommandEncoder,
        label: &str,
        pipeline: &wgpu::RenderPipeline,
        bind: &wgpu::BindGroup,
        view: &wgpu::TextureView,
    ) {
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some(label),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view,
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
        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, bind, &[]);
        pass.draw(0..3, 0..1);
    }

    // Draw the fullscreen sky into an already-begun render pass targeting the scene.
    pub fn render(&self, pass: &mut wgpu::RenderPass<'_>) {
        pass.set_pipeline(&self.sky_pipeline);
        pass.set_bind_group(0, &self.sky_bind, &[]);
        pass.draw(0..3, 0..1);
    }

    pub fn render_planar(&self, pass: &mut wgpu::RenderPass<'_>) {
        pass.set_pipeline(&self.planar_sky_pipeline);
        pass.set_bind_group(0, &self.sky_bind, &[]);
        pass.draw(0..3, 0..1);
    }

    // Clouds are drawn only on the HDR path (the composite blends linear radiance over the HDR scene)
    // and when coverage is non-zero. lib.rs gates the cloud pass + composite on this.
    pub fn clouds_active(&self, sky: &WgrSky) -> bool {
        self.linear > 0.5 && sky.cloud0[0] > 0.001
    }

    pub fn cloud_temporal_view(&self) -> Option<&wgpu::TextureView> {
        self.cloud_temporal_view.as_ref()
    }

    // Phase 1: march the clouds at LOW RES into cloud_lo, bounding each ray at the resolved scene
    // depth so they occlude terrain / envelop the camera. (Re)allocates cloud_lo at half the scene
    // size on resize and rebuilds the composite bind. `upload` + `render_luts` must have run first.
    // depth_view is the single-sample resolved prepass depth (gfx3d.depth_sample_view()).
    pub fn render_cloud(
        &mut self,
        device: &wgpu::Device,
        encoder: &mut wgpu::CommandEncoder,
        depth_view: &wgpu::TextureView,
        width: u32,
        height: u32,
        reflection: bool,
        layer_fog: bool,
    ) {
        let lo = (width.div_ceil(2).max(1), height.div_ceil(2).max(1));
        // Find-or-create in the size-keyed cache (see the field comment): the planar and the
        // over-scene marches ask for two different sizes every frame, so a single slot
        // reallocated the texture on every call.
        let slot = match self.cloud_lo.iter().position(|t| t.size == lo) {
            Some(i) => i,
            None => {
                let tex = device.create_texture(&wgpu::TextureDescriptor {
                    label: Some("wgr_cloud_lo"),
                    size: wgpu::Extent3d {
                        width: lo.0,
                        height: lo.1,
                        depth_or_array_layers: 1,
                    },
                    mip_level_count: 1,
                    sample_count: 1,
                    dimension: wgpu::TextureDimension::D2,
                    format: crate::HDR_FORMAT,
                    usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                        | wgpu::TextureUsages::TEXTURE_BINDING,
                    view_formats: &[],
                });
                let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
                // Evict oldest first so a window resize cannot grow this without bound.
                if self.cloud_lo.len() >= CLOUD_LO_CACHE {
                    self.cloud_lo.remove(0);
                }
                self.cloud_lo.push(CloudLoTarget {
                    size: lo,
                    _tex: tex,
                    view,
                    depth_src: None,
                    layer_fog: false,
                    composite_bind: None,
                    depth_bind: None,
                });
                self.cloud_lo.len() - 1
            }
        };
        // The composite bind carries the low-res cloud buffer + the scene depth (for the bilateral
        // upsample); group(1) carries the same depth for the march itself. PERF: both used to be
        // rebuilt unconditionally on the claim that "the depth view is regenerated every frame".
        // It is not — Gfx3d::ensure_depth recreates it only on a resize, and the planar target
        // likewise. Compare by identity (wgpu resource equality is identity) and rebuild only
        // when the view genuinely changed. Cached per size slot because the two calls per frame
        // pass two different depth views.
        {
            let target = &mut self.cloud_lo[slot];
            if target.depth_src.as_ref() != Some(depth_view) || target.composite_bind.is_none() || target.layer_fog != layer_fog {
                target.composite_bind =
                    Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                        label: Some("wgr_cloud_composite_bind"),
                        layout: &self.cloud_composite_layout,
                        entries: &[
                            wgpu::BindGroupEntry {
                                binding: 0,
                                resource: wgpu::BindingResource::TextureView(&target.view),
                            },
                            wgpu::BindGroupEntry {
                                binding: 1,
                                resource: wgpu::BindingResource::Sampler(
                                    &self.cloud_composite_sampler,
                                ),
                            },
                            wgpu::BindGroupEntry {
                                binding: 2,
                                resource: wgpu::BindingResource::TextureView(depth_view),
                            },
                            wgpu::BindGroupEntry {binding:3,resource:wgpu::BindingResource::TextureView(&self.fog.view)},
                            wgpu::BindGroupEntry {binding:4,resource:self.fog.consumer.as_entire_binding()},
                            wgpu::BindGroupEntry {binding:5,resource:self.fog_cloud_controls[layer_fog as usize].as_entire_binding()},
                        ],
                    }));
                target.depth_bind = Some(device.create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("wgr_cloud_depth_bind"),
                    layout: &self.cloud_depth_layout,
                    entries: &[wgpu::BindGroupEntry {
                        binding: 6,
                        resource: wgpu::BindingResource::TextureView(depth_view),
                    }],
                }));
                target.depth_src = Some(depth_view.clone());
                target.layer_fog = layer_fog;
            }
        }
        // `composite_cloud` runs later in the frame against whichever target this call filled.
        self.cloud_composite_bind = self.cloud_lo[slot].composite_bind.clone();
        let depth_bind = self.cloud_lo[slot]
            .depth_bind
            .clone()
            .expect("cloud depth bind just built");
        let lo_view = &self.cloud_lo[slot].view;
        if !reflection {
            self.cloud_temporal_view = Some(lo_view.clone());
        }
        encoder.push_debug_group("wgr_cloud");
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("wgr_cloud"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: lo_view,
                depth_slice: None,
                resolve_target: None,
                ops: wgpu::Operations {
                    load: wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT),
                    store: wgpu::StoreOp::Store,
                },
            })],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        pass.set_pipeline(if reflection {
            &self.cloud_pipeline_reflection
        } else {
            &self.cloud_pipeline
        });
        pass.set_bind_group(0, &self.sky_bind, &[]);
        pass.set_bind_group(1, &depth_bind, &[]);
        pass.draw(0..3, 0..1);
        drop(pass);
        encoder.pop_debug_group();
    }

    // Composite the upsampled low-res clouds over the lit scene (premultiplied blend) in an
    // already-begun render pass targeting scene_view. Must run after render_cloud on the same frame.
    pub fn composite_cloud(&self, pass: &mut wgpu::RenderPass<'_>) {
        if let Some(bind) = self.cloud_composite_bind.as_ref() {
            pass.set_pipeline(&self.cloud_composite_pipeline);
            pass.set_bind_group(0, bind, &[]);
            pass.draw(0..3, 0..1);
        }
    }

    pub fn composite_cloud_planar(&self, pass: &mut wgpu::RenderPass<'_>) {
        if let Some(bind) = self.cloud_composite_bind.as_ref() {
            pass.set_pipeline(&self.cloud_composite_planar_pipeline);
            pass.set_bind_group(0, bind, &[]);
            pass.draw(0..3, 0..1);
        }
    }

    // Bake the disc-free sky radiance into the reflection env map (equirect) for this frame. Reuses
    // this frame's sky uniform + LUTs (so `upload` + `render_luts` must have run first) via the same
    // group(0) bind as the sky pass. Cheap (256x128); recorded once per frame before the water pass.
    pub fn render_env(&self, encoder: &mut wgpu::CommandEncoder) {
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("wgr_sky_env"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: &self.env_view,
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
        pass.set_pipeline(&self.env_pipeline);
        pass.set_bind_group(0, &self.sky_bind, &[]);
        pass.draw(0..3, 0..1);
    }

    // The reflection env map view, lent to the water bind group (water look plan Stage 4a).
    pub fn env_view(&self) -> &wgpu::TextureView {
        &self.env_view
    }

    // Project the env map into SH-9 diffuse sky irradiance for this frame. Must run after render_env
    // (reads the freshly-baked env) and before the lit-mesh / terrain passes read `sh_buf`. One tiny
    // dispatch. Recorded on the same encoder so wgpu barriers env-write -> read and sh-write -> read.
    pub fn render_sh(&self, encoder: &mut wgpu::CommandEncoder) {
        encoder.push_debug_group("wgr_sky_sh");
        let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("wgr_sky_sh"),
            timestamp_writes: None,
        });
        pass.set_pipeline(&self.sh_pipeline);
        pass.set_bind_group(0, &self.sh_bind, &[]);
        pass.dispatch_workgroups(1, 1, 1);
        drop(pass);
        encoder.pop_debug_group();
    }

    // The SH-9 sky-irradiance buffer, lent to the camera bind group (frame group binding 9) so the
    // lit-mesh + terrain shaders evaluate directional sky ambient (frame::sky_irradiance).
    pub fn sh_buffer(&self) -> &wgpu::Buffer {
        &self.sh_buf
    }

    // Fill the aerial-perspective froxel volume for this frame (see cs_froxel). Reuses
    // this frame's sky uniform + LUTs, so `upload` + `render_luts` must have run first.
    // One thread per screen column marches the atmosphere front-to-back into the slices.
    pub fn render_froxel(
        &self,
        device: &wgpu::Device,
        encoder: &mut wgpu::CommandEncoder,
        shadow_mask_view: &wgpu::TextureView,
        csm_view: &wgpu::TextureView,
    ) {
        // Group(1) is rebuilt each frame because the terrain mask + CSM textures are owned
        // elsewhere and regenerated; cheap. upload() has already refreshed mapping_buf/csm_ubo.
        let shadow_bind = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("wgr_sky_froxel_shadow_bind"),
            layout: &self.froxel_shadow_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(shadow_mask_view),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::Sampler(&self.mask_sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: self.mapping_buf.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: wgpu::BindingResource::TextureView(csm_view),
                },
                wgpu::BindGroupEntry {
                    binding: 4,
                    resource: wgpu::BindingResource::Sampler(&self.csm_cmp_sampler),
                },
                wgpu::BindGroupEntry {
                    binding: 5,
                    resource: self.csm_ubo.as_entire_binding(),
                },
            ],
        });
        encoder.push_debug_group("wgr_sky_froxel");
        let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("wgr_sky_froxel"),
            timestamp_writes: None,
        });
        pass.set_pipeline(&self.froxel_pipeline);
        pass.set_bind_group(0, &self.froxel_bind, &[]);
        pass.set_bind_group(1, &shadow_bind, &[]);
        pass.dispatch_workgroups(FROXEL_W.div_ceil(8), FROXEL_H.div_ceil(8), 1);
        drop(pass);
        encoder.pop_debug_group();

        // CLD-020: the cloud sun-transmittance map, in the same encoder and reusing group(0).
        // Runs after the froxel fill only for tidiness -- the two are independent, and both read
        // the sky uniform this frame already wrote.
        //
        // The dispatch is unconditional even at strength 0, and that is deliberate: the shader
        // early-outs to "fully lit" per texel, so the map is always VALID. Skipping the dispatch
        // would leave whatever the last enabled frame wrote, and toggling the feature off would
        // freeze the old shadows on the ground instead of clearing them.
        encoder.push_debug_group("wgr_cloud_shadow");
        let mut cloud_pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("wgr_cloud_shadow"),
            timestamp_writes: None,
        });
        cloud_pass.set_pipeline(&self.cloud_shadow_pipeline);
        cloud_pass.set_bind_group(0, &self.froxel_bind, &[]);
        cloud_pass.dispatch_workgroups(
            CLOUD_SHADOW_DIM.div_ceil(8),
            CLOUD_SHADOW_DIM.div_ceil(8),
            1,
        );
        drop(cloud_pass);
        encoder.pop_debug_group();
    }

    // The froxel volume view, lent to the camera bind group so the forward shaders
    // sample it (frame::froxel_fog).
    pub fn froxel_view(&self) -> &wgpu::TextureView {
        &self.froxel_view
    }
}

#[cfg(test)]
mod cloud_evolution_tests {
    // Cloud EVOLUTION must actually reach the noise lookup, on an axis the horizontal sampling
    // does not already vary. Wind only translates the field: the same clouds slide past forever,
    // which is what the sky did before. Drifting the volume's third axis walks through
    // uncorrelated slices, so banks build and clear in place.
    //
    // Pinned in the shader source because the alternative — a uniform lane that is uploaded,
    // plumbed through three structs, exposed in ImGui, and then never read — looks identical from
    // every side except the screen.
    #[test]
    fn evolution_offsets_drive_the_noise_lookup() {
        let src = include_str!("sky.wgsl");
        assert!(
            src.contains("cloud4: vec4<f32>"),
            "the sky uniform must carry the evolution offsets"
        );
        // Shape and detail drift on Y, which the world-position lookup otherwise holds fixed.
        assert!(
            src.contains("ws.y += sky.cloud4.x * sky.cloud1.z"),
            "cloud SHAPE must be sampled at a drifting slice"
        );
        assert!(
            src.contains("wd.y += sky.cloud4.y * sky.cloud1.w"),
            "cloud DETAIL must be sampled at a drifting slice"
        );
        // And where it is cloudy at all has to move, or coverage patches stay pinned to the world
        // forever and only the wisps inside them change.
        assert!(
            src.contains("0.5 + sky.cloud4.z * sky.cloud3.x"),
            "the coverage/weather field must drift too"
        );
    }
}

#[cfg(test)]
mod cirrus_tests {
    // The second cloud layer read as tiled bands of near-identical wisps marching in rows, and the
    // cause was arithmetic: ONE tap for the sheet and ONE for the fibres, both axis-aligned, both
    // at fixed slices of a volume that tiles with period 1 on every axis. That put the sheet's
    // repeat at 28 km in world X and 7 km in world Z; the fibres repeated at 2 km in Z, and
    // 7000/2000 = 7/2 is RATIONAL, so the two agreed on a common period of 14 km. From 6.5 km
    // below, 14 km of sheet is a few degrees of sky — four or five identical copies per frame.
    //
    // Pinned in the source because every one of these measures is invisible from outside: remove
    // any of them and the shader still compiles, still validates, still renders cirrus, and the
    // rows come back. Only a screenshot would tell you, and only if someone looked.
    #[test]
    fn the_cirrus_field_cannot_repeat_on_a_short_period() {
        let src = include_str!("sky.wgsl");
        let body = src
            .split_once("fn cirrus_field(")
            .expect("sky.wgsl declares cirrus_field")
            .1
            .split_once("\nfn ")
            .expect("cirrus_field is followed by another function")
            .0;
        // 1. Two sheet octaves whose scales are in the GOLDEN RATIO. An irrational ratio has no
        //    common period at all, so their sum never recurs at any distance. This is the measure
        //    that makes the repeat impossible rather than merely distant.
        assert!(
            body.contains("1.61803399"),
            "the sheet must be two octaves at an irrational scale ratio"
        );
        // 2. Every lookup rotated, and by DIFFERENT angles, so the octaves' lattices are not even
        //    parallel to each other -- and none of them parallel to the streaks.
        assert_eq!(
            body.matches("cirrus_rot(").count(),
            3,
            "each of the three field lookups must rotate its own domain"
        );
        // 3. Slice drift: the volume's third axis advances with world position, so two points a
        //    whole tile apart in X/Z read DIFFERENT slices.
        assert!(
            body.contains("let slice = (p.x * 0.61803399 + p.y * 0.41421356)"),
            "the field must walk the noise volume's third axis with world position"
        );
        // 4. The fibre lookup must not share a period with the sheet. 3.5 = 7/2 did; 3.301 does
        //    not. A rational ratio here is what let the two fields agree on a 14 km repeat.
        assert!(
            body.contains("p.y * 3.301"),
            "the fibre lookup's z ratio must be irrational against the sheet's"
        );
        assert!(
            !body.contains("* 3.5"),
            "3.5 = 7/2 shares a period with the sheet -- that was the original bug"
        );
        // And the domain warp, which is what stops the lattice being a straight line anywhere.
        assert!(
            src.contains("fn cirrus_warp("),
            "the cirrus field must be domain-warped"
        );
    }

    // The Sky tab's two toggles ride output.w rather than a new WgrSkyLook lane (that struct's
    // size is part of the ABI handshake). If the shader stops reading the lane the checkboxes go
    // dead silently -- the layer just renders whatever it renders.
    #[test]
    fn the_layer_mode_is_read_from_the_uniform() {
        let src = include_str!("sky.wgsl");
        assert!(
            src.contains("let mode = sky.output.w"),
            "cirrus_layer must read its mode (0 off / 1 flat / 2 volumetric) from output.w"
        );
        assert!(
            src.contains("if (mode < 0.5 || sky.cloud0.x <= 0.001 || dir.y <= 0.01)"),
            "mode 0 must return before any work -- the toggle has to cost nothing when off"
        );
    }
}

#[cfg(test)]
mod cirrus_puffiness_tests {
    use super::*;

    // The whole promise of the puffiness slider is that its DEFAULT changes nothing. That is why
    // the mapping is three-point with the shipped value in the middle rather than a lerp between
    // two ends: at 0.5 the shader uses the old constant ITSELF. Pinned because nobody can see a
    // few percent of drift in one of five parameters, so a later "tidy" of the mapping would
    // silently re-tune the shipped sky for everyone.
    #[test]
    fn the_default_puffiness_reproduces_the_shipped_constants() {
        let src = include_str!("sky.wgsl");
        let value_of = |name: &str| -> f32 {
            let decl = format!("const {}: f32 = ", name);
            let tail = src
                .split_once(decl.as_str())
                .unwrap_or_else(|| panic!("sky.wgsl declares {}", name))
                .1;
            let end = tail.find(';').expect("constant is terminated");
            tail[..end].trim().parse().expect("constant is a number")
        };
        // The values the layer rendered with before puffiness existed.
        let shipped = [
            ("ASPECT", 0.25f32), // sheet octaves stretched 4x along world X
            ("FINE", 0.38),      // weight of the x1.618 octave
            ("ERODE", 0.45),     // fibre erosion strength
            ("DENSITY", 2.8),    // optical depth straight up
            ("THICK", 900.0),    // volumetric shell thickness (m)
        ];
        for (name, was) in shipped {
            assert_eq!(
                value_of(&format!("CIRRUS_{}_SHIPPED", name)),
                was,
                "CIRRUS_{}_SHIPPED must stay the value the layer shipped with",
                name
            );
            // And the two ends must actually bracket it, or the slider would fold back on itself
            // and both halves would move the same way.
            let lo = value_of(&format!("CIRRUS_{}_FIBRE", name));
            let hi = value_of(&format!("CIRRUS_{}_PUFF", name));
            assert!(
                (lo - was).signum() != (hi - was).signum(),
                "CIRRUS_{}: the fibrous and puffy ends must sit on opposite sides of shipped",
                name
            );
        }
        // The mapping's middle knot IS the shipped value, but the DEFAULT is no longer the
        // midpoint: 7236fa2f set full puffiness on owner request ("match the deck, full
        // puffiness, full wander"). Pin the deliberate default and keep the knot contract.
        assert_eq!(CIRRUS_PUFF_DEFAULT, 1.0);
        assert!(
            src.contains(
                "return select(mix(fibre, shipped, t), mix(shipped, puffy, t - 1.0), puff > 0.5);"
            ),
            "the puffiness map must pass through the shipped value at 0.5"
        );
        for name in ["ASPECT", "FINE", "ERODE", "DENSITY", "THICK"] {
            assert!(
                src.contains(&format!("CIRRUS_{}_SHIPPED", name)),
                "CIRRUS_{}_SHIPPED must be mapped through, not just declared",
                name
            );
        }
    }

    // The five parameters must all be READ, or the slider moves a number nothing looks at. Each
    // one is an independent silent failure: drop the thickness and the layer stops gaining depth,
    // drop the erosion and it stops losing its fibres, and either way it still renders cirrus.
    #[test]
    fn every_puffiness_parameter_reaches_the_shader() {
        let src = include_str!("sky.wgsl");
        assert!(
            src.contains("let puff = clamp(sky.cloud4.w, 0.0, 1.0)"),
            "the shader must read puffiness from the cloud4.w spare lane"
        );
        for used in [
            "p.x * sh.aspect",   // streak aspect
            "let wb = sh.fine;", // octave balance
            "fibre * sh.erode",  // fibre erosion
            "-d * sh.density",   // flat-mode optical depth
            "sh.alt - sh.thick", // shell geometry (sh.alt is CIRRUS_ALT until MATCH moves it)
        ] {
            assert!(src.contains(used), "sky.wgsl must use `{}`", used);
        }
        // Optical depth per metre divides by the LIVE thickness, so a deeper shell is not also a
        // brighter one -- without this, "puffier" would read as "whiter" and the slider would be
        // an exposure control wearing a shape control's label.
        assert!(
            src.contains("sh.density / (sh.thick * CIRRUS_PROFILE_INTEGRAL)"),
            "extinction must be normalised by the live shell thickness"
        );
        // The shape is resolved once per pixel, not once per march step.
        assert!(
            src.contains("let sh = cirrus_shape();"),
            "cirrus_layer must hoist the puffiness mixes out of the march"
        );
    }

    // AMOUNT and MATCH are two more sliders that can silently move a number nothing reads: the
    // layer still renders cirrus either way, so nothing fails loudly. Assert the same three things
    // the puffiness tests assert -- that the lane is read, that every derived parameter is used,
    // and that the neutral setting is bit-for-bit the shipped look.
    #[test]
    fn the_look_sliders_reach_the_shader() {
        let src = include_str!("sky.wgsl");
        assert!(
            src.contains("let amount = clamp(sky.cirrus.x, 0.0, 1.0)")
                && src.contains("let m = clamp(sky.cirrus.y, 0.0, 1.0)"),
            "the shader must read AMOUNT and MATCH from the cirrus lane"
        );
        // EDGE SOFTNESS shares the lane. It must be READ, applied BEFORE the fibre erosion (so
        // erosion carves the fringe rather than the fringe blurring the erosion), and weighted by
        // the local coverage -- that weighting is the whole of "the BIGGER ones get fuzzier", and
        // without it the slider softens isolated wisps into nothing while the banks barely move.
        assert!(
            src.contains("sh.soft = clamp(sky.cirrus.z, 0.0, 1.0);")
                && src.contains(
                    "let soft = sh.soft * smoothstep(CIRRUS_SOFT_COV_LO, CIRRUS_SOFT_COV_HI, cover);"
                )
                && src.contains("d0 = clamp(pow(d0, k) * ((k + 1.0) * 0.5), 0.0, 1.0);"),
            "the second layer's edge softness must be read, coverage-weighted and mean-restored"
        );
        // The mean restoration is what keeps it a SHAPE control: pow() on a roughly uniform [0,1]
        // drops the mean from 1/2 to 1/(k+1), so (k+1)/2 back is what stops the slider reading as
        // a brightness control. Same principle PUFFINESS states for its own density coupling.
        assert_eq!(CIRRUS_SOFT_DEFAULT, 0.45);
        for used in [
            // AMOUNT: the coverage threshold the field remaps against, and its density coupling.
            "sh.cover = cirrus_mix3(CIRRUS_COVER_SPARSE, CIRRUS_COVER, CIRRUS_COVER_FULL, amount)",
            "CIRRUS_AMOUNT_DENSITY_SPARSE",
            "let cover = sh.cover * smoothstep(0.02, 0.35, sky.cloud0.x);",
            // MATCH: altitude, feature scale, isotropy, striation, depth, opacity, step count.
            "sh.alt = mix(CIRRUS_ALT, matched_alt, m);",
            "sh.scale = mix(CIRRUS_SCALE,",
            "sh.aspect = mix(sh.aspect, CIRRUS_ASPECT_MATCHED, m);",
            "sh.erode = mix(sh.erode, CIRRUS_ERODE_MATCHED, m);",
            "sh.density = mix(sh.density, CIRRUS_DENSITY_MATCHED, m);",
            "sh.thick = mix(sh.thick, matched_thick, m);",
            "sh.step_gain = mix(1.0, CIRRUS_STEPS_MATCH_GAIN, m);",
            // And the two places MATCH shows up outside the shape block.
            "let phase = mix(ice_phase, deck_phase, sh.match_deck);",
            "exp(-sh.density * (1.0 - h) * 0.35), sh.match_deck)",
            // ...and it must attenuate the SUN term only. Applying it to `lit` (sun + ambient)
            // pulls the shaded sides toward the direct sun's hue, which at a low sun is olive.
            "col += trans * (lit_sun * shade + ambient) * (1.0 - seg);",
            // The field and the warp must both take the LIVE scale, or MATCH shrinks the clouds
            // in one of them and not the other and the layer smears.
            "let s = sh.scale;",
            "cirrus_warp(world_xz, sh.scale)",
        ] {
            assert!(src.contains(used), "sky.wgsl must use `{}`", used);
        }
        // The march bound must be the live step count, not the constant it starts from.
        // The step COUNT must come from the ray's own path, not from a constant. A fixed count
        // makes dt scale with the angle-dependent path length, the step phase then varies smoothly
        // across the screen, and the aliasing organises into bands parallel to the horizon -- the
        // reported "parallel strips". The per-pixel jittered start is the second half of the cure:
        // it scatters whatever undersampling survives into noise instead of letting it line up.
        assert!(
            src.contains(
                "let n = clamp(ceil(path / target_step), CIRRUS_STEPS_MIN, f32(CIRRUS_STEPS_MAX));"
            ) && src.contains("let dt = path / n;")
                && src.contains("for (var i: i32 = 0; i < steps; i = i + 1)"),
            "the shell march must derive its step count from the path length"
        );
        assert!(
            src.contains("t = t0 + dt * mix(0.5, jitter, step(0.0001, jitter));")
                && src.contains("hash21(in.clip.xy), scene_dist)"),
            "the shell march must start at a per-pixel jittered offset on the full-res path"
        );
        // ...and NOT on the env bake, where a jittered march reads as a checkerboard once the
        // 256x128 texels are magnified by a water reflection. Same reasoning the deck records.
        assert!(
            // `bg`, not `color`: SKY-002 split the env bake's two lanes, so the ambient the shell
            // is lit by is the star-free sky while `color` (what the water reflects) keeps its
            // stars. The 0.0 jitter argument is what this assertion is actually about.
            src.contains("cirrus_layer(pos, dir, sun, radiance, bg, 0.0, 1e12)"),
            "the env bake must march the cirrus shell without jitter"
        );
        // AMOUNT's midpoint knot IS the old CIRRUS_COVER constant and stays the default;
        // MATCH defaults to the TOP of its range since 7236fa2f (owner request: the second
        // layer matches the deck).
        assert_eq!(CIRRUS_AMOUNT_DEFAULT, 0.5);
        assert_eq!(CIRRUS_MATCH_DEFAULT, 1.0);
    }

    // MATCH must never drop the second layer into the first. The shader clamps the matched
    // altitude to the deck's top plus a gap, with a floor of its own -- a deck authored absurdly
    // high (or inverted) must still leave the layer above it, not inside it.
    #[test]
    fn the_matched_layer_stays_above_the_deck() {
        let src = include_str!("sky.wgsl");
        assert!(
            src.contains(
                "let matched_alt = clamp(sky.cloud0.w + CIRRUS_ALT_MATCH_GAP, CIRRUS_ALT_MATCH_MIN, CIRRUS_ALT);"
            ),
            "the matched altitude must be clamped to [floor, CIRRUS_ALT] above the deck's top"
        );
        // Reproduce the clamp here so the constants themselves are checked, not just the source
        // line: a positive gap and a floor below the cirrus altitude are what make it total.
        let alt = 7000.0f32; // CIRRUS_ALT
        let gap = 1200.0f32; // CIRRUS_ALT_MATCH_GAP
        let floor = 2500.0f32; // CIRRUS_ALT_MATCH_MIN
        assert!(gap > 0.0 && floor < alt);
        for deck_top in [0.0f32, 800.0, 2000.0, 6000.0, 20000.0] {
            let matched = (deck_top + gap).clamp(floor, alt);
            assert!(
                matched > deck_top || deck_top + gap > alt,
                "deck top {} would put the matched layer at {}",
                deck_top,
                matched
            );
        }
    }

    // The variation rides a clock that JUMPS: the C++ side wraps the weather drift at 100 km, so
    // the signal has to have the wrap as an exact period or the sky's shape steps every few hours
    // of play. Integer frequencies in units of the wrap are what guarantee that, and the guarantee
    // survives nothing -- change 7.0 to 7.5 and it still compiles, still varies, and pops.
    #[test]
    fn the_variation_is_continuous_across_the_drift_wrap() {
        let before = cirrus_variation(CIRRUS_VAR_WRAP - 1e-3, 0.0);
        let after = cirrus_variation(0.0, 0.0);
        assert!(
            (before - after).abs() < 1e-4,
            "the variation must not step where the drift clock wraps ({} vs {})",
            before,
            after
        );
        // Bounded, so `base + amount * wave` cannot leave the slider's range by more than the
        // amount itself, and 0 amount is exactly the base.
        let mut max = 0.0f32;
        for i in 0..20000 {
            let v = cirrus_variation(i as f32 * (CIRRUS_VAR_WRAP / 20000.0), 0.0);
            max = max.max(v.abs());
        }
        assert!(
            max <= 1.0,
            "the variation must stay inside [-1,1], got {}",
            max
        );
        // ...and it must actually get somewhere near the ends, or the amount slider would be
        // calibrated against a signal that never delivers what it promises.
        assert!(max > 0.85, "the variation barely moves: peak {}", max);
        // It must also not be a metronome: two frequencies means the peaks are not evenly spaced.
        // Sampling a single period of the SLOWER component must show more than one distinct level.
        let a = cirrus_variation(CIRRUS_VAR_WRAP / 28.0, 0.0);
        let b = cirrus_variation(CIRRUS_VAR_WRAP / 28.0 + CIRRUS_VAR_WRAP / 7.0, 0.0);
        assert!(
            (a - b).abs() > 0.05,
            "the two components share a period -- the layer would breathe like a metronome"
        );
    }

    // 0 variation must mean EXACTLY the authored value, at every point on the clock. This is the
    // "perfectly steady" contract, and it is what makes the slider safe to leave at 0.
    #[test]
    fn zero_variation_is_perfectly_steady() {
        for &drift in &[0.0f32, 1.0, 137.0, 5_000.0, 99_999.0, 1.0e6] {
            for &base in &[0.0f32, 0.25, CIRRUS_PUFF_DEFAULT, 1.0] {
                assert_eq!(cirrus_puff_target(base, 0.0, 0.0, drift), base);
            }
        }
        // The default is full puffiness with full wander (7236fa2f, owner request), so the
        // excursion legitimately spans the mapping's range. What must still hold: the target
        // stays inside [0,1] and actually REACHES near both the wandered floor and the base
        // (a variation that collapsed to a constant would make the slider a no-op).
        let mut lo = 1.0f32;
        let mut hi = 0.0f32;
        for i in 0..20000 {
            let d = i as f32 * (CIRRUS_VAR_WRAP / 20000.0);
            let t = cirrus_puff_target(CIRRUS_PUFF_DEFAULT, CIRRUS_PUFF_VAR_DEFAULT, 0.0, d);
            assert!((0.0..=1.0).contains(&t), "puff target left [0,1]: {}", t);
            lo = lo.min(t);
            hi = hi.max(t);
        }
        assert!(
            hi > 0.9 && lo < hi - 0.3,
            "full variation must wander over a real range up to the base, got {}..{}",
            lo,
            hi
        );
    }

    // The ease is the anti-pop guarantee: whatever the sliders do, the value the shader sees moves
    // by a fraction of the gap per frame. It must be a real fraction (not 1, which is no ease at
    // all, and not so small the control feels dead) and it must converge.
    #[test]
    fn the_applied_puffiness_cannot_step() {
        assert!(CIRRUS_PUFF_EASE > 0.0 && CIRRUS_PUFF_EASE < 0.5);
        // Worst case: the user drags puffiness from one end to the other in one frame.
        let mut now = 0.0f32;
        let target = 1.0f32;
        now += (target - now) * CIRRUS_PUFF_EASE;
        assert!(
            now < 0.15,
            "a full-range slider jump moved the sky by {} in one frame",
            now
        );
        for _ in 0..600 {
            now += (target - now) * CIRRUS_PUFF_EASE;
        }
        assert!(
            (now - target).abs() < 1e-4,
            "the ease must converge well inside a screenshot delay, got {}",
            now
        );
    }
}

#[cfg(test)]
mod sky_uniform_layout_tests {
    // SkyUniform and sky.wgsl's `struct Sky` are the same buffer seen from two languages, and
    // nothing checks that they agree. Inserting a field in one and not the other is not a compile
    // error in either — it is a silent layout shift, and it surfaces as
    //
    //   "the buffer bound at binding index 0 is bound with size 336 where the shader expects 352"
    //
    // on EVERY 3D draw at once, which points at the bind group rather than at the field that
    // moved. That is exactly what adding cloud4 for cloud evolution did.
    //
    // Compare the field ORDER, not just the size: two vec4 fields swapped keeps the size identical
    // and silently reinterprets both.
    #[test]
    fn rust_and_wgsl_sky_structs_declare_the_same_fields_in_the_same_order() {
        let src = include_str!("sky.wgsl");
        let body = src
            .split_once("struct Sky {")
            .expect("sky.wgsl declares struct Sky")
            .1
            .split_once("};")
            .expect("struct Sky is terminated")
            .0;
        let wgsl: Vec<&str> = body
            .lines()
            .filter_map(|l| {
                let t = l.trim();
                if t.starts_with("//") {
                    return None;
                }
                t.split_once(':').map(|(name, _)| name.trim())
            })
            .filter(|n| !n.is_empty() && !n.contains(' '))
            .collect();

        // The Rust side, in declaration order. Kept as a literal list rather than derived,
        // because deriving it from the struct would mean the test could only ever agree with
        // itself — this list is the assertion.
        let rust = [
            "inv_view_proj",
            "sun_dir",
            "moon_dir",
            "rayleigh",
            "mie",
            "ground_albedo",
            "params",
            "control",
            "fog_color",
            "night_zenith",
            "night_horizon",
            "night_params",
            "cloud0",
            "cloud1",
            "cloud2",
            "cloud3",
            "cloud4",
            "output",
            "cam_pos",
            // CLD-020's cloud sun-transmittance mapping. It was added to BOTH structs but not to
            // this list, so the check that is supposed to catch a layout skew was itself skewed
            // and had been failing since.
            "cloud_shadow",
            // The moon disc's two lanes. They ride the SAME UBO as everything above, so the
            // C++ static_assert(sizeof(WgrSkyRuntime) == 112) and this list are the pair that
            // has to move together — one without the other is silent garbage in the shader.
            "moon_params",
            "moon_sun",
            // The second layer's AMOUNT / MATCH sliders. Renderer-written, like cloud4.w — but
            // unlike it they needed a lane of their own, so this list moves with them.
            "cirrus",
            // SKY-002's star cloud-occlusion strength. Renderer-written like `cirrus`, and on this
            // list for the same reason: the two structs are only kept in step by this assertion.
            "night_sky",
        ];
        assert_eq!(
            wgsl, rust,
            "sky.wgsl's Sky and Rust's SkyUniform must declare identical fields in identical order"
        );
        // 19 vec4 lanes + one mat4. If this moves, both lists above must have moved with it.
        assert_eq!(
            std::mem::size_of::<super::SkyUniform>(),
            64 + (rust.len() - 1) * 16
        );
    }
}

#[cfg(test)]
mod cloud_shadow_range_tests {
    use super::{
        CLOUD_SHADOW_DIM, CLOUD_SHADOW_SPAN_MAX, CLOUD_SHADOW_SPAN_MIN, cloud_shadow_span_for,
    };

    #[test]
    fn disabling_cloud_shadows_does_not_disable_smoke_on_surfaces() {
        let src = include_str!("../shaders/frame.wgsl");
        let body = src.split("fn cloud_sun_shadow(").nth(1).unwrap()
            .split("fn terrain_sun_shadow(").next().unwrap();
        assert!(!body.contains("cloud_shadow.w <="));
        assert!(body.contains("t.r * t.g"));
        let producer = include_str!("sky.wgsl");
        assert!(producer.contains("vec4<f32>(1.0, smoke0, 0.0, 1.0)"));
    }

    // The reported fault: the map spanned a fixed 4096 m centred on the camera, so it reached
    // 2048 m, and `cloud_sun_shadow` returns fully lit outside it. With the view distance wound
    // out for flying, the ground past 2048 m was in full sun and the boundary drew a straight
    // line across the landscape that moved with the camera.
    //
    // The requirement, stated as a property rather than as a table: whatever the draw distance,
    // the map's HALF extent must reach it, up to the cap.
    #[test]
    fn the_map_reaches_the_far_plane_at_every_draw_distance() {
        let mut span = CLOUD_SHADOW_SPAN_MIN;
        for step in 0..200 {
            let draw = 50.0 * (step as f32 + 1.0); // 50 m .. 10 km
            span = cloud_shadow_span_for(span, draw);
            let reach = span * 0.5;
            assert!(
                reach >= draw.min(CLOUD_SHADOW_SPAN_MAX * 0.5),
                "a {draw} m draw distance needs {draw} m of reach, got {reach} m (span {span})"
            );
        }
    }

    // On foot nothing may change: 4096 m / 512 = 8 m per texel is the quality this feature
    // shipped with, and it is the level that must survive a normal view distance.
    #[test]
    fn short_draw_distances_keep_the_original_span_and_resolution() {
        for draw in [200.0f32, 900.0, 1200.0, 2048.0] {
            assert_eq!(
                cloud_shadow_span_for(CLOUD_SHADOW_SPAN_MIN, draw),
                CLOUD_SHADOW_SPAN_MIN,
                "{draw} m fits inside the shipped span; it must not be widened"
            );
        }
        assert_eq!(CLOUD_SHADOW_SPAN_MIN / CLOUD_SHADOW_DIM as f32, 8.0);
        assert_eq!(CLOUD_SHADOW_SPAN_MAX / CLOUD_SHADOW_DIM as f32, 32.0);
    }

    // Every level's texel must be a power-of-two multiple of the finest one, or the origin snap
    // in `cloud_shadow_mapping` lands on a grid that is NOT a subset of the previous level's and
    // the whole shadow field slides when the level changes. Powers of two are what make a level
    // change a one-off re-rasterisation instead of a crawl.
    #[test]
    fn every_span_level_is_a_power_of_two_multiple_of_the_minimum() {
        let mut span = CLOUD_SHADOW_SPAN_MIN;
        for draw in [100.0f32, 3000.0, 5000.0, 40000.0, 5000.0, 100.0] {
            span = cloud_shadow_span_for(span, draw);
            let ratio = span / CLOUD_SHADOW_SPAN_MIN;
            assert!(
                ratio >= 1.0 && (ratio as u32).is_power_of_two() && ratio.fract() == 0.0,
                "span {span} is not a power-of-two multiple of {CLOUD_SHADOW_SPAN_MIN}"
            );
            assert!(
                span <= CLOUD_SHADOW_SPAN_MAX,
                "span {span} exceeded the cap"
            );
        }
    }

    // The snap in `cloud_shadow_mapping` is what stops the shadows crawling over the ground as
    // the camera moves, and a variable span is the obvious way to break it: re-snapping to a
    // different grid translates the whole field. Powers of two are the guard — a coarser level's
    // grid lines are a SUBSET of every finer level's — so a level change re-rasterises at a new
    // resolution without sliding, and within a level nothing moves at all.
    //
    // Asserted on the snap arithmetic itself rather than on the constants, because the constants
    // could stay powers of two while the origin formula stopped being a floor to the texel.
    #[test]
    fn a_coarse_origin_is_still_on_every_finer_levels_grid() {
        let origin_at = |span: f32, cam: f32| {
            let texel = span / CLOUD_SHADOW_DIM as f32;
            ((cam - span * 0.5) / texel).floor() * texel
        };
        let mut span = CLOUD_SHADOW_SPAN_MIN;
        while span <= CLOUD_SHADOW_SPAN_MAX {
            let fine = CLOUD_SHADOW_SPAN_MIN / CLOUD_SHADOW_DIM as f32;
            for cam in [0.0f32, 1.0, 4321.5, -777.25, 12800.0] {
                let o = origin_at(span, cam);
                assert_eq!(
                    (o / fine).fract(),
                    0.0,
                    "span {span}'s origin {o} is off the {fine} m base grid; a level change \
                     would translate the whole shadow field"
                );
            }
            span *= 2.0;
        }
    }

    // The draw distance is the scene FOG MAX, which the engine widens and narrows with the
    // weather, so it drifts. Without hysteresis a range sitting on a level boundary would
    // re-rasterise the field every few frames -- the same visible crawl the snap exists to
    // prevent, just at a slower rate.
    #[test]
    fn a_draw_distance_drifting_across_a_boundary_does_not_flap_the_level() {
        // 2048 m is the 4096 <-> 8192 boundary. Grow once, then wobble either side of it.
        let grown = cloud_shadow_span_for(CLOUD_SHADOW_SPAN_MIN, 2100.0);
        assert_eq!(grown, 8192.0);
        for draw in [2100.0f32, 2000.0, 2049.0, 1900.0, 2200.0, 1850.0] {
            assert_eq!(
                cloud_shadow_span_for(grown, draw),
                grown,
                "{draw} m is inside the hysteresis band and must not change the level"
            );
        }
        // A genuine drop -- someone turned the view distance down -- still takes effect.
        assert_eq!(cloud_shadow_span_for(grown, 900.0), CLOUD_SHADOW_SPAN_MIN);
    }

    // A camera with no usable fog range (fog off) must not yank the map back to the minimum for
    // a frame; that would re-rasterise the field for no reason at all.
    #[test]
    fn an_unknown_draw_distance_holds_the_current_level() {
        for bad in [0.0f32, -1.0, f32::NAN, f32::INFINITY] {
            assert_eq!(cloud_shadow_span_for(8192.0, bad), 8192.0);
        }
    }

    // The edge fade is the graceful-degradation half of the fix, and its DIRECTION is the whole
    // point: mixing toward 1.0 can only ever lighten, so it cannot produce the dark band at the
    // map's edge that the hard `return 1.0` above it exists to prevent. Pinned in the source
    // because reversing the mix arguments still compiles, still validates, still renders, and
    // replaces one rendering fault with a worse one.
    #[test]
    fn the_edge_fade_can_only_lighten() {
        for (name, src, konst) in [
            (
                "frame.wgsl",
                include_str!("../shaders/frame.wgsl"),
                "CLOUD_SHADOW_EDGE_FADE",
            ),
            (
                "godrays.wgsl",
                include_str!("../godrays.wgsl"),
                "CLOUD_MAP_EDGE_FADE",
            ),
        ] {
            assert!(
                src.contains(&format!("const {konst}: f32 = 0.85;")),
                "{name} must declare the map-edge fade band"
            );
            assert!(
                src.contains(&format!("smoothstep({konst}, 1.0, edge)")),
                "{name} must ramp over the OUTER band (edge 1 = the map boundary)"
            );
            // mix(1.0, x, keep): keep 0 at the boundary -> fully lit. The reversed form
            // mix(x, 1.0, keep) would darken toward the edge instead.
            //
            // What sits in the middle argument is deliberately not pinned, only its DIRECTION.
            // godrays.wgsl fades `vis` -- cloud and smoke already combined -- rather than the
            // cloud term alone: both are channels of the same texel of the same map, so both
            // leave it at the same boundary and both must ease out there.
            assert!(
                src.contains("mix(1.0, shadow, keep)") || src.contains("mix(1.0, vis, keep)"),
                "{name} must fade TOWARD fully lit, never away from it"
            );
        }
    }
}

#[cfg(test)]
#[path = "reflection_probe_tests.rs"]
mod reflection_probe_tests;
