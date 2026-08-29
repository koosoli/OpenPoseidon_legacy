// REN-TEMP-001M — NVIDIA DLSS Super Resolution over the NGX Vulkan C API.
//
// Compiled ONLY under the off-by-default `dlss` cargo feature: the NGX SDK is
// proprietary and GPL-incompatible, so a distributed build must never enable it
// (docs/temporal-upscaling-plan.md §0.1 — private-build posture). This module is the
// vendor boundary of plan Phase 13: no NGX type, constant or handle may leak out of it;
// callers speak in the renderer's own terms (extents, jitter, wgpu-hal raw handles).
//
// FFI is hand-written against .tmp-dlss-sdk/include/nvsdk_ngx_vk.h (the application-
// side C signatures — the NGX_SNIPPET_BUILD variants in the header are for NVIDIA's
// own snippet builds and have a DIFFERENT Init parameter list; using them is the
// classic silent-ABI-mismatch trap). The DLSS runtime `nvngx_dlss.dll` is found in the
// executable's folder — Deploy.ps1 places it there when the feature is built.
#![allow(non_snake_case, dead_code, clippy::too_many_arguments)]

use std::os::raw::{c_char, c_uint, c_void};

// --- raw NGX types -------------------------------------------------------------

// NVSDK_NGX_Result: 0x1 = success, 0xBAD0xxxx = failures (nvsdk_ngx_defs.h:96).
pub type NgxResult = u32;
pub const NGX_SUCCESS: NgxResult = 0x1;
// NVSDK_NGX_Version_API (nvsdk_ngx_defs.h:56).
const NGX_VERSION_API: u32 = 0x15;
// NVSDK_NGX_Feature_SuperSampling (nvsdk_ngx_defs.h:190).
const NGX_FEATURE_SUPERSAMPLING: u32 = 1;

// Opaque NGX objects.
#[repr(C)]
pub struct NgxParameter {
    _opaque: [u8; 0],
}
#[repr(C)]
pub struct NgxHandle {
    _opaque: [u8; 0],
}

// Vulkan handles: dispatchable = pointer-sized, non-dispatchable = 64-bit.
pub type VkInstance = *mut c_void;
pub type VkPhysicalDevice = *mut c_void;
pub type VkDevice = *mut c_void;
pub type VkCommandBuffer = *mut c_void;
pub type VkImage = u64;
pub type VkImageView = u64;

// VkImageSubresourceRange (Vulkan core, five u32s).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VkImageSubresourceRange {
    pub aspect_mask: u32,
    pub base_mip_level: u32,
    pub level_count: u32,
    pub base_array_layer: u32,
    pub layer_count: u32,
}

// NVSDK_NGX_ImageViewInfo_VK (nvsdk_ngx_defs_vk.h:53).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct NgxImageViewInfoVk {
    pub image_view: VkImageView,
    pub image: VkImage,
    pub subresource_range: VkImageSubresourceRange,
    pub format: u32, // VkFormat
    pub width: u32,
    pub height: u32,
}

// NVSDK_NGX_Resource_VK (nvsdk_ngx_defs_vk.h:93). The union's other member
// (BufferInfo: u64 + u32) is strictly smaller than ImageViewInfo, so declaring the
// image-view member alone preserves the C layout for the resources this module uses.
// Type 0 = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct NgxResourceVk {
    pub image_view_info: NgxImageViewInfoVk,
    pub ty: u32,
    pub read_write: bool,
}
pub const NGX_RESOURCE_VK_TYPE_IMAGEVIEW: u32 = 0;

// NVSDK_NGX_DLSS_Feature_Flags (nvsdk_ngx_defs.h:291-297).
pub const DLSS_FLAG_IS_HDR: i32 = 1 << 0;
pub const DLSS_FLAG_MV_LOWRES: i32 = 1 << 1;
pub const DLSS_FLAG_MV_JITTERED: i32 = 1 << 2;
pub const DLSS_FLAG_DEPTH_INVERTED: i32 = 1 << 3;
pub const DLSS_FLAG_AUTO_EXPOSURE: i32 = 1 << 6;

// NVSDK_NGX_PerfQuality_Value (nvsdk_ngx_defs.h): 0 MaxPerf, 1 Balanced, 2 MaxQuality,
// 3 UltraPerformance, 4 UltraQuality, 5 DLAA.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Quality {
    Performance = 0,
    Balanced = 1,
    Quality = 2,
    UltraPerformance = 3,
    Dlaa = 5,
}

