//! Diagnostic (WGR_UPLOAD_REPORT_MB=<MB>): bytes handed to `queue.write_*` by the streaming
//! paths, per frame. A frame that queued at least the threshold prints one `UPLOAD` line on
//! stderr, stamped with the same clock as `GPU_SPIKE` (gpu_timers.rs), so the two can be lined
//! up. Off (no env): one relaxed atomic add per upload call, nothing else.

use std::sync::atomic::{AtomicU64, Ordering};

pub(crate) enum Kind {
    Texture = 0,
    Pool = 1,
    Instances = 2,
    Tables = 3,
}

static BYTES: [AtomicU64; 4] = [AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0)];
static CALLS: [AtomicU64; 4] = [AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0)];

pub(crate) fn add(kind: Kind, bytes: usize) {
    let k = kind as usize;
    BYTES[k].fetch_add(bytes as u64, Ordering::Relaxed);
    CALLS[k].fetch_add(1, Ordering::Relaxed);
}

/// Milliseconds since the first call -- the shared clock of UPLOAD and GPU_SPIKE lines.
pub(crate) fn clock_ms() -> f64 {
    static START: std::sync::OnceLock<std::time::Instant> = std::sync::OnceLock::new();
    START.get_or_init(std::time::Instant::now).elapsed().as_secs_f64() * 1e3
}

fn threshold() -> Option<u64> {
    static T: std::sync::OnceLock<Option<u64>> = std::sync::OnceLock::new();
    *T.get_or_init(|| {
        std::env::var("WGR_UPLOAD_REPORT_MB").ok().and_then(|v| v.trim().parse::<f64>().ok()).map(|mb| (mb * 1048576.0) as u64)
    })
}

/// Called once per frame: report (above the threshold) and reset.
pub(crate) fn frame_end() {
    let Some(limit) = threshold() else { return };
    let take = |i: usize| (BYTES[i].swap(0, Ordering::Relaxed), CALLS[i].swap(0, Ordering::Relaxed));
    let rows = [take(0), take(1), take(2), take(3)];
    let total: u64 = rows.iter().map(|r| r.0).sum();
    if total >= limit {
        let mb = |b: u64| b as f64 / 1048576.0;
        eprintln!(
            "UPLOAD t={:.0} total={:.1}MB tex={:.1}MB/{} pool={:.1}MB/{} inst={:.1}MB/{} tables={:.1}MB/{}",
            clock_ms(), mb(total), mb(rows[0].0), rows[0].1, mb(rows[1].0), rows[1].1, mb(rows[2].0), rows[2].1,
            mb(rows[3].0), rows[3].1
        );
    }
}
