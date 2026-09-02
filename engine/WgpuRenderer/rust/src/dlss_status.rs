// DLSS status — WHY DLSS is (in)active, as one specific sentence.
//
// Compiled in EVERY build, dlss feature or not: a plain build has a reason too ("this
// build was compiled without DLSS"), and a tester reading the dev panel must never see a
// bare "inactive". Every gate between "the GPU could do DLSS" and "DLSS ran this frame"
// reports through here, in the order the gates are evaluated at startup:
//
//   build feature -> WGR_DLSS opt-out -> nvngx_dlss.dll beside the exe -> Vulkan hal
//   instance -> adapter vendor/backend -> hal device -> driver requirements probe ->
//   NGX init -> SuperSampling.Available -> (per frame) temporal path / panel switch /
//   feature create / evaluate.
//
// Tester reports 2026-09-02 motivated this: several DLSS-capable machines showed
// "DLSS route up | DLSS inactive" with nothing else, and the package they had been sent
// held the exe and the renderer DLL but not the snippet. The first gate that would have
// named that was a WARN line that only appears after the first upscaled frame.
//
// Everything in this module is pure (no NGX, no GPU) so it is unit-testable; the dlss
// feature module hands it raw codes and it hands back words.

use std::path::{Path, PathBuf};

pub const SNIPPET_DLL: &str = "nvngx_dlss.dll";

// NVSDK_NGX_Result values (nvsdk_ngx_defs.h:99-177). The low bits of a 0xBAD0xxxx code
// name the failure; the names are what a tester can search for.
pub fn ngx_result_name(code: u32) -> &'static str {
    match code {
        0x1 => "Success",
        0xBAD00000 => "Fail",
        0xBAD00001 => "FeatureNotSupported",
        0xBAD00002 => "PlatformError",
        0xBAD00003 => "FeatureAlreadyExists",
        0xBAD00004 => "FeatureNotFound",
        0xBAD00005 => "InvalidParameter",
        0xBAD00006 => "ScratchBufferTooSmall",
        0xBAD00007 => "NotInitialized",
        0xBAD00008 => "UnsupportedInputFormat",
        0xBAD00009 => "RWFlagMissing",
        0xBAD0000A => "MissingInput",
        0xBAD0000B => "UnableToInitializeFeature",
        0xBAD0000C => "OutOfDate",
        0xBAD0000D => "OutOfGPUMemory",
        0xBAD0000E => "UnsupportedFormat",
        0xBAD0000F => "UnableToWriteToAppDataPath",
        0xBAD00010 => "UnsupportedParameter",
        0xBAD00011 => "Denied",
        0xBAD00012 => "NotImplemented",
        _ => "unknown",
    }
}

// "0xbad00004 FeatureNotFound" — the code and its name together, every time.
pub fn ngx_code(code: u32) -> String {
    format!("{code:#x} {}", ngx_result_name(code))
}

// NVSDK_NGX_FeatureSupportResult bitfield from GetFeatureRequirements
// (nvsdk_ngx_defs.h:439-444). 0 = supported.
pub const SUPPORT_CHECK_NOT_PRESENT: u32 = 1;
pub const SUPPORT_DRIVER_TOO_OLD: u32 = 2;
pub const SUPPORT_ADAPTER_UNSUPPORTED: u32 = 4;
pub const SUPPORT_OS_TOO_OLD: u32 = 8;
pub const SUPPORT_NOT_IMPLEMENTED: u32 = 16;

// The driver's own verdict, in words. `adapter` is the wgpu adapter name so the
// sentence names the card the tester is actually running on.
pub fn requirements_reason(bits: u32, adapter: &str) -> String {
    let mut parts: Vec<String> = Vec::new();
    if bits & SUPPORT_ADAPTER_UNSUPPORTED != 0 {
        parts.push(format!(
            "adapter {adapter} has no DLSS support (needs an RTX-class GPU with tensor cores)"
        ));
    }
    if bits & SUPPORT_DRIVER_TOO_OLD != 0 {
        parts.push("NVIDIA driver too old for DLSS (update the driver)".to_owned());
    }
    if bits & SUPPORT_OS_TOO_OLD != 0 {
        parts.push("OS version below the DLSS minimum".to_owned());
    }
    if bits & SUPPORT_CHECK_NOT_PRESENT != 0 {
        parts.push("driver has no NGX feature check (driver too old for NGX)".to_owned());
    }
    if bits & SUPPORT_NOT_IMPLEMENTED != 0 {
        parts.push("NGX reports the support check as not implemented on this driver".to_owned());
    }
    if parts.is_empty() {
        parts.push(format!("driver reports DLSS unsupported (bits {bits:#x})"));
    }
    format!("{} [GetFeatureRequirements bits {bits:#x}]", parts.join("; "))
}

