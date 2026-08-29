// REN-TEMP-001M — Vulkan instance/device creation with the NGX-required extensions,
// wrapped back into wgpu through wgpu-hal's from_raw/device_from_raw.
//
// wgpu's normal creation path has no hook for extra native extensions (the plan's §0.2
// finding), so when DLSS is requested the instance and device are created HERE with
// ash — wgpu's own desired/required extension lists UNIONed with NGX's — and handed to
// wgpu via `Instance::from_hal` / `Adapter::create_device_from_hal`. Everything else
// (surface, adapter selection, feature/limit negotiation) stays on the normal wgpu API.
//
// This module must remain a strict fallback: any error returns Err and the caller
// continues on the untouched native path. It is compiled only under the off-by-default
// `dlss` cargo feature and does nothing unless the runtime also asks (WGR_DLSS=1).
#![allow(dead_code)]

use ash::vk;
use ash::vk::Handle;
use std::ffi::CStr;
use std::os::raw::c_void;

// Raw handles NGX init needs. Dispatchable Vulkan handles are pointer-sized.
pub struct NgxRawHandles {
    pub instance: *mut c_void,
    pub physical_device: *mut c_void,
    pub device: *mut c_void,
}
unsafe impl Send for NgxRawHandles {}

fn leak_cstr(s: &str) -> &'static CStr {
    let owned = std::ffi::CString::new(s).expect("extension name with NUL");
    Box::leak(owned.into_boxed_c_str())
}

// Union wgpu's extension list with NGX's, deduped by name. NGX names arrive as owned
// strings and are leaked to satisfy wgpu-hal's `&'static CStr` contract — a handful of
// short strings, once per process.
fn union_extensions(base: Vec<&'static CStr>, ngx: &[String]) -> Vec<&'static CStr> {
    let mut out = base;
    for name in ngx {
        let c = leak_cstr(name);
        if !out.iter().any(|e| *e == c) {
            out.push(c);
        }
    }
    out
}

// Create a Vulkan-backed wgpu::Instance whose VkInstance carries the NGX instance
// extensions on top of everything wgpu itself wants.
pub fn create_instance(flags: wgpu::InstanceFlags) -> Result<wgpu::Instance, String> {
    let (ngx_instance_exts, _) =
        crate::dlss::required_extensions().map_err(|r| format!("NGX RequiredExtensions: {r:#x}"))?;
    let entry = unsafe { ash::Entry::load() }.map_err(|e| format!("vulkan loader: {e}"))?;
    // NGX wants a modern instance; cap at what the loader offers.
    let loader_version = unsafe { entry.try_enumerate_instance_version() }
        .ok()
        .flatten()
        .unwrap_or(vk::API_VERSION_1_1);
    let api_version = loader_version.min(vk::API_VERSION_1_3);
    let wgpu_exts =
        wgpu::hal::vulkan::Instance::desired_extensions(&entry, api_version, flags)
            .map_err(|e| format!("desired_extensions: {e}"))?;
    let extensions = union_extensions(wgpu_exts, &ngx_instance_exts);
    let ext_ptrs: Vec<*const i8> = extensions.iter().map(|e| e.as_ptr()).collect();
    let app_info = vk::ApplicationInfo::default()
        .application_name(c"OpenPoseidon")
        .api_version(api_version);
    let create_info = vk::InstanceCreateInfo::default()
        .application_info(&app_info)
        .enabled_extension_names(&ext_ptrs);
    let raw_instance = unsafe { entry.create_instance(&create_info, None) }
        .map_err(|e| format!("vkCreateInstance: {e}"))?;
    let hal_instance = unsafe {
        wgpu::hal::vulkan::Instance::from_raw(
            entry,
            raw_instance,
            api_version,
            0,
            None, // no debug-utils messenger on this path
            extensions,
            flags,
            wgpu::MemoryBudgetThresholds::default(),
            false, // has_nv_optimus
            None,  // drop_callback — hal owns and destroys the instance
        )
    }
    .map_err(|e| format!("hal Instance::from_raw: {e}"))?;
    Ok(unsafe { wgpu::Instance::from_hal::<wgpu::hal::api::Vulkan>(hal_instance) })
}

// Create the device with NGX's device extensions and hand back (Device, Queue) plus the
// raw handles NGX init needs. `features`/`limits` are the same negotiated values the
// normal `request_device` call would use.
pub fn create_device(
    adapter: &wgpu::Adapter,
    features: wgpu::Features,
    limits: &wgpu::Limits,
) -> Result<(wgpu::Device, wgpu::Queue, NgxRawHandles), String> {
    let (_, ngx_device_exts) =
        crate::dlss::required_extensions().map_err(|r| format!("NGX RequiredExtensions: {r:#x}"))?;
    let hal_adapter = unsafe { adapter.as_hal::<wgpu::hal::api::Vulkan>() }
        .ok_or("adapter is not Vulkan (DLSS requires the Vulkan backend)")?;
    let phys = hal_adapter.raw_physical_device();
    let shared = hal_adapter.shared_instance();
    let raw_instance = shared.raw_instance();

    let extensions = union_extensions(
        hal_adapter.required_device_extensions(features),
        &ngx_device_exts,
    );
    let ext_ptrs: Vec<*const i8> = extensions.iter().map(|e| e.as_ptr()).collect();
    let mut phys_features = hal_adapter.physical_device_features(&extensions, features);

    // First graphics+compute family — the same family wgpu's own open() would pick.
    let families =
        unsafe { raw_instance.get_physical_device_queue_family_properties(phys) };
    let family_index = families
        .iter()
        .position(|f| {
            f.queue_flags
                .contains(vk::QueueFlags::GRAPHICS | vk::QueueFlags::COMPUTE)
        })
        .ok_or("no graphics+compute queue family")? as u32;
    let priorities = [1.0f32];
    let queue_info = [vk::DeviceQueueCreateInfo::default()
        .queue_family_index(family_index)
        .queue_priorities(&priorities)];
    let pre_info = vk::DeviceCreateInfo::default()
        .queue_create_infos(&queue_info)
        .enabled_extension_names(&ext_ptrs);
    let device_info = phys_features.add_to_device_create(pre_info);
    let raw_device = unsafe { raw_instance.create_device(phys, &device_info, None) }
        .map_err(|e| format!("vkCreateDevice: {e}"))?;
    let raw_device_handle = raw_device.handle();

    let open = unsafe {
        hal_adapter.device_from_raw(
            raw_device,
            None, // hal owns and destroys the device
            &extensions,
            features,
            limits,
            &wgpu::MemoryHints::default(),
            family_index,
            0,
        )
    }
    .map_err(|e| format!("hal device_from_raw: {e}"))?;
    let (device, queue) = unsafe {
        adapter.create_device_from_hal::<wgpu::hal::api::Vulkan>(
            open,
            &wgpu::DeviceDescriptor {
                label: Some("wgr_device_dlss"),
                required_features: features,
                required_limits: limits.clone(),
                ..Default::default()
            },
        )
    }
    .map_err(|e| format!("create_device_from_hal: {e}"))?;
    Ok((
        device,
        queue,
        NgxRawHandles {
            instance: raw_instance.handle().as_raw() as usize as *mut c_void,
            physical_device: phys.as_raw() as usize as *mut c_void,
            device: raw_device_handle.as_raw() as usize as *mut c_void,
        },
    ))
}
