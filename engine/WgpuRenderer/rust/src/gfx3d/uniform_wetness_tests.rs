use wgpu::util::DeviceExt;

fn probe_source() -> String {
    let source = include_str!("shader3d.wgsl");
    let helper = source.split("// BEGIN_UNIFORM_CLOTH_HELPERS").nth(1).unwrap()
        .split_once('\n').unwrap().1.split("// END_UNIFORM_CLOTH_HELPERS").next().unwrap();
    let material = source.split("struct Material {").nth(1).unwrap().split("};").next().unwrap();
    let color = include_str!("../shaders/color.wgsl").lines()
        .filter(|line| !line.starts_with('#')).collect::<Vec<_>>().join("\n");
    format!(r#"{color}
{helper}
struct Material {{{material}}};
@group(0) @binding(0) var<storage,read> inputs: array<vec4<f32>>;
@group(0) @binding(1) var<storage,read_write> outputs: array<vec4<f32>>;
@compute @workgroup_size(1)
fn cs_probe(@builtin(global_invocation_id) id: vec3<u32>) {{
    let v = inputs[id.x];
    let rgb = uniform_cloth_color(vec3<f32>(0.02,0.15,0.6),v.z,v.xy,vec2<f32>(v.w));
    outputs[id.x] = vec4<f32>(rgb, uniform_cloth_mask(v.xy,v.z,vec2<f32>(v.w)));
}}
"#)
}

fn parsed_probe() -> naga::Module {
    let source = probe_source();
    let module = naga::front::wgsl::parse_str(&source)
        .unwrap_or_else(|error| panic!("{}",error.emit_to_string(&source)));
    naga::valid::Validator::new(naga::valid::ValidationFlags::all(),naga::valid::Capabilities::empty())
        .validate(&module).expect("production cloth helper must validate");
    module
}

fn textile_helpers() -> &'static str {
    include_str!("../shaders/shading.wgsl")
        .split("// BEGIN_WET_TEXTILE_HELPERS").nth(1).unwrap()
        .split_once('\n').unwrap().1.split("// END_WET_TEXTILE_HELPERS").next().unwrap()
}

#[test]
fn actual_wet_textile_helpers_validate_and_paths_keep_source_admission() {
    parsed_probe(); // Includes the actual shared absorption/sheen mask decoder.
    let helpers = textile_helpers();
    let module = naga::front::wgsl::parse_str(helpers).unwrap();
    naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::empty())
        .validate(&module).expect("actual wet textile helpers must validate");
    let direct = include_str!("shader3d.wgsl");
    let retained = include_str!("gpu_driven.wgsl");
    assert!(direct.contains("m.wet_cloth = uniform_cloth_wetness(material.local_specular.w, in.uv, uniform_uv_footprint);"));
    assert!(retained.contains("m.wet_cloth = 0.0;"));
    assert!(!retained.contains("uniform_cloth_wetness("));
    let shared = include_str!("../shaders/shading.wgsl");
    assert!(shared.contains("if (mat.wet_cloth > 0.0 && linear > 0.5)"));
    assert!(shared.contains("let cloth_n = surface_normal(geo_normal, world_pos, dwx, dwy);"));
    assert!(shared.contains("* frame.sun_ambient.w * amb_ao * cavity;"));
    assert!(shared.contains("* sun_vis;"));
    assert!(shared.contains("rgb = wet_textile_composite(rgb, coat_sky, coat_sun, coat_local.specular, response);"));
    assert!(!shared.contains("coat_sky = sky_irradiance(reflect("));
}