// The NGX capability block after a successful init, decoded. `init_result` is
// SuperSampling.FeatureInitResult — the code that says WHY the feature is unavailable
// even though NGX itself came up (FeatureNotFound = the snippet DLL did not load).
pub fn capability_reason(
    needs_driver_update: Option<i32>,
    min_driver: (Option<i32>, Option<i32>),
    init_result: Option<i32>,
    snippet_path: Option<&Path>,
) -> String {
    if needs_driver_update.unwrap_or(0) != 0 {
        return match min_driver {
            (Some(maj), Some(min)) => {
                format!("NVIDIA driver too old for this DLSS runtime (needs at least {maj}.{min})")
            }
            _ => "NVIDIA driver too old for this DLSS runtime (update the driver)".to_owned(),
        };
    }
    match init_result.map(|v| v as u32) {
        Some(0xBAD00004) => match snippet_path {
            Some(p) => format!(
                "NGX could not load {SNIPPET_DLL} from {} (FeatureInitResult {})",
                p.display(),
                ngx_code(0xBAD00004)
            ),
            None => format!(
                "{SNIPPET_DLL} not found next to the executable (FeatureInitResult {})",
                ngx_code(0xBAD00004)
            ),
        },
        Some(code) if code != 0x1 && code != 0 => {
            format!("NGX refused the DLSS feature (FeatureInitResult {})", ngx_code(code))
        }
        _ => "NGX reports SuperSampling.Available = 0 with no further detail".to_owned(),
    }
}

// Where the snippet may be: the executable's folder first (Deploy.ps1 puts it there),
// then WGR_NGX_SNIPPET_DIR (the headless tests run from target/debug/deps). Same list,
// same order, as the FeatureCommonInfo path list handed to NGX at init.
pub fn snippet_search_dirs(exe_dir: Option<PathBuf>, extra: Option<String>) -> Vec<PathBuf> {
    let mut dirs = Vec::new();
    if let Some(d) = exe_dir {
        dirs.push(d);
    }
    if let Some(e) = extra {
        dirs.push(PathBuf::from(e));
    }
    dirs
}

// The first directory that actually holds nvngx_dlss.dll, or None.
pub fn find_snippet(dirs: &[PathBuf]) -> Option<PathBuf> {
    dirs.iter()
        .map(|d| d.join(SNIPPET_DLL))
        .find(|p| p.is_file())
}

pub fn snippet_missing_reason(dirs: &[PathBuf]) -> String {
    match dirs.first() {
        Some(d) => format!(
            "{SNIPPET_DLL} not found next to the executable ({}); copy it beside OpenPoseidon.exe",
            d.display()
        ),
        None => format!("{SNIPPET_DLL} not found (executable folder unknown)"),
    }
}

// Reasons decided before any hardware is touched.
pub const REASON_BUILD_WITHOUT_DLSS: &str =
    "this build was compiled without DLSS support (POSEIDON_DLSS=OFF); FSR 1 is the upscaler";
pub const REASON_OPTED_OUT: &str =
    "DLSS turned off (WGR_DLSS=0 — Options > Upscaler is Off or FSR, or the env var)";

// wgpu adapter facts that rule DLSS out before NGX is asked.
pub const VENDOR_NVIDIA: u32 = 0x10DE;

pub fn adapter_reason(name: &str, vendor: u32, backend: &str) -> Option<String> {
    if backend != "Vulkan" {
        return Some(format!(
            "adapter {name} is on the {backend} backend; DLSS needs Vulkan (WGPU_BACKEND=vulkan)"
        ));
    }
    if vendor != VENDOR_NVIDIA {
        return Some(format!(
            "adapter is {name} (vendor {vendor:#06x}), not NVIDIA — on a laptop, check the discrete GPU is selected for OpenPoseidon.exe"
        ));
    }
    None
}

// Per-frame reasons from the backend.
pub const REASON_TEMPORAL_OFF: &str =
    "DLSS needs the temporal path (jitter + motion vectors), which is off (WGR_TEMPORAL=0 or the dev panel)";