unsafe extern "C" {
    fn NVSDK_NGX_VULKAN_RequiredExtensions(
        out_instance_count: *mut c_uint,
        out_instance_exts: *mut *mut *const c_char,
        out_device_count: *mut c_uint,
        out_device_exts: *mut *mut *const c_char,
    ) -> NgxResult;
    // Application-side signature (GIPA/GDPA/FeatureCommonInfo/Version) — see header note.
    fn NVSDK_NGX_VULKAN_Init(
        app_id: u64,
        app_data_path: *const u16, // wchar_t on Windows
        instance: VkInstance,
        physical_device: VkPhysicalDevice,
        device: VkDevice,
        gipa: *const c_void,
        gdpa: *const c_void,
        feature_info: *const c_void,
        sdk_version: u32,
    ) -> NgxResult;
    // The path NVIDIA recommends for applications without an NVIDIA-assigned id: any
    // stable project string + ENGINE_TYPE_CUSTOM.
    fn NVSDK_NGX_VULKAN_Init_with_ProjectID(
        project_id: *const c_char,
        engine_type: u32, // 0 = NVSDK_NGX_ENGINE_TYPE_CUSTOM
        engine_version: *const c_char,
        app_data_path: *const u16,
        instance: VkInstance,
        physical_device: VkPhysicalDevice,
        device: VkDevice,
        gipa: *const c_void,
        gdpa: *const c_void,
        feature_info: *const c_void,
        sdk_version: u32,
    ) -> NgxResult;
    fn NVSDK_NGX_VULKAN_Shutdown1(device: VkDevice) -> NgxResult;
    fn NVSDK_NGX_VULKAN_GetFeatureRequirements(
        instance: VkInstance,
        physical_device: VkPhysicalDevice,
        discovery: *const NgxFeatureDiscoveryInfo,
        out: *mut NgxFeatureRequirement,
    ) -> NgxResult;
    fn NVSDK_NGX_VULKAN_GetCapabilityParameters(out: *mut *mut NgxParameter) -> NgxResult;
    fn NVSDK_NGX_VULKAN_AllocateParameters(out: *mut *mut NgxParameter) -> NgxResult;
    fn NVSDK_NGX_VULKAN_DestroyParameters(params: *mut NgxParameter) -> NgxResult;
    fn NVSDK_NGX_VULKAN_CreateFeature1(
        device: VkDevice,
        cmd: VkCommandBuffer,
        feature: u32,
        params: *mut NgxParameter,
        out_handle: *mut *mut NgxHandle,
    ) -> NgxResult;
    fn NVSDK_NGX_VULKAN_EvaluateFeature_C(
        cmd: VkCommandBuffer,
        handle: *const NgxHandle,
        params: *const NgxParameter,
        progress: *const c_void,
    ) -> NgxResult;
    fn NVSDK_NGX_VULKAN_ReleaseFeature(handle: *mut NgxHandle) -> NgxResult;

    fn NVSDK_NGX_Parameter_SetI(p: *mut NgxParameter, name: *const c_char, v: i32);
    fn NVSDK_NGX_Parameter_SetUI(p: *mut NgxParameter, name: *const c_char, v: u32);
    fn NVSDK_NGX_Parameter_SetF(p: *mut NgxParameter, name: *const c_char, v: f32);
    fn NVSDK_NGX_Parameter_SetVoidPointer(p: *mut NgxParameter, name: *const c_char, v: *mut c_void);
    fn NVSDK_NGX_Parameter_GetI(p: *mut NgxParameter, name: *const c_char, out: *mut i32) -> NgxResult;
    fn NVSDK_NGX_Parameter_GetVoidPointer(
        p: *mut NgxParameter,
        name: *const c_char,
        out: *mut *mut c_void,
    ) -> NgxResult;
}

// Parameter-name string constants (nvsdk_ngx_defs.h) as NUL-terminated byte literals.
macro_rules! name {
    ($s:literal) => {
        concat!($s, "\0").as_ptr() as *const c_char
    };
}

// --- safe-ish wrapper ------------------------------------------------------------

