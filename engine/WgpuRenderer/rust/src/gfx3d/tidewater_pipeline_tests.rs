// Every Tidewater pipeline (water, spray, breaker lips, probe, swash field, ...) built on a real device
// against the renderer's real frame layout. naga validates a shader on its own; it cannot see that a
// stage reads a binding the layout does not expose to it. On 2026-10-12 the spray started reading the
// sky SH (@group(0) @binding(9)) in its vertex stage while the layout exposed it to FRAGMENT | COMPUTE
// only: every unit test passed, and in game the invalid pipeline froze the frame (the screenshots
// repeated an old one) with "Error matching ShaderStages(VERTEX) shader requirements".

#[test]
fn tidewater_pipelines_build_against_the_real_frame_layout() {
    let instance = wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
    let Ok(adapter) = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
        compatible_surface: None,
        force_fallback_adapter: false,
        ..Default::default()
    })) else {
        eprintln!("tidewater pipeline test skipped: no headless adapter");
        return;
    };
    let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
        required_limits: adapter.limits(),
        // everything the adapter has but the experimental features, which need an opt-in
        required_features: adapter.features().difference(
            wgpu::Features::EXPERIMENTAL_RAY_QUERY
                | wgpu::Features::EXPERIMENTAL_MESH_SHADER
                | wgpu::Features::EXPERIMENTAL_RAY_HIT_VERTEX_RETURN
                | wgpu::Features::EXPERIMENTAL_MESH_SHADER_MULTIVIEW
                | wgpu::Features::EXPERIMENTAL_MESH_SHADER_POINTS
                | wgpu::Features::EXPERIMENTAL_COOPERATIVE_MATRIX,
        ),
        ..Default::default()
    }))
    .unwrap_or_else(|e| panic!("tidewater pipeline test device: {e}"));
    let scope = device.push_error_scope(wgpu::ErrorFilter::Validation);
    let camera = super::CameraGroup::new(&device);
    let _tw = crate::water_tw::Tidewater::new(&device, &queue, &camera.layout, wgpu::TextureFormat::Rgba16Float, 1);
    let error = pollster::block_on(scope.pop());
    assert!(error.is_none(), "Tidewater pipeline validation against the frame layout: {error:?}");
}