#[test]
fn actual_wet_textile_response_is_broad_bounded_and_reallocates_energy_on_gpu() {
    let (device, queue) = super::cull::tests::headless().expect("wet textile proof needs a device");
    let source = format!(r#"{}
@group(0) @binding(0) var<storage, read> inputs: array<vec4<f32>>;
@group(0) @binding(1) var<storage, read_write> outputs: array<vec4<f32>>;
@compute @workgroup_size(1)
fn cs_probe(@builtin(global_invocation_id) id: vec3<u32>) {{
    let v = inputs[id.x];
    let r = wet_textile_response(v.x, v.y, v.z);
    outputs[id.x * 2u] = r;
    let lobe = wet_textile_sun_lobe(v.w, 1.0, r);
    let back = wet_textile_sun_lobe(v.w, -1.0, r);
    // Unit white substrate under unit uniform sky: reflected share replaces
    // the matching underlying energy exactly, even at the grazing ceiling.
    let white_sky = wet_textile_composite(vec3<f32>(1.0), vec3<f32>(1.0),
        vec3<f32>(0.0), vec3<f32>(0.0), r).x;
    outputs[id.x * 2u + 1u] = vec4<f32>(lobe, back, white_sky, 1.0 - r.x * r.y);
}}
"#, textile_helpers());
    let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("actual bounded wet textile response"), source: wgpu::ShaderSource::Wgsl(source.into()),
    });
    let input: Vec<[f32;4]> = vec![
        [0.0, 0.5, 0.001, 1.0], [-1.0, 0.5, 0.001, 1.0],
        [0.5, 1.0, 0.001, 1.0], [1.0, 0.0, 0.001, 1.0],
        [1.0, 1.0, 0.08, 1.0], [1.0, 0.5, 0.001, 0.8],
        [1.0, 1.0, 0.001, -1.0], [0.38, 0.5, 0.04, 1.0],
    ];
    let inputs = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label: Some("wet textile angle/footprint cases"), contents: bytemuck::cast_slice(&input),
        usage: wgpu::BufferUsages::STORAGE,
    });
    let bytes = (input.len() * 32) as u64;
    let outputs = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("wet textile actual outputs"), size: bytes, mapped_at_creation: false,
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
    });
    let staging = device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("wet textile readback"), size: bytes, mapped_at_creation: false,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
    });
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some("actual wet textile probe"), layout: None, module: &shader,
        entry_point: Some("cs_probe"), compilation_options: Default::default(), cache: None,
    });
    let bindings = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("wet textile cases/results"), layout: &pipeline.get_bind_group_layout(0),
        entries: &[
            wgpu::BindGroupEntry { binding: 0, resource: inputs.as_entire_binding() },
            wgpu::BindGroupEntry { binding: 1, resource: outputs.as_entire_binding() },
        ],
    });
    let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor::default());
    {
        let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor::default());
        pass.set_pipeline(&pipeline); pass.set_bind_group(0, &bindings, &[]);
        pass.dispatch_workgroups(input.len() as u32, 1, 1);
    }
    encoder.copy_buffer_to_buffer(&outputs, 0, &staging, 0, bytes);
    queue.submit(Some(encoder.finish()));
    let slice = staging.slice(..); let (sender, receiver) = std::sync::mpsc::channel();
    slice.map_async(wgpu::MapMode::Read, move |r| { let _ = sender.send(r); });
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap(); receiver.recv().unwrap().unwrap();
    let data = slice.get_mapped_range(); let actual: &[[f32;4]] = bytemuck::cast_slice(&data);
    for (i, v) in input.iter().enumerate() {
        let response = actual[i * 2]; let light = actual[i * 2 + 1];
        assert!(response.iter().chain(light.iter()).all(|x| x.is_finite()));
        if v[0] <= 0.0 { assert_eq!(response, [0.0;4]); assert_eq!(light[0], 0.0); }
        else {
            assert!((response[0] - 0.125 * v[0] * (2.0 - v[0])).abs() < 0.00001);
            assert!(response[1] >= 0.019999 && response[1] <= 0.120001);
            assert!(response[2] >= 8.0 && response[2] <= 16.0);
            assert!(response[3] > 0.0 && response[3] < 0.015);
        }
        assert_eq!(light[1], 0.0, "backlit cloth must not acquire direct wet sheen");
        assert!((light[2] - 1.0).abs() < 0.000001, "white uniform sky gained energy");
        assert!(light[3] >= 0.984999 && light[3] <= 1.0);
        assert!(light[0] >= 0.0 && light[0] < 0.015);
    }
    assert_eq!(actual[4][2], 16.0); assert_eq!(actual[8][2], 8.0);
    assert!(actual[11][0] > 0.0 && actual[11][0] < actual[7][0]);
    assert_eq!(actual[13][0], 0.0);
    drop(data); staging.unmap();
}