// The extension lists DLSS needs at instance/device creation, as owned strings
// (the NGX arrays are static, but owning them decouples callers from FFI lifetimes).
pub fn required_extensions() -> Result<(Vec<String>, Vec<String>), NgxResult> {
    let mut icount: c_uint = 0;
    let mut iexts: *mut *const c_char = std::ptr::null_mut();
    let mut dcount: c_uint = 0;
    let mut dexts: *mut *const c_char = std::ptr::null_mut();
    let r = unsafe {
        NVSDK_NGX_VULKAN_RequiredExtensions(&mut icount, &mut iexts, &mut dcount, &mut dexts)
    };
    if r != NGX_SUCCESS {
        return Err(r);
    }
    let read = |arr: *mut *const c_char, n: c_uint| -> Vec<String> {
        (0..n as usize)
            .filter_map(|i| {
                let p = unsafe { *arr.add(i) };
                if p.is_null() {
                    return None;
                }
                Some(
                    unsafe { std::ffi::CStr::from_ptr(p) }
                        .to_string_lossy()
                        .into_owned(),
                )
            })
            .collect()
    };
    Ok((read(iexts, icount), read(dexts, dcount)))
}

// NVSDK_NGX_FeatureCommonInfo (nvsdk_ngx_defs.h:400): the snippet-DLL search-path list.
// Passed at init because the engine curates its own DLL search paths, and NGX's default
// probe (the executable folder) demonstrably misses there (FeatureInitResult was
// 0xBAD00004 FeatureNotFound with nvngx_dlss.dll sitting right next to the exe).
#[repr(C)]
struct NgxPathListInfo {
    path: *const *const u16,
    length: c_uint,
}
#[repr(C)]
struct NgxLoggingInfo {
    callback: *const c_void,
    minimum_level: u32,
    disable_other_sinks: bool,
}
#[repr(C)]
struct NgxFeatureCommonInfo {
    path_list: NgxPathListInfo,
    internal_data: *mut c_void,
    logging: NgxLoggingInfo,
}

// NVSDK_NGX_FeatureDiscoveryInfo family (nvsdk_ngx_defs.h:473-546), for the
// GetFeatureRequirements diagnostic — the driver's own answer to "why not".
#[repr(C)]
struct NgxProjectIdDescription {
    project_id: *const c_char,
    engine_type: u32,
    engine_version: *const c_char,
}
#[repr(C)]
struct NgxApplicationIdentifier {
    identifier_type: u32, // 1 = Project_Id
    // Union { ProjectIdDescription, u64 } — the struct member is the larger.
    project: NgxProjectIdDescription,
}
#[repr(C)]
struct NgxFeatureDiscoveryInfo {
    sdk_version: u32,
    feature_id: u32,
    identifier: NgxApplicationIdentifier,
    application_data_path: *const u16,
    feature_info: *const NgxFeatureCommonInfo,
}
#[repr(C)]
struct NgxFeatureRequirement {
    feature_supported: u32,
    min_hw_architecture: c_uint,
    min_os_version: [u8; 255],
}

// Ask the driver directly whether DLSS SR is supported on this adapter, BEFORE any
// device exists. Returns the raw support bitfield (0 = supported) or Err(ngx result).
pub fn feature_requirements(
    instance: VkInstance,
    physical_device: VkPhysicalDevice,
) -> Result<u32, NgxResult> {
    let data_path: Vec<u16> = ". ".encode_utf16().collect();
    let disco = NgxFeatureDiscoveryInfo {
        sdk_version: NGX_VERSION_API,
        feature_id: NGX_FEATURE_SUPERSAMPLING,
        identifier: NgxApplicationIdentifier {
            identifier_type: 1,
            project: NgxProjectIdDescription {
                project_id: name!("cc7a5a52-6d2e-4cf1-9e04-9a2f1c9b7f10"),
                engine_type: 0,
                engine_version: name!("0.1"),
            },
        },
        application_data_path: data_path.as_ptr(),
        feature_info: std::ptr::null(),
    };
    let mut req = NgxFeatureRequirement {
        feature_supported: u32::MAX,
        min_hw_architecture: 0,
        min_os_version: [0; 255],
    };
    let r = unsafe {
        NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physical_device, &disco, &mut req)
    };
    if r != NGX_SUCCESS {
        return Err(r);
    }
    Ok(req.feature_supported)
}

// NGX log hook: forwarded to stderr, which the capture harness records per run.
unsafe extern "C" fn ngx_log(msg: *const c_char, _level: u32, _component: u32) {
    if msg.is_null() {
        return;
    }
    let text = unsafe { std::ffi::CStr::from_ptr(msg) }.to_string_lossy();
    eprintln!("[ngx] {}", text.trim_end());
}