pub const REASON_PANEL_OFF: &str = "DLSS switched off in the dev panel / Options";

pub fn init_failed_reason(code: u32) -> String {
    format!("NGX init failed ({})", ngx_code(code))
}

pub fn create_failed_reason(code: u32) -> String {
    format!("NGX CreateFeature failed ({})", ngx_code(code))
}

pub fn evaluate_failed_reason(code: u32) -> String {
    format!("NGX EvaluateFeature failed ({})", ngx_code(code))
}

// The ablation switches (dlss builds). They make init report the failure a machine
// WITHOUT DLSS would report, at the same gate, so the fallback path can be exercised on
// a machine that has it. Distinct from WGR_DLSS=0, which is the user opting out and
// skips the hardware gates entirely.
//
//   WGR_DLSS_FORCE_UNAVAILABLE=1  GetFeatureRequirements says AdapterUnsupported (a GTX
//                                 1070 with a current driver)
//   WGR_DLSS_FORCE_UNAVAILABLE=2  requirements pass, NGX init succeeds, but
//                                 SuperSampling.Available = 0 (snippet failed to load)
//   WGR_DLSS_FORCE_FAIL=init      NGX init returns UnableToInitializeFeature
//   WGR_DLSS_FORCE_FAIL=create    CreateFeature returns FeatureNotSupported — AFTER the
//                                 frame has been split for the raw command buffer
//   WGR_DLSS_FORCE_FAIL=evaluate  EvaluateFeature returns Fail every frame
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct ForcedFailure {
    pub requirements_unsupported: bool,
    pub capability_unavailable: bool,
    pub init: bool,
    pub create: bool,
    pub evaluate: bool,
}

impl ForcedFailure {
    pub fn parse(unavailable: Option<&str>, fail: Option<&str>) -> Self {
        let mut f = ForcedFailure::default();
        match unavailable.map(str::trim) {
            Some("1") => f.requirements_unsupported = true,
            Some("2") => f.capability_unavailable = true,
            _ => {}
        }
        match fail.map(|s| s.trim().to_ascii_lowercase()).as_deref() {
            Some("init") => f.init = true,
            Some("create") => f.create = true,
            Some("evaluate") | Some("eval") => f.evaluate = true,
            _ => {}
        }
        f
    }

    pub fn from_env() -> Self {
        Self::parse(
            std::env::var("WGR_DLSS_FORCE_UNAVAILABLE").ok().as_deref(),
            std::env::var("WGR_DLSS_FORCE_FAIL").ok().as_deref(),
        )
    }

    pub fn any(&self) -> bool {
        self.requirements_unsupported || self.capability_unavailable || self.init || self.create || self.evaluate
    }
}

// The renderer-wide record. Two tiers: a LATCHED reason (a startup gate, or the backend
// dying — permanent for this process, restart to retry) and a PER-FRAME reason (the
// panel switch, the temporal path, SSAA — recomputed every frame, so toggling a control
// changes the sentence immediately). `active` means DLSS produced the last frame.
#[derive(Clone, Debug, Default)]
pub struct DlssStatus {
    latched: Option<String>,
    frame: Option<String>,
    active: bool,
}

impl DlssStatus {
    pub fn latched(reason: impl Into<String>) -> Self {
        DlssStatus {
            latched: Some(reason.into()),
            frame: None,
            active: false,
        }
    }

    // A permanent reason. The FIRST one wins: a consequence ("temporal path off after
    // the fallback") must never overwrite its cause ("snippet missing").
    pub fn latch(&mut self, reason: impl Into<String>) {
        if self.latched.is_none() {
            self.latched = Some(reason.into());
        }
        self.active = false;
    }

    pub fn is_latched(&self) -> bool {
        self.latched.is_some()
    }

    // Start of a frame: nothing has run yet.
    pub fn begin_frame(&mut self) {
        self.active = false;
        self.frame = None;
    }

    // This frame's reason, when no permanent one exists.
    pub fn frame_reason(&mut self, reason: impl Into<String>) {
        if self.latched.is_none() && self.frame.is_none() {
            self.frame = Some(reason.into());
        }
    }

    pub fn set_active(&mut self) {
        if self.latched.is_none() {
            self.active = true;
            self.frame = None;
        }
    }

    pub fn is_active(&self) -> bool {
        self.active
    }