#[test]
fn actual_uniform_helper_preserves_material_tail_layout() {
    let module = parsed_probe();
    let (_, material) = module.types.iter().find(|(_,ty)|ty.name.as_deref()==Some("Material")).unwrap();
    let naga::TypeInner::Struct {members,span} = &material.inner else {panic!("Material must be struct")};
    assert_eq!(*span as usize,std::mem::size_of::<super::MaterialUbo>());
    let tail = members.iter().find(|m|m.name.as_deref()==Some("local_specular")).unwrap();
    assert_eq!(tail.offset as usize,std::mem::offset_of!(super::MaterialUbo,local_specular));
    assert_eq!(tail.offset,112);
    assert_eq!(*span,128); // existing .w at byte124, no public ABI extension
}

#[test]
fn production_wet_cloth_layer_preserves_real_camo_contrast_and_light_units_on_gpu() {
    // Actual stock merged/00007mc_vojakw2.paa texels, decoded by PoseidonTools:
    // member SHA256 70f0ac74e5c9904d3140ed4b5eed9c72c627c5b714e501d282b948ad447b328d.
    // Light values below are explicit controls, not captured installed-frame coefficients.
    let (device, queue) = super::cull::tests::headless().expect("cloth optics needs a real test device");
    let prelude = probe_source().split("@group(0) @binding(0)").next().unwrap().to_owned();
    let source = format!(r#"{prelude}
{}
struct ClothCase {{ albedo: vec4<f32>, uv_wet: vec4<f32>, sky: vec4<f32>, sun_local: vec4<f32>, angles: vec4<f32> }};
@group(0) @binding(0) var<storage, read> cases: array<ClothCase>;
@group(0) @binding(1) var<storage, read_write> results: array<vec4<f32>>;
@compute @workgroup_size(1)
fn cs_layer(@builtin(global_invocation_id) id: vec3<u32>) {{
    let v = cases[id.x];
    let wetness = uniform_cloth_wetness(v.uv_wet.z, v.uv_wet.xy, vec2<f32>(v.angles.w));
    let response = wet_textile_response(wetness, v.angles.x, v.angles.w);
    let wet_albedo = srgb_to_linear(uniform_cloth_color(v.albedo.rgb, v.uv_wet.z,
        v.uv_wet.xy, vec2<f32>(v.angles.w)));
    let light_cosine = clamp(v.angles.z, 0.0, 1.0);
    let illumination = v.sky.rgb + v.sun_local.rgb * light_cosine + vec3<f32>(v.sun_local.w);
    let substrate = wet_albedo * illumination;
    let sun = v.sun_local.rgb * wet_textile_sun_lobe(v.angles.y, v.angles.z, response);
    // Matches the existing local-light Blinn amplitude after attenuation/shadow:
    // its positive-facing branch precedes this lobe; no new local occlusion model.
    let local = vec3<f32>(select(0.0, v.sun_local.w * response.w
        * pow(clamp(v.angles.y, 0.0, 1.0), response.z), v.angles.z > 0.0));
    let wet = wet_textile_composite(substrate, v.sky.rgb, sun, local, response);
    results[id.x * 3u] = vec4<f32>(srgb_to_linear(v.albedo.rgb) * illumination, 0.0);
    results[id.x * 3u + 1u] = vec4<f32>(wet, wetness);
    results[id.x * 3u + 2u] = response;
}}
"#, textile_helpers());
    let module = naga::front::wgsl::parse_str(&source).unwrap_or_else(|e| panic!("{}",e.emit_to_string(&source)));
    naga::valid::Validator::new(naga::valid::ValidationFlags::all(),naga::valid::Capabilities::empty()).validate(&module).unwrap();
    let texels = [([8.0,16.0,33.0],[160.0/1024.0,128.0/1024.0]),
        ([99.0,89.0,82.0],[800.0/1024.0,180.0/1024.0]),
        ([49.0,36.0,33.0],[389.0/1024.0,102.0/1024.0]),
        ([57.0,60.0,49.0],[620.0/1024.0,280.0/1024.0])];
    let mut input: Vec<[[f32;4];5]> = texels.iter().map(|(rgb,uv)|[
        [rgb[0]/255.0,rgb[1]/255.0,rgb[2]/255.0,1.0], [uv[0],uv[1],3.0,0.0],
        [1.0,1.0,1.0,0.0], [0.0;4], [0.7,0.95,0.8,0.001]]).collect();
    input.push(input[2]); input[4][1][2] = 2.38; // ordinary retained dampness
    input.push(input[2]); input[5][1][2] = 2.0; // exact dry history
    input.push(input[2]); input[6][1] = [0.28,0.28,3.0,0.0]; // west hand atlas
    input.push(input[2]); input[7][1] = [0.90,0.65,3.0,0.0]; // boots
    input.push(input[2]); input[8][1] = [0.10,0.80,3.0,0.0]; // baked far face/body
    input.push(input[2]); input[9][2] = [0.0;4]; // pitch dark, no source
    input.push(input[2]); input[10][2] = [0.0002,0.0002,0.0002,0.0]; // dim sky
    input.push(input[2]); input[11][2] = [20.0,20.0,20.0,0.0]; // HDR linear scaling
    input.push(input[2]); input[12][2] = [0.0;4]; input[12][3] = [1.0,0.8,0.6,0.0]; // real sun lobe
    input.push(input[12]); input[13][4][2] = -1.0; // back-facing sun
    input.push(input[2]); input[14][2] = [0.0;4]; input[14][3] = [0.0,0.0,0.0,1.0]; // local source
    input.push(input[2]); input[15][0] = [0.0,0.0,0.0,1.0]; // neutral reflection cannot inherit dye
    input.push(input[2]); input[16][4][0] = 0.0; input[16][4][3] = 0.08; // grazing/distant rough fibre
    let shader=device.create_shader_module(wgpu::ShaderModuleDescriptor{label:Some("production wet cloth optical layer"),source:wgpu::ShaderSource::Naga(std::borrow::Cow::Owned(module))});
    let inputs=device.create_buffer_init(&wgpu::util::BufferInitDescriptor{label:None,contents:bytemuck::cast_slice(&input),usage:wgpu::BufferUsages::STORAGE});
    let bytes=(input.len()*48) as u64;
    let output=device.create_buffer(&wgpu::BufferDescriptor{label:None,size:bytes,mapped_at_creation:false,usage:wgpu::BufferUsages::STORAGE|wgpu::BufferUsages::COPY_SRC});
    let staging=device.create_buffer(&wgpu::BufferDescriptor{label:None,size:bytes,mapped_at_creation:false,usage:wgpu::BufferUsages::MAP_READ|wgpu::BufferUsages::COPY_DST});
    let pipeline=device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor{label:None,layout:None,module:&shader,entry_point:Some("cs_layer"),compilation_options:Default::default(),cache:None});
    let bind=device.create_bind_group(&wgpu::BindGroupDescriptor{label:None,layout:&pipeline.get_bind_group_layout(0),entries:&[
        wgpu::BindGroupEntry{binding:0,resource:inputs.as_entire_binding()},wgpu::BindGroupEntry{binding:1,resource:output.as_entire_binding()}]});
    let mut encoder=device.create_command_encoder(&Default::default());
    {let mut pass=encoder.begin_compute_pass(&Default::default());pass.set_pipeline(&pipeline);pass.set_bind_group(0,&bind,&[]);pass.dispatch_workgroups(input.len() as u32,1,1);}
    encoder.copy_buffer_to_buffer(&output,0,&staging,0,bytes);queue.submit(Some(encoder.finish()));
    let (sender,receiver)=std::sync::mpsc::channel();staging.slice(..).map_async(wgpu::MapMode::Read,move|r|{let _=sender.send(r);});
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();receiver.recv().unwrap().unwrap();
    let data=staging.slice(..).get_mapped_range();let actual:&[[f32;4]]=bytemuck::cast_slice(&data);
    let dry=|i:usize|actual[i*3]; let wet=|i:usize|actual[i*3+1];
    let luminance=|v:[f32;4]|v[0]*0.2126+v[1]*0.7152+v[2]*0.0722;
    assert!(actual.iter().flatten().all(|x|x.is_finite()));
    assert!(luminance(wet(0)) < luminance(dry(0)),"dark actual camo lost absorption beneath neutral reflection");
    for i in 1..5 {assert!(luminance(wet(i)) < luminance(dry(i))*0.85,"wet actual camo must darken: case{i}");}
    let contrast=|a:f32,b:f32|(a-b).abs()/(a+b);
    let dry_contrast=contrast(luminance(dry(0)),luminance(dry(1)));
    let wet_contrast=contrast(luminance(wet(0)),luminance(wet(1)));
    assert!(wet_contrast>dry_contrast*0.85,"neutral film washed out actual camouflage contrast");
    for i in 5..9 {assert_eq!(&wet(i)[..3],&dry(i)[..3],"dry/skin/boots/far atlas changed at{i}");}
    assert_eq!(&wet(9)[..3],&[0.0;3],"wet cloth self-lit without sky/sun/local source");
    for c in 0..3 {
        assert!((wet(10)[c]-wet(2)[c]*0.0002).abs()<0.00000001);
        assert!((wet(11)[c]-wet(2)[c]*20.0).abs()<0.000001,"HDR light units lost linearity");
    }
    assert!(luminance(wet(12))>0.0); assert_eq!(&wet(13)[..3],&[0.0;3]);
    assert!(luminance(wet(14))>0.0); assert_eq!(wet(15)[0],wet(15)[1]); assert_eq!(wet(15)[1],wet(15)[2]);
    assert!(wet(15)[0]>0.0,"neutral dielectric glare must not depend on camouflage albedo");
    assert!(actual[16*3+2][0]<=0.125 && actual[16*3+2][1]<=0.120001);
    eprintln!("WET_CLOTH_OPTICS dryContrast={dry_contrast} wetContrast={wet_contrast} texelWet={:?} texelDry={:?} scope=actual-production-absorption-and-composite-controlled-lighting-not-installed-frame",wet(2),dry(2));
    drop(data);staging.unmap();
}