// One initialised NGX instance bound to a Vulkan device, plus the created DLSS
// feature. Drop order: feature, then shutdown — enforced by `release`.
pub struct Ngx {
    device: VkDevice,
    capability: *mut NgxParameter,
    params: *mut NgxParameter,
    feature: Option<*mut NgxHandle>,
}

// The NGX context is renderer-thread-confined like every other renderer resource.
unsafe impl Send for Ngx {}

pub struct OptimalSettings {
    pub render_width: u32,
    pub render_height: u32,
}

impl Ngx {
    // App id 0x0 + project-less init is permitted for internal/dev use; the data path
    // is where the driver may write NGX logs.
    pub unsafe fn init(
        instance: VkInstance,
        physical_device: VkPhysicalDevice,
        device: VkDevice,
    ) -> Result<Self, NgxResult> {
        // EVERYTHING handed to NGX at init is leaked to 'static on purpose: whether the
        // driver copies the FeatureCommonInfo/paths or stores the pointers and probes
        // lazily is undocumented, and a dangling stack pointer here would read back as
        // FeatureNotFound with no log anywhere. A few hundred bytes, once per process.
        fn wide(s: &str) -> &'static [u16] {
            Box::leak(
                s.encode_utf16()
                    .chain(std::iter::once(0))
                    .collect::<Vec<u16>>()
                    .into_boxed_slice(),
            )
        }
        // Absolute, writable data path (NGX logs/caches there).
        let data_dir = std::env::var("LOCALAPPDATA")
            .map(|l| format!("{l}\\OpenPoseidon\\ngx"))
            .unwrap_or_else(|_| ".".to_owned());
        let _ = std::fs::create_dir_all(&data_dir);
        let data_path: &'static [u16] = wide(&data_dir);
        // Explicit snippet search path: the executable's folder, where Deploy.ps1 puts
        // nvngx_dlss.dll.
        let exe_dir = std::env::current_exe()
            .ok()
            .and_then(|p| p.parent().map(|d| d.to_path_buf()))
            .map(|d| d.to_string_lossy().into_owned())
            .unwrap_or_else(|| ".".to_owned());
        let paths: &'static [*const u16] =
            Box::leak(vec![wide(&exe_dir).as_ptr()].into_boxed_slice());
        let common: &'static NgxFeatureCommonInfo = Box::leak(Box::new(NgxFeatureCommonInfo {
            path_list: NgxPathListInfo {
                path: paths.as_ptr(),
                length: 1,
            },
            internal_data: std::ptr::null_mut(),
            logging: NgxLoggingInfo {
                callback: ngx_log as *const c_void,
                minimum_level: 2, // verbose
                disable_other_sinks: false,
            },
        }));
        let r = unsafe {
            NVSDK_NGX_VULKAN_Init_with_ProjectID(
                // A stable project UUID + CUSTOM engine: NVIDIA's documented path for
                // applications without an assigned application id.
                name!("cc7a5a52-6d2e-4cf1-9e04-9a2f1c9b7f10"),
                0,
                name!("0.1"),
                data_path.as_ptr(),
                instance,
                physical_device,
                device,
                std::ptr::null(),
                std::ptr::null(),
                common as *const NgxFeatureCommonInfo as *const c_void,
                NGX_VERSION_API,
            )
        };
        if r != NGX_SUCCESS {
            return Err(r);
        }
        let mut capability: *mut NgxParameter = std::ptr::null_mut();
        let r = unsafe { NVSDK_NGX_VULKAN_GetCapabilityParameters(&mut capability) };
        if r != NGX_SUCCESS {
            unsafe { NVSDK_NGX_VULKAN_Shutdown1(device) };
            return Err(r);
        }
        let mut params: *mut NgxParameter = std::ptr::null_mut();
        let r = unsafe { NVSDK_NGX_VULKAN_AllocateParameters(&mut params) };
        if r != NGX_SUCCESS {
            unsafe { NVSDK_NGX_VULKAN_Shutdown1(device) };
            return Err(r);
        }
        Ok(Ngx {
            device,
            capability,
            params,
            feature: None,
        })
    }

    // Driver/hardware support for DLSS Super Resolution.
    pub fn supersampling_available(&self) -> bool {
        let mut v: i32 = 0;
        let r = unsafe {
            NVSDK_NGX_Parameter_GetI(self.capability, name!("SuperSampling.Available"), &mut v)
        };
        r == NGX_SUCCESS && v != 0
    }

    // Why SuperSampling is unavailable, in the driver's own words — logged on failure so
    // "unavailable" is diagnosable from a capture log (driver too old vs snippet DLL not
    // found vs feature denied).
    pub fn availability_diagnostics(&self) -> String {
        let get = |n: *const c_char| -> Option<i32> {
            let mut v: i32 = 0;
            (unsafe { NVSDK_NGX_Parameter_GetI(self.capability, n, &mut v) } == NGX_SUCCESS)
                .then_some(v)
        };
        format!(
            "available={:?} needs_driver_update={:?} min_driver={:?}.{:?} init_result={:#x?}",
            get(name!("SuperSampling.Available")),
            get(name!("SuperSampling.NeedsUpdatedDriver")),
            get(name!("SuperSampling.MinDriverVersionMajor")),
            get(name!("SuperSampling.MinDriverVersionMinor")),
            get(name!("SuperSampling.FeatureInitResult")),
        )
    }

    // The render resolution DLSS wants for a given output size + quality mode, from
    // the driver's own optimal-settings callback (never hard-coded).
    pub fn optimal_settings(
        &self,
        out_w: u32,
        out_h: u32,
        quality: Quality,
    ) -> Result<OptimalSettings, NgxResult> {
        type Callback = unsafe extern "C" fn(*mut NgxParameter) -> NgxResult;
        let mut cb_ptr: *mut c_void = std::ptr::null_mut();
        let r = unsafe {
            NVSDK_NGX_Parameter_GetVoidPointer(
                self.capability,
                name!("DLSSOptimalSettingsCallback"),
                &mut cb_ptr,
            )
        };
        if r != NGX_SUCCESS || cb_ptr.is_null() {
            return Err(r.max(1));
        }
        unsafe {
            NVSDK_NGX_Parameter_SetUI(self.capability, name!("Width"), out_w);
            NVSDK_NGX_Parameter_SetUI(self.capability, name!("Height"), out_h);
            NVSDK_NGX_Parameter_SetI(self.capability, name!("PerfQualityValue"), quality as i32);
            NVSDK_NGX_Parameter_SetI(self.capability, name!("RTXValue"), 0);
            let cb: Callback = std::mem::transmute(cb_ptr);
            let r = cb(self.capability);
            if r != NGX_SUCCESS {
                return Err(r);
            }
            let mut rw: i32 = 0;
            let mut rh: i32 = 0;
            NVSDK_NGX_Parameter_GetI(self.capability, name!("OutWidth"), &mut rw);
            NVSDK_NGX_Parameter_GetI(self.capability, name!("OutHeight"), &mut rh);
            if rw <= 0 || rh <= 0 {
                return Err(1);
            }
            Ok(OptimalSettings {
                render_width: rw as u32,
                render_height: rh as u32,
            })
        }
    }

    // Create the SR feature for these extents. Conventions per the renderer:
    // HDR input, render-res motion vectors, JITTERED raster, REVERSED depth,
    // exposure texture supplied by us (no auto-exposure).
    pub unsafe fn create_feature(
        &mut self,
        cmd: VkCommandBuffer,
        render: (u32, u32),
        output: (u32, u32),
        quality: Quality,
    ) -> Result<(), NgxResult> {
        self.release_feature();
        unsafe {
            NVSDK_NGX_Parameter_SetUI(self.params, name!("Width"), render.0);
            NVSDK_NGX_Parameter_SetUI(self.params, name!("Height"), render.1);
            NVSDK_NGX_Parameter_SetUI(self.params, name!("OutWidth"), output.0);
            NVSDK_NGX_Parameter_SetUI(self.params, name!("OutHeight"), output.1);
            NVSDK_NGX_Parameter_SetI(self.params, name!("PerfQualityValue"), quality as i32);
            NVSDK_NGX_Parameter_SetI(
                self.params,
                name!("DLSS.Feature.Create.Flags"),
                DLSS_FLAG_IS_HDR | DLSS_FLAG_MV_LOWRES | DLSS_FLAG_MV_JITTERED
                    | DLSS_FLAG_DEPTH_INVERTED,
            );
            let mut handle: *mut NgxHandle = std::ptr::null_mut();
            let r = NVSDK_NGX_VULKAN_CreateFeature1(
                self.device,
                cmd,
                NGX_FEATURE_SUPERSAMPLING,
                self.params,
                &mut handle,
            );
            if r != NGX_SUCCESS {
                return Err(r);
            }
            self.feature = Some(handle);
        }
        Ok(())
    }

    // Record one SR evaluation. `mv_scale` converts our uv-space motion (current ->
    // previous) into the pixel-space vectors DLSS consumes; jitter is in pixels.
    pub unsafe fn evaluate(
        &self,
        cmd: VkCommandBuffer,
        color: &mut NgxResourceVk,
        depth: &mut NgxResourceVk,
        motion: &mut NgxResourceVk,
        exposure: Option<&mut NgxResourceVk>,
        output: &mut NgxResourceVk,
        jitter_px: [f32; 2],
        mv_scale: [f32; 2],
        reset: bool,
        render: (u32, u32),
    ) -> Result<(), NgxResult> {
        let Some(feature) = self.feature else {
            return Err(1);
        };
        unsafe {
            let p = self.params;
            NVSDK_NGX_Parameter_SetVoidPointer(p, name!("Color"), color as *mut _ as *mut c_void);
            NVSDK_NGX_Parameter_SetVoidPointer(p, name!("Depth"), depth as *mut _ as *mut c_void);
            NVSDK_NGX_Parameter_SetVoidPointer(
                p,
                name!("MotionVectors"),
                motion as *mut _ as *mut c_void,
            );
            if let Some(exposure) = exposure {
                NVSDK_NGX_Parameter_SetVoidPointer(
                    p,
                    name!("ExposureTexture"),
                    exposure as *mut _ as *mut c_void,
                );
            }
            NVSDK_NGX_Parameter_SetVoidPointer(p, name!("Output"), output as *mut _ as *mut c_void);
            NVSDK_NGX_Parameter_SetF(p, name!("Jitter.Offset.X"), jitter_px[0]);
            NVSDK_NGX_Parameter_SetF(p, name!("Jitter.Offset.Y"), jitter_px[1]);
            NVSDK_NGX_Parameter_SetF(p, name!("MV.Scale.X"), mv_scale[0]);
            NVSDK_NGX_Parameter_SetF(p, name!("MV.Scale.Y"), mv_scale[1]);
            NVSDK_NGX_Parameter_SetI(p, name!("Reset"), i32::from(reset));
            NVSDK_NGX_Parameter_SetUI(p, name!("DLSS.Render.Subrect.Dimensions.Width"), render.0);
            NVSDK_NGX_Parameter_SetUI(p, name!("DLSS.Render.Subrect.Dimensions.Height"), render.1);
            let r = NVSDK_NGX_VULKAN_EvaluateFeature_C(cmd, feature, p, std::ptr::null());
            if r != NGX_SUCCESS {
                return Err(r);
            }
        }
        Ok(())
    }

    pub fn release_feature(&mut self) {
        if let Some(h) = self.feature.take() {
            unsafe { NVSDK_NGX_VULKAN_ReleaseFeature(h) };
        }
    }

    // Full teardown. Consumes self; the device must still be alive.
    pub fn release(mut self) {
        self.release_feature();
        unsafe {
            NVSDK_NGX_VULKAN_DestroyParameters(self.params);
            NVSDK_NGX_VULKAN_Shutdown1(self.device);
        }
    }
}

#[cfg(test)]
mod tests {
    // Link + call proof: RequiredExtensions is a static query into the NGX static lib
    // (no Vulkan objects, no driver). If the FFI signatures or the link line are wrong
    // this fails at build or crashes here — before any in-game bring-up.
    #[test]
    fn ngx_links_and_reports_required_extensions() {
        let (inst, dev) = super::required_extensions().expect("NGX RequiredExtensions");
        assert!(
            !inst.is_empty() && !dev.is_empty(),
            "expected non-empty extension lists, got {inst:?} / {dev:?}"
        );
        // Sanity: the canonical ones NGX has required since day one.
        assert!(
            inst.iter().any(|e| e.contains("VK_KHR_get_physical_device_properties2"))
                || dev.iter().any(|e| e.contains("VK_")),
            "extension lists look wrong: {inst:?} / {dev:?}"
        );
    }
}