    // What the dev panel prints after "DLSS:" and what the log says.
    pub fn text(&self) -> &str {
        if let Some(l) = self.latched.as_deref() {
            l
        } else if self.active {
            "active"
        } else if let Some(f) = self.frame.as_deref() {
            f
        } else {
            "inactive (no upscaled frame has run yet)"
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ngx_codes_name_the_failure_and_keep_the_hex() {
        assert_eq!(ngx_code(0xBAD00004), "0xbad00004 FeatureNotFound");
        assert_eq!(ngx_code(0xBAD0000C), "0xbad0000c OutOfDate");
        assert_eq!(ngx_code(0x1), "0x1 Success");
        assert_eq!(ngx_code(0xDEADBEEF), "0xdeadbeef unknown");
    }

    // A GTX 1070 with a current driver: the adapter bit alone.
    #[test]
    fn requirements_name_the_adapter_for_a_pascal_card() {
        let r = requirements_reason(SUPPORT_ADAPTER_UNSUPPORTED, "NVIDIA GeForce GTX 1070");
        assert!(r.contains("GTX 1070"), "{r}");
        assert!(r.contains("no DLSS support"), "{r}");
        assert!(r.contains("bits 0x4"), "{r}");
    }

    #[test]
    fn requirements_list_every_set_bit() {
        let r = requirements_reason(
            SUPPORT_DRIVER_TOO_OLD | SUPPORT_OS_TOO_OLD,
            "NVIDIA GeForce RTX 3070",
        );
        assert!(r.contains("driver too old"), "{r}");
        assert!(r.contains("OS version"), "{r}");
        assert!(!r.contains("no DLSS support"), "{r}");
    }

    #[test]
    fn requirements_unknown_bits_still_say_something_specific() {
        let r = requirements_reason(0x40, "X");
        assert!(r.contains("bits 0x40"), "{r}");
    }

    // The tester package case: NGX up, snippet absent -> FeatureNotFound.
    #[test]
    fn capability_feature_not_found_without_snippet_names_the_missing_file() {
        let r = capability_reason(Some(0), (None, None), Some(0xBAD00004u32 as i32), None);
        assert!(r.starts_with("nvngx_dlss.dll not found next to the executable"), "{r}");
        assert!(r.contains("FeatureNotFound"), "{r}");
    }

    #[test]
    fn capability_feature_not_found_with_snippet_present_says_it_did_not_load() {
        let p = PathBuf::from("C:/game/nvngx_dlss.dll");
        let r = capability_reason(Some(0), (None, None), Some(0xBAD00004u32 as i32), Some(&p));
        assert!(r.contains("could not load nvngx_dlss.dll from"), "{r}");
        assert!(r.contains("C:/game/nvngx_dlss.dll"), "{r}");
    }

    #[test]
    fn capability_driver_update_wins_and_quotes_the_minimum() {
        let r = capability_reason(Some(1), (Some(531), Some(0)), Some(0xBAD00004u32 as i32), None);
        assert_eq!(r, "NVIDIA driver too old for this DLSS runtime (needs at least 531.0)");
    }

    #[test]
    fn capability_other_init_codes_are_named() {
        let r = capability_reason(Some(0), (None, None), Some(0xBAD00011u32 as i32), None);
        assert!(r.contains("Denied"), "{r}");
        let r = capability_reason(None, (None, None), None, None);
        assert!(r.contains("SuperSampling.Available = 0"), "{r}");
    }

    #[test]
    fn snippet_lookup_prefers_the_exe_dir_then_the_override() {
        let tmp = std::env::temp_dir().join(format!("wgr-dlss-status-{}", std::process::id()));
        let exe = tmp.join("exe");
        let extra = tmp.join("extra");
        std::fs::create_dir_all(&exe).unwrap();
        std::fs::create_dir_all(&extra).unwrap();
        let dirs = snippet_search_dirs(Some(exe.clone()), Some(extra.to_string_lossy().into_owned()));
        assert_eq!(dirs, vec![exe.clone(), extra.clone()]);
        assert_eq!(find_snippet(&dirs), None);
        let reason = snippet_missing_reason(&dirs);
        assert!(reason.starts_with("nvngx_dlss.dll not found next to the executable ("), "{reason}");
        assert!(reason.contains(&exe.to_string_lossy().into_owned()), "{reason}");

        std::fs::write(extra.join(SNIPPET_DLL), b"x").unwrap();
        assert_eq!(find_snippet(&dirs), Some(extra.join(SNIPPET_DLL)));
        std::fs::write(exe.join(SNIPPET_DLL), b"x").unwrap();
        assert_eq!(find_snippet(&dirs), Some(exe.join(SNIPPET_DLL)));
        let _ = std::fs::remove_dir_all(&tmp);
    }

    #[test]
    fn adapter_gate_names_the_igpu_and_the_backend() {
        let r = adapter_reason("Intel(R) UHD Graphics 630", 0x8086, "Vulkan").unwrap();
        assert!(r.contains("Intel(R) UHD Graphics 630"), "{r}");
        assert!(r.contains("not NVIDIA"), "{r}");
        assert!(r.contains("discrete GPU"), "{r}");
        let r = adapter_reason("NVIDIA GeForce RTX 4070", VENDOR_NVIDIA, "Dx12").unwrap();
        assert!(r.contains("Dx12 backend"), "{r}");
        assert!(r.contains("needs Vulkan"), "{r}");
        assert_eq!(adapter_reason("NVIDIA GeForce RTX 4070", VENDOR_NVIDIA, "Vulkan"), None);
    }

    #[test]
    fn forced_failures_parse_each_switch_and_nothing_else() {
        assert_eq!(ForcedFailure::parse(None, None), ForcedFailure::default());
        assert!(!ForcedFailure::parse(None, None).any());
        assert!(ForcedFailure::parse(Some("1"), None).requirements_unsupported);
        assert!(ForcedFailure::parse(Some("2"), None).capability_unavailable);
        assert!(!ForcedFailure::parse(Some("0"), None).any());
        assert!(ForcedFailure::parse(None, Some("init")).init);
        assert!(ForcedFailure::parse(None, Some("CREATE ")).create);
        assert!(ForcedFailure::parse(None, Some("evaluate")).evaluate);
        assert!(ForcedFailure::parse(None, Some("eval")).evaluate);
        assert!(!ForcedFailure::parse(None, Some("bogus")).any());
    }

    // The first permanent reason is the cause; consequences must not overwrite it, and
    // per-frame reasons never mask it.
    #[test]
    fn status_latches_the_first_permanent_reason() {
        let mut s = DlssStatus::default();
        assert_eq!(s.text(), "inactive (no upscaled frame has run yet)");
        s.latch("nvngx_dlss.dll not found");
        s.latch(REASON_TEMPORAL_OFF);
        assert_eq!(s.text(), "nvngx_dlss.dll not found");
        assert!(s.is_latched());
        s.begin_frame();
        s.frame_reason(REASON_PANEL_OFF);
        assert_eq!(s.text(), "nvngx_dlss.dll not found");
        s.set_active();
        assert!(!s.is_active());
        assert_eq!(s.text(), "nvngx_dlss.dll not found");
    }

    // Per-frame reasons follow the controls frame by frame; active clears them.
    #[test]
    fn status_frame_reasons_are_recomputed_each_frame() {
        let mut s = DlssStatus::default();
        s.begin_frame();
        s.frame_reason(REASON_PANEL_OFF);
        s.frame_reason(REASON_TEMPORAL_OFF);
        assert_eq!(s.text(), REASON_PANEL_OFF);
        s.begin_frame();
        s.frame_reason(REASON_TEMPORAL_OFF);
        assert_eq!(s.text(), REASON_TEMPORAL_OFF);
        s.begin_frame();
        s.set_active();
        assert!(s.is_active());
        assert_eq!(s.text(), "active");
        s.begin_frame();
        assert!(!s.is_active());
        s.latch("NGX EvaluateFeature failed (0xbad00000 Fail)");
        assert_eq!(s.text(), "NGX EvaluateFeature failed (0xbad00000 Fail)");
        assert_eq!(DlssStatus::latched("x").text(), "x");
    }

    #[test]
    fn build_and_opt_out_reasons_name_the_switch() {
        assert!(REASON_BUILD_WITHOUT_DLSS.contains("POSEIDON_DLSS"));
        assert!(REASON_OPTED_OUT.contains("WGR_DLSS=0"));
        assert!(init_failed_reason(0xBAD0000B).contains("UnableToInitializeFeature"));
        assert!(create_failed_reason(0xBAD00001).contains("FeatureNotSupported"));
        assert!(evaluate_failed_reason(0xBAD00000).contains("0xbad00000 Fail"));
    }
}