#[test]
fn real_uniform_shader_preserves_skin_and_cloth_linear_hue_on_gpu() {
    // Probe calls the ACTUAL production WGSL helpers, not a copied Rust mask.
    // Headless GPU absence is a failure, never silently successful acceptance.
    let (device,queue)=super::cull::tests::headless().expect("uniform shader proof needs a test device");
    let cases: Vec<([f32;4],f32)> = vec![
        ([0.38,0.10,0.0,0.001],0.0), // exact dry
        ([0.38,0.10,0.5,0.001],0.5), // separate authored cloth
        ([0.38,0.10,2.08,0.001],0.08), // first actual damp history band
        ([0.38,0.10,2.2,0.001],0.2), // wet enough for installed admission gate
        ([0.38,0.10,2.38,0.001],0.38), // earlier subtle installed WB proof
        ([0.38,0.10,1.0,0.001],1.0),
        ([0.38,0.10,3.0,0.001],1.0), // west main uniform
        ([0.10,0.10,3.0,0.001],1.0),
        ([0.28,0.28,3.0,0.001],0.0), // photographed west hands
        ([0.48,0.28,3.0,0.001],0.0),
        ([0.52,0.28,3.0,0.001],0.0),
        ([0.73,0.28,3.0,0.001],0.0),
        ([0.38,0.28,3.0,0.001],1.0), // pelvis cloth between hands must still wet
        ([0.62,0.28,3.0,0.001],1.0),
        ([0.80,0.42,3.0,0.001],0.0), // helmet
        ([0.90,0.65,3.0,0.001],0.0), // boots
        ([0.10,0.80,3.0,0.001],0.0), // baked far LOD face/body
        ([0.65,0.10,5.0,0.001],1.0), // east main uniform
        ([0.10,0.10,5.0,0.001],1.0),
        ([0.53,0.29,5.0,0.001],0.0), // photographed east hands
        ([0.73,0.29,5.0,0.001],0.0),
        ([0.78,0.29,5.0,0.001],0.0),
        ([0.97,0.29,5.0,0.001],0.0),
        ([0.62,0.29,5.0,0.001],1.0), // pelvis cloth between hands
        ([0.88,0.29,5.0,0.001],1.0),
        ([0.38,0.45,5.0,0.001],0.0), // east boots
        ([0.65,0.48,5.0,0.001],0.0),
        ([0.60,0.65,5.0,0.001],0.0),
        ([0.10,0.80,5.0,0.001],0.0), // baked far LOD face/body
        ([0.28,0.28,3.0,0.05],0.0), // minification never re-admits skin
        ([0.53,0.29,5.0,0.05],0.0),
    ];
    let input: Vec<[f32;4]>=cases.iter().map(|(v,_)|*v).collect();
    let bytes=(input.len()*16) as u64;
    let input_buffer=device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
        label:Some("cloth_uv_cases"),contents:bytemuck::cast_slice(&input),usage:wgpu::BufferUsages::STORAGE,
    });
    let output=device.create_buffer(&wgpu::BufferDescriptor {
        label:Some("cloth_actual_results"),size:bytes,mapped_at_creation:false,
        usage:wgpu::BufferUsages::STORAGE|wgpu::BufferUsages::COPY_SRC,
    });
    let staging=device.create_buffer(&wgpu::BufferDescriptor {
        label:Some("cloth_readback"),size:bytes,mapped_at_creation:false,
        usage:wgpu::BufferUsages::MAP_READ|wgpu::BufferUsages::COPY_DST,
    });
    let shader=device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label:Some("actual_uniform_cloth_helper"),
        source:wgpu::ShaderSource::Naga(std::borrow::Cow::Owned(parsed_probe())),
    });
    let pipeline=device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label:Some("cloth_probe"),layout:None,module:&shader,entry_point:Some("cs_probe"),
        compilation_options:Default::default(),cache:None,
    });
    let bind=device.create_bind_group(&wgpu::BindGroupDescriptor {
        label:Some("cloth_probe_inputs"),layout:&pipeline.get_bind_group_layout(0),
        entries:&[
            wgpu::BindGroupEntry {binding:0,resource:input_buffer.as_entire_binding()},
            wgpu::BindGroupEntry {binding:1,resource:output.as_entire_binding()},
        ],
    });
    let mut encoder=device.create_command_encoder(&Default::default());
    {
        let mut pass=encoder.begin_compute_pass(&Default::default());
        pass.set_pipeline(&pipeline);pass.set_bind_group(0,&bind,&[]);
        pass.dispatch_workgroups(cases.len() as u32,1,1);
    }
    encoder.copy_buffer_to_buffer(&output,0,&staging,0,bytes);
    queue.submit(Some(encoder.finish()));
    let slice=staging.slice(..);let (sender,receiver)=std::sync::mpsc::channel();
    slice.map_async(wgpu::MapMode::Read,move |result| {let _=sender.send(result);});
    device.poll(wgpu::PollType::wait_indefinitely()).unwrap();receiver.recv().unwrap().unwrap();
    let data=slice.get_mapped_range();let actual: &[[f32;4]]=bytemuck::cast_slice(&data);
    let decode=|x:f32|if x<=0.04045 {x/12.92} else {((x+0.055)/1.055).powf(2.4)};
    for (i,((input,wet),row)) in cases.iter().zip(actual).enumerate() {
        for channel in 0..3 {
            let original=[0.02,0.15,0.6][channel];
            if *wet==0.0 {
                assert_eq!(row[channel],original,"excluded/dry changed at case{i}: {input:?}");
            } else {
                let ratio=decode(row[channel])/decode(original);
                let expected = 1.0-0.55*wet*(2.0-wet);
                assert!((ratio-expected).abs()<0.00005,
                    "linear cloth hue/brightness changed at case{i} channel{channel}: ratio{ratio}");
                if (*wet-0.38).abs()<0.00001 {
                    assert!(ratio<0.67,"ordinary retained dampness must have visible absorption, not the old8% linear change");
                }
            }
        }
    }
    drop(data);staging.unmap();
}
