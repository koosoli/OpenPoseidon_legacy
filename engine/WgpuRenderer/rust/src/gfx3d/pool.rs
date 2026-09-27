// Merged geometry pool (docs/gpu-culling-and-depth-plan.md §2.1) — Stage 1 of
// GPU-driven rendering.
//
// Every resident mesh's vertices and indices are suballocated into ONE shared vertex
// buffer + ONE shared index buffer, replacing the dedicated vbuf/ibuf that
// mesh_create used to hand each Shape/LOD. Indirect multi-draw (Stage 2/3) cannot
// rebind vertex/index buffers between sub-draws, so all drawable geometry has to live
// in one buffer pair, each section addressed by {base_vertex, first_index,
// index_count}. This module owns that pair plus a free-list suballocator over each,
// growing (realloc + GPU copy of the used prefix) when a map load exhausts them. The
// rare load/unload of geometry (map changes) frees ranges back, coalescing neighbours.
//
// Indices are widened to Uint32 (the pool spans well past the u16 vertex range) and
// stored MESH-LOCAL (0-based within each mesh, values unchanged by the widen). A draw
// binds the pool's vertex buffer SLICED to the mesh's `vbase` and uses base_vertex = 0,
// so @builtin(vertex_index) stays mesh-local exactly as it was with per-mesh buffers
// (see draw_one); the index buffer is bound whole and the draw's index range is offset
// by the mesh's `ibase`.

use crate::ffi::WgrMeshVertex;

const VERT_SIZE: u64 = std::mem::size_of::<WgrMeshVertex>() as u64; // 36
const INDEX_SIZE: u64 = 4; // Uint32

// How much bigger the pool gets when it runs out. 2.0 (the historical behaviour) is the
// DEFAULT and stays the default.
//
// Why it is a lever at all: growth allocates the new buffer BEFORE releasing the old one (it
// has to — the used prefix is copied across), so a doubling costs 3x the current size in peak
// VRAM, not 2x. Measured on DayZ Chernarus: the vertex buffer doubled 604 MB -> 1.21 GB, which
// needs 1.81 GB live at the moment of the copy, on top of ~4 GB of resident textures on an 8 GB
// card. `create_buffer` came back INVALID (that is how wgpu reports out of memory) and every
// later draw was rejected with "Buffer with 'wgr_geo_pool_vbuf' label is invalid". The same run
// finished with 2.28 GB of pool capacity against 1.44 GB live — 839 MB of pure doubling slack.
//
// A factor of 1.5 makes that step 604 -> 906 MB (1.51 GB peak instead of 1.81 GB) and lands
// closer to the live figure, at the cost of more growth copies. It is NOT on by default because
// each extra growth is an extra full-pool GPU copy plus a bind-group rebuild, and the trade is
// worth measuring rather than assuming. Clamped to [1.25, 2.0]: below 1.25 the copy count grows
// faster than the memory saved.
/// RFG-076: the pool buffers' current byte sizes, reported from lib.rs on the frame after
/// they change. A native Reforger world drove the pool past what the device would
/// allocate, `create_buffer` came back invalid, and there was no number anywhere.
/// RFG-079: the last 16 pool buffer events in order -- (re)make with bytes, retire,
/// and the retired-list clear that destroys them. `wgr_geo_pool_ibuf` was reported
/// INVALID at a write while the pool was 17 MB; that is not out-of-memory, it is a
/// question of which generation of the buffer was written when, and only the order
/// can answer it.
pub(crate) static GEO_POOL_EVENTS: std::sync::Mutex<Vec<String>> = std::sync::Mutex::new(Vec::new());
pub(crate) fn pool_event(what: &str) {
    if let Ok(mut g) = GEO_POOL_EVENTS.lock() {
        if g.len() >= 16 {
            g.remove(0);
        }
        g.push(what.to_string());
    }
}
pub(crate) fn geometry_pool_events_log_line() -> Option<String> {
    let taken: Vec<String> = GEO_POOL_EVENTS.lock().ok().map(|mut g| std::mem::take(&mut *g)).unwrap_or_default();
    if taken.is_empty() {
        return None;
    }
    Some(format!("geometry pool events: {}", taken.join(" > ")))
}

pub(crate) static GEO_POOL_VBUF_BYTES: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
pub(crate) static GEO_POOL_IBUF_BYTES: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
static GEO_POOL_REPORTED: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

/// One line whenever the pool has been (re)made since the last report, else None.
pub(crate) fn geometry_pool_log_line() -> Option<String> {
    use std::sync::atomic::Ordering::Relaxed;
    let v = GEO_POOL_VBUF_BYTES.load(Relaxed);
    let i = GEO_POOL_IBUF_BYTES.load(Relaxed);
    let key = v ^ (i << 1);
    if key == 0 || GEO_POOL_REPORTED.swap(key, Relaxed) == key {
        return None;
    }
    Some(format!(
        "geometry pool: vertex buffer {:.1} MB, index buffer {:.1} MB",
        v as f64 / 1048576.0,
        i as f64 / 1048576.0
    ))
}

fn growth_factor() -> f64 {
    static FACTOR: std::sync::OnceLock<f64> = std::sync::OnceLock::new();
    *FACTOR.get_or_init(|| {
        std::env::var("WGR_GEO_POOL_GROWTH")
            .ok()
            .and_then(|v| v.parse::<f64>().ok())
            .map(|v| v.clamp(1.25, 2.0))
            .unwrap_or(2.0)
    })
}

// The capacity to grow to when `n` more units are needed and `cap` are allocated. Always at
// least `cap + n`, so the caller's pending allocation is guaranteed to fit in the fresh tail.
fn grown_cap(cap: u32, n: u32) -> u32 {
    let scaled = (f64::from(cap) * growth_factor()).ceil();
    let scaled = if scaled >= f64::from(u32::MAX) {
        u32::MAX
    } else {
        scaled as u32
    };
    // saturating_add, not `cap + n`: both are u32 unit counts and a debug build panics on the
    // overflow rather than reporting an allocation that cannot be served.
    scaled.max(cap.saturating_add(n))
}

// Keep ordinary growth unchanged unless its old+new overlap crosses the soft
// allocation budget. Required geometry still fits; pressure only reduces slack.
fn pressure_cap(cap: u32, n: u32, ordinary: u32, unit_bytes: u64,
                allocated: u64, budget: u64) -> u32 {
    if budget == 0 || allocated == 0 ||
        allocated.saturating_add(u64::from(ordinary) * unit_bytes) <= budget {
        return ordinary;
    }
    let smaller = (u64::from(cap) * 3).div_ceil(2).min(u64::from(u32::MAX)) as u32;
    ordinary.min(smaller.max(cap.saturating_add(n)))
}

// A first-fit free-list allocator over a linear space of `cap` fixed-size units
// (vertices for the vbuf, indices for the ibuf). Freed ranges coalesce with adjacent
// free blocks so fragmentation doesn't accumulate across map changes.
struct RangeAllocator {
    cap: u32,
    // Disjoint free blocks (offset, len), kept sorted by offset so free() coalesces
    // with both neighbours in one pass.
    free: Vec<(u32, u32)>,
}

impl RangeAllocator {
    fn new(cap: u32) -> Self {
        Self {
            cap,
            free: if cap > 0 { vec![(0, cap)] } else { Vec::new() },
        }
    }

    // Reserve `n` units; returns the base offset, or None if no single free block fits
    // (the caller then grows and retries).
    fn alloc(&mut self, n: u32) -> Option<u32> {
        if n == 0 {
            return None;
        }
        for i in 0..self.free.len() {
            let (off, len) = self.free[i];
            if len >= n {
                if len == n {
                    self.free.remove(i);
                } else {
                    self.free[i] = (off + n, len - n);
                }
                return Some(off);
            }
        }
        None
    }

    // Return `[off, off + n)` to the free set, coalescing with the block before and/or
    // after it.
    fn free(&mut self, off: u32, n: u32) {
        if n == 0 {
            return;
        }
        let pos = self.free.partition_point(|&(o, _)| o < off);
        self.free.insert(pos, (off, n));
        let mut i = pos;
        // Merge with the previous block if it ends exactly where this one starts.
        if i > 0 {
            let (po, pl) = self.free[i - 1];
            if po + pl == self.free[i].0 {
                self.free[i - 1].1 += self.free[i].1;
                self.free.remove(i);
                i -= 1;
            }
        }
        // Merge with the next block if this one ends exactly where it starts.
        if i + 1 < self.free.len() {
            let (o, l) = self.free[i];
            if o + l == self.free[i + 1].0 {
                self.free[i].1 += self.free[i + 1].1;
                self.free.remove(i + 1);
            }
        }
    }

    // Extend capacity to `new_cap`, adding the appended tail as a free block. The caller
    // guarantees `new_cap - cap >= n` for the pending alloc, so the tail alone satisfies
    // it (and coalesces with any free block ending at the old cap).
    fn grow(&mut self, new_cap: u32) {
        if new_cap <= self.cap {
            return;
        }
        let old = self.cap;
        self.cap = new_cap;
        self.free(old, new_cap - old);
    }

    fn used(&self) -> u32 {
        let free: u64 = self.free.iter().map(|&(_, len)| len as u64).sum();
        (self.cap as u64).saturating_sub(free) as u32
    }

    // The free block that runs to the end of the space, if there is one: (offset, len).
    // `offset` is the LOWEST capacity this allocator can be trimmed to without moving a
    // live range, which is the whole reason a shrink is expressible here at all (see
    // GeometryPool::shrink_target).
    fn trailing_free(&self) -> Option<(u32, u32)> {
        self.free
            .last()
            .copied()
            .filter(|&(off, len)| off.saturating_add(len) == self.cap)
    }

    // All live spans end at or before this prefix. Interior holes still belong to
    // the copy: used() is a SUM and cannot locate the last live allocation.
    // A trailing free block is sorted/coalesced and touches cap; its start is the
    // exclusive live high-water bound. No scan, remap or new allocator state.
    fn occupied_prefix_units(&self) -> u32 {
        self.trailing_free().map_or(self.cap, |(off, _)| off)
    }

    // Reduce capacity to `new_cap`. Only legal when `new_cap` is at or above the start of
    // the trailing free block — i.e. nothing live sits at or above `new_cap`. Returns false
    // and changes nothing otherwise, so a caller that got the arithmetic wrong loses the
    // saving rather than corrupting an allocation.
    fn shrink(&mut self, new_cap: u32) -> bool {
        if new_cap >= self.cap {
            return false;
        }
        let Some((off, _len)) = self.trailing_free() else {
            return false;
        };
        if new_cap < off {
            return false;
        }
        if new_cap == off {
            self.free.pop();
        } else {
            let last = self.free.len() - 1;
            self.free[last] = (off, new_cap - off);
        }
        self.cap = new_cap;
        true
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct GeometryResidency {
    // Bytes suballocated to live meshes. Only `free()` (i.e. mesh_destroy) returns bytes here.
    pub live_bytes: u64,
    // Bytes the two backing buffers occupy. NOT monotonic since REN-RES-002: the pool grows
    // by doubling and gives capacity back through `maybe_shrink()` once live has stayed well
    // under it for `SHRINK_FRAMES` consecutive frames. `capacity - live` is therefore
    // transient slack, not permanent.
    pub capacity_bytes: u64,
    // NOT an eviction counter, despite reading like one where it surfaces as "geometryRetired".
    // It is the transient set of buffers superseded by a growth *this frame*, held only until
    // `frame_submitted()` (which the renderer calls immediately after every queue submit). It is
    // therefore ZERO at every sampling point that is not inside a growing frame — which is every
    // steady-state sample. `retired = 0` means "the pool did not grow in the sampled frame", and
    // says nothing whatsoever about whether geometry is being released. There is no geometry
    // eviction path in this module; the only way bytes leave the pool is the engine destroying
    // a mesh.
    pub retired_bytes: u64,
}

// Where one mesh's geometry landed in the pool.
pub struct MeshAlloc {
    pub vbase: u32,
    pub ibase: u32,
}

pub struct GeometryPool {
    vbuf: wgpu::Buffer,
    ibuf: wgpu::Buffer,
    valloc: RangeAllocator, // over vertices
    ialloc: RangeAllocator, // over indices
    // Buffers replaced by a growth, kept alive until the next frame submit.
    //
    // Every draw site fetches vbuf()/ibuf() live, so nothing caches a stale
    // handle -- but a growth can land *between* a pass recording
    // set_vertex_buffer and the queue submit that consumes it. Dropping the old
    // buffer there destroys it while a recorded command still references it, and
    // wgpu rejects the submit with "Buffer with 'wgr_geo_pool_vbuf' label is
    // invalid". OFP never hit this because its geometry fits the initial
    // capacity; an Arma 3 world streams meshes in while frames are rendering and
    // grows the pool repeatedly.
    //
    // Queue submission takes ownership of every resource referenced by its command
    // buffers. Once the renderer has submitted the frame, these application-side
    // handles can be dropped; wgpu keeps the underlying allocations alive for any
    // GPU work still in flight. Keeping a fixed number of *growth generations*
    // instead retained hundreds of MiB indefinitely on dense A3/DayZ worlds.
    retired: Vec<wgpu::Buffer>,
    // Submission drops handles, but allocation overlap lasts until GPU completion.
    submitted_retired_bytes: std::sync::Arc<std::sync::atomic::AtomicU64>,
    // Bumped whenever vbuf or ibuf is reallocated by a growth OR a shrink. Cached bind
    // groups / pass state referencing a pool buffer compare against this to know when to
    // rebuild.
    epoch: u64,
    // Consecutive frames in which at least one allocator has been at or below
    // SHRINK_LOW_WATER of its capacity. THE HYSTERESIS: a shrink needs SHRINK_FRAMES of
    // them in a row, so one quiet frame between two loads cannot trigger a full-pool
    // realloc + GPU copy. Reset to 0 by any frame that is not under the mark, by any
    // growth, and by a shrink that actually fired.
    shrink_streak: u32,
    pressure_budget: u64,
}

impl GeometryPool {
    // Initial capacities (grown by doubling on demand). OFP's whole resident geometry
    // working set is a modest budget; these keep load-time growth to a few copies.
    const INIT_VERTS: u32 = 1 << 18; // 256K verts (~9 MB)
    const INIT_INDICES: u32 = 1 << 19; // 512K indices (2 MB)

    // REN-RES-002 — shrink policy. Measured motivation (roadmap Phase 7, 2026-08-31,
    // Reforger Everon traverse): geometry live 682 MB against 1,344 MB of pool capacity —
    // 661 MB allocated and never used, because growth doubles and there was no shrink path
    // at all. At a 2 GB residency budget that slack alone is a third of the budget.
    //
    // WHY THIS SHAPE AND NOT COMPACTION. A live range's offset is baked into the engine's
    // `Mesh.alloc` (and, through it, into the cull tables and every recorded draw) the
    // moment `alloc()` returns; the pool holds no back-reference to the owners, so it
    // cannot move a range and tell anyone. Real compaction would need a mesh-handle
    // rewrite pass across gfx3d — a much larger change. What IS available for free is the
    // TAIL: if the free list ends in a block that runs to `cap`, every live range lies
    // below that block's offset, so capacity can be trimmed to it with every live range
    // keeping its exact offset and every baked base staying correct. That is the "cheaper
    // variant" — it recovers the doubling slack (which is precisely a trailing free block)
    // and recovers nothing from interior fragmentation.
    //
    // THE THREE CONSTANTS, and why ping-pong is impossible:
    //  - a shrink needs used <= 0.5 * cap for SHRINK_FRAMES CONSECUTIVE frames;
    //  - it targets 1.25 * used, so immediately after a shrink used/cap >= 0.8, which is
    //    far above the 0.5 mark — the streak cannot re-arm without something being freed;
    //  - it only fires when the new capacity at most HALVES the buffer, so a marginal
    //    saving never pays for a full-pool GPU copy;
    //  - any growth resets the streak to 0.
    // Together: shrink -> grow -> shrink oscillation would require crossing 0.5 and then
    // 0.8 within the same working set, which the 1.25 headroom rules out.

    // Live must be at or under this fraction of capacity to arm the streak.
    const SHRINK_LOW_WATER: f64 = 0.5;
    // Consecutive frames under the mark before a shrink fires. ~2 s at 60 fps: long enough
    // that a map load's quiet gaps do not trigger a realloc, short enough that a level
    // transition's slack is handed back well inside a session.
    const SHRINK_FRAMES: u32 = 120;
    // Capacity to aim for, as a multiple of live. Above 1.0 so the next few allocations do
    // not immediately grow again; this is the anti-ping-pong term.
    const SHRINK_HEADROOM: f64 = 1.25;

    pub fn new(device: &wgpu::Device) -> Self {
        Self {
            vbuf: Self::make_vbuf(device, Self::INIT_VERTS),
            ibuf: Self::make_ibuf(device, Self::INIT_INDICES),
            valloc: RangeAllocator::new(Self::INIT_VERTS),
            ialloc: RangeAllocator::new(Self::INIT_INDICES),
            epoch: 0,
            retired: Vec::new(),
            submitted_retired_bytes: std::sync::Arc::new(std::sync::atomic::AtomicU64::new(0)),
            shrink_streak: 0,
            pressure_budget: 0,
        }
    }

    // STORAGE on both: the vbuf is the compute skin-bake's rest-pose source, and a
    // later cull/indirect compute reads the ibuf; VERTEX/INDEX for the draw; COPY_* for
    // uploads and the growth copy.
    fn make_vbuf(device: &wgpu::Device, verts: u32) -> wgpu::Buffer {
        // RFG-076: say how big the pool buffer is every time it is (re)made. The retained
        // path had no number for this at all: a native Reforger world took the pool past
        // what the device would allocate, `create_buffer` came back invalid, and the only
        // evidence was an empty landscape and two validation errors naming the label.
        let bytes = verts as u64 * VERT_SIZE;
        GEO_POOL_VBUF_BYTES.store(bytes, std::sync::atomic::Ordering::Relaxed);
        pool_event(&format!("make vbuf {:.1}MB", bytes as f64 / 1048576.0));
        device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_geo_pool_vbuf"),
            size: verts as u64 * VERT_SIZE,
            usage: wgpu::BufferUsages::VERTEX
                | wgpu::BufferUsages::STORAGE
                | wgpu::BufferUsages::COPY_DST
                | wgpu::BufferUsages::COPY_SRC,
            mapped_at_creation: false,
        })
    }

    fn make_ibuf(device: &wgpu::Device, indices: u32) -> wgpu::Buffer {
        // RFG-076: say how big the pool buffer is every time it is (re)made. The retained
        // path had no number for this at all: a native Reforger world took the pool past
        // what the device would allocate, `create_buffer` came back invalid, and the only
        // evidence was an empty landscape and two validation errors naming the label.
        let bytes = indices as u64 * INDEX_SIZE;
        GEO_POOL_IBUF_BYTES.store(bytes, std::sync::atomic::Ordering::Relaxed);
        pool_event(&format!("make ibuf {:.1}MB", bytes as f64 / 1048576.0));
        device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("wgr_geo_pool_ibuf"),
            size: indices as u64 * INDEX_SIZE,
            usage: wgpu::BufferUsages::INDEX
                | wgpu::BufferUsages::STORAGE
                | wgpu::BufferUsages::COPY_DST
                | wgpu::BufferUsages::COPY_SRC,
            mapped_at_creation: false,
        })
    }

    fn retire(&mut self, old: wgpu::Buffer) {
        pool_event(&format!("retire {:.1}MB", old.size() as f64 / 1048576.0));
        self.retired.push(old);
    }

    // Called immediately after the renderer's frame command buffer has been
    // submitted. This is the precise lifetime boundary required by retire().
    //
    // `device`/`queue` are here so the shrink check can run at exactly this boundary and
    // nowhere else: `retired` is cleared FIRST (releasing last frame's superseded
    // handles), then a shrink may push this frame's superseded buffer into the now-empty
    // list, where it is held until the NEXT submit. That is the same lifetime a growth
    // gets, so a shrink can never free a buffer the GPU is still reading.
    //
    // Returns true if a shrink actually reallocated (the caller must then drop any cache
    // holding a pool buffer, exactly as it does for a growth).
    pub fn frame_submitted(&mut self, device: &wgpu::Device, queue: &wgpu::Queue) -> bool {
        if !self.retired.is_empty() {
            pool_event(&format!("clear {} retired", self.retired.len()));
        }
        let bytes: u64 = self.retired.iter().map(wgpu::Buffer::size).sum();
        if bytes != 0 {
            use std::sync::atomic::Ordering::Relaxed;
            self.submitted_retired_bytes.fetch_add(bytes, Relaxed);
            let pending = self.submitted_retired_bytes.clone();
            queue.on_submitted_work_done(move || { pending.fetch_sub(bytes, Relaxed); });
        }
        self.retired.clear();
        self.maybe_shrink(device, queue)
    }

    // The capacity `alloc` should be trimmed to, or None if a shrink is not warranted.
    // `init` is the allocator's initial capacity, a floor: dropping below it would only
    // buy a few MB and reintroduce load-time growth copies on the next map.
    fn shrink_target(alloc: &RangeAllocator, init: u32) -> Option<u32> {
        let used = alloc.used();
        if f64::from(used) > Self::SHRINK_LOW_WATER * f64::from(alloc.cap) {
            return None;
        }
        // Everything live is below `toff`; without a trailing free block there is nothing
        // to give back regardless of how much is free in the interior.
        let (toff, _) = alloc.trailing_free()?;
        let want = (f64::from(used) * Self::SHRINK_HEADROOM).ceil();
        let want = if want >= f64::from(u32::MAX) {
            u32::MAX
        } else {
            want as u32
        };
        let new_cap = want.max(toff).max(init);
        // Only worth a full-buffer realloc plus a GPU copy if it at least halves the
        // buffer. `>` not `>=` so an exact halving still qualifies.
        if new_cap > alloc.cap / 2 {
            return None;
        }
        Some(new_cap)
    }

    // Whether either allocator is currently under the low-water mark. Split out so the
    // streak logic reads as one statement and can be reasoned about in tests.
    fn under_low_water(&self) -> bool {
        let under = |a: &RangeAllocator| {
            a.cap > 0 && f64::from(a.used()) <= Self::SHRINK_LOW_WATER * f64::from(a.cap)
        };
        under(&self.valloc) || under(&self.ialloc)
    }

    fn maybe_shrink(&mut self, device: &wgpu::Device, queue: &wgpu::Queue) -> bool {
        if !self.under_low_water() {
            self.shrink_streak = 0;
            return false;
        }
        self.shrink_streak += 1;
        if self.shrink_streak < Self::SHRINK_FRAMES {
            return false;
        }
        // Reset unconditionally: whether or not a buffer actually qualifies this frame,
        // the streak has been spent. Otherwise a pool that is under the mark but has no
        // trailing free block would re-attempt (and re-fail) the arithmetic every frame.
        self.shrink_streak = 0;

        let mut shrank = false;
        if let Some(new_cap) = Self::shrink_target(&self.valloc, Self::INIT_VERTS) {
            let new_buf = Self::make_vbuf(device, new_cap);
            Self::copy_prefix(
                device,
                queue,
                &self.vbuf,
                &new_buf,
                u64::from(self.valloc.occupied_prefix_units()) * VERT_SIZE,
                "v",
            );
            let old = std::mem::replace(&mut self.vbuf, new_buf);
            self.retire(old);
            // Only after the copy has been issued: shrink() is the point of no return for
            // the free list, and it must describe the buffer that is now installed.
            self.valloc.shrink(new_cap);
            shrank = true;
        }
        if let Some(new_cap) = Self::shrink_target(&self.ialloc, Self::INIT_INDICES) {
            let new_buf = Self::make_ibuf(device, new_cap);
            Self::copy_prefix(
                device,
                queue,
                &self.ibuf,
                &new_buf,
                u64::from(self.ialloc.occupied_prefix_units()) * INDEX_SIZE,
                "i",
            );
            let old = std::mem::replace(&mut self.ibuf, new_buf);
            self.retire(old);
            self.ialloc.shrink(new_cap);
            shrank = true;
        }
        if shrank {
            self.epoch += 1;
        }
        shrank
    }

    pub fn vbuf(&self) -> &wgpu::Buffer {
        &self.vbuf
    }

    pub fn ibuf(&self) -> &wgpu::Buffer {
        &self.ibuf
    }

    pub fn generation(&self) -> u64 {
        self.epoch
    }

    pub fn residency(&self) -> GeometryResidency {
        GeometryResidency {
            live_bytes: self.valloc.used() as u64 * VERT_SIZE
                + self.ialloc.used() as u64 * INDEX_SIZE,
            capacity_bytes: self.valloc.cap as u64 * VERT_SIZE
                + self.ialloc.cap as u64 * INDEX_SIZE,
            retired_bytes: self.retired.iter().map(wgpu::Buffer::size).sum::<u64>()
                + self.submitted_retired_bytes.load(std::sync::atomic::Ordering::Relaxed),
        }
    }

    // Suballocate one mesh and upload it. Indices are Uint32 end to end (0-based and
    // mesh-local). They used to arrive as u16 and be widened here, which capped a LOD
    // at 65535 vertices for no reason the GPU cared about -- the pool buffer has always
    // been Uint32. Grows the backing buffer(s) if a load exhausts them. Returns None for
    // an empty mesh (mesh_create's 0-handle case).
    pub fn alloc(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        verts: &[WgrMeshVertex],
        indices: &[u32],
    ) -> Option<MeshAlloc> {
        if verts.is_empty() || indices.is_empty() {
            return None;
        }
        let vbase = self.alloc_verts(device, queue, verts.len() as u32);
        let ibase = self.alloc_indices(device, queue, indices.len() as u32);
        // RFG-079: a write past the end of either buffer is reported here, once, with the
        // numbers. `wgr_geo_pool_ibuf` went INVALID at a write with the pool at 2 MB and no
        // growth or retire in between -- so the write itself is the suspect.
        {
            let vend = vbase as u64 * VERT_SIZE + verts.len() as u64 * VERT_SIZE;
            let iend = ibase as u64 * INDEX_SIZE + indices.len() as u64 * INDEX_SIZE;
            if vend > self.vbuf.size() || iend > self.ibuf.size() {
                pool_event(&format!(
                    "OVERRUN write v[{}+{}]/{} i[{}+{}]/{}",
                    vbase, verts.len(), self.vbuf.size() / VERT_SIZE, ibase, indices.len(), self.ibuf.size() / INDEX_SIZE
                ));
            }
        }
        queue.write_buffer(
            &self.vbuf,
            vbase as u64 * VERT_SIZE,
            bytemuck::cast_slice(verts),
        );
        queue.write_buffer(
            &self.ibuf,
            ibase as u64 * INDEX_SIZE,
            bytemuck::cast_slice(indices),
        );
        Some(MeshAlloc { vbase, ibase })
    }

    // Rewrite an existing mesh's vertices in place (dynamic/dirty re-upload). Topology
    // (indices) is unchanged.
    pub fn update_verts(&self, queue: &wgpu::Queue, vbase: u32, verts: &[WgrMeshVertex]) {
        queue.write_buffer(
            &self.vbuf,
            vbase as u64 * VERT_SIZE,
            bytemuck::cast_slice(verts),
        );
    }

    pub fn free(&mut self, alloc: &MeshAlloc, vcount: u32, icount: u32) {
        self.valloc.free(alloc.vbase, vcount);
        self.ialloc.free(alloc.ibase, icount);
    }

    pub fn set_pressure_budget(&mut self, bytes: u64) {
        // Explicit historical factor overrides remain exact for reproducible A/B.
        let enabled = std::env::var("WGR_GEO_POOL_PRESSURE_GROWTH").is_ok_and(|v| v == "1");
        if enabled && std::env::var_os("WGR_GEO_POOL_GROWTH").is_none() {
            self.pressure_budget = bytes;
        }
    }

    fn growth_capacity(&self, device: &wgpu::Device, cap: u32, n: u32, unit: u64) -> u32 {
        let ordinary = grown_cap(cap, n);
        if self.pressure_budget == 0 { return ordinary; }
        let counters = device.get_internal_counters().hal;
        let allocated = (counters.buffer_memory.read().max(0) as u64)
            .saturating_add(counters.texture_memory.read().max(0) as u64);
        let result = pressure_cap(cap, n, ordinary, unit, allocated, self.pressure_budget);
        if result != ordinary {
            pool_event(&format!("pressure growth {} -> {} instead of {} bytes; allocated {} budget {}",
                u64::from(cap) * unit, u64::from(result) * unit, u64::from(ordinary) * unit,
                allocated, self.pressure_budget));
        }
        result
    }

    fn alloc_verts(&mut self, device: &wgpu::Device, queue: &wgpu::Queue, n: u32) -> u32 {
        if let Some(o) = self.valloc.alloc(n) {
            return o;
        }
        let cap = self.valloc.cap;
        let new_cap = self.growth_capacity(device, cap, n, VERT_SIZE);
        let new_buf = Self::make_vbuf(device, new_cap);
        Self::copy_prefix(
            device,
            queue,
            &self.vbuf,
            &new_buf,
            u64::from(self.valloc.occupied_prefix_units()) * VERT_SIZE,
            "v",
        );
        let old_v = std::mem::replace(&mut self.vbuf, new_buf);
        self.retire(old_v);
        self.valloc.grow(new_cap);
        self.epoch += 1;
        // A growth is proof the working set is not shrinking; the hysteresis restarts.
        self.shrink_streak = 0;
        self.valloc.alloc(n).expect("post-grow vertex alloc")
    }

    fn alloc_indices(&mut self, device: &wgpu::Device, queue: &wgpu::Queue, n: u32) -> u32 {
        if let Some(o) = self.ialloc.alloc(n) {
            return o;
        }
        let cap = self.ialloc.cap;
        let new_cap = self.growth_capacity(device, cap, n, INDEX_SIZE);
        let new_buf = Self::make_ibuf(device, new_cap);
        Self::copy_prefix(
            device,
            queue,
            &self.ibuf,
            &new_buf,
            u64::from(self.ialloc.occupied_prefix_units()) * INDEX_SIZE,
            "i",
        );
        let old_i = std::mem::replace(&mut self.ibuf, new_buf);
        self.retire(old_i);
        self.ialloc.grow(new_cap);
        self.epoch += 1;
        // A growth is proof the working set is not shrinking; the hysteresis restarts.
        self.shrink_streak = 0;
        self.ialloc.alloc(n).expect("post-grow index alloc")
    }

    // Copy through the last possible live allocation, preserving offsets and all
    // interior gaps. Omit only the unallocated trailing bytes: later allocations
    // write their entire spans, and free-range contents have no consumer contract.
    // u32 units * fixed u64 element size cannot overflow; both element sizes are
    // multiples of COPY_BUFFER_ALIGNMENT. Capacity/growth/shrink policy is unchanged.
    fn copy_prefix(
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        old: &wgpu::Buffer,
        new: &wgpu::Buffer,
        bytes: u64,
        tag: &str,
    ) {
        const { assert!(VERT_SIZE % wgpu::COPY_BUFFER_ALIGNMENT == 0); }
        const { assert!(INDEX_SIZE % wgpu::COPY_BUFFER_ALIGNMENT == 0); }
        debug_assert!(bytes <= old.size() && bytes <= new.size());
        debug_assert_eq!(bytes % wgpu::COPY_BUFFER_ALIGNMENT, 0);
        note_prefix_copy(tag, old.size().min(new.size()), bytes);
        if bytes == 0 {
            return; // No live span: no empty encoder/submit or invalid zero-sized copy.
        }
        let mut enc = device.create_command_encoder(&wgpu::CommandEncoderDescriptor {
            label: Some(if tag == "v" {
                "wgr_geo_pool_grow_v"
            } else {
                "wgr_geo_pool_grow_i"
            }),
        });
        enc.copy_buffer_to_buffer(old, 0, new, 0, bytes);
        queue.submit(std::iter::once(enc.finish()));
    }
}

// Optional bounded reallocation-only evidence, not a frame timing, memory budget
// or route-total guarantee. Counters/log formatting are absent unless exact1.
// The cumulative prefix ends after32 notices; the cap marker is explicit.
fn note_prefix_copy(tag: &str, nominal: u64, copied: u64) {
    use std::sync::{atomic::{AtomicU64, Ordering::Relaxed}, OnceLock};
    static ENABLED: OnceLock<bool> = OnceLock::new();
    if !*ENABLED.get_or_init(|| std::env::var("WGR_GEO_POOL_COPY_DIAGNOSTICS").is_ok_and(|v| v == "1")) {
        return;
    }
    static NOTICES: AtomicU64 = AtomicU64::new(0);
    static NOMINAL: AtomicU64 = AtomicU64::new(0);
    static COPIED: AtomicU64 = AtomicU64::new(0);
    let notice = NOTICES.fetch_add(1, Relaxed);
    if notice < 32 {
        // At most32 bounded u32-span buffer sizes: these sums fit u64.
        let nominal_total = NOMINAL.fetch_add(nominal, Relaxed) + nominal;
        let copied_total = COPIED.fetch_add(copied, Relaxed) + copied;
        pool_event(&format!("occupied-prefix copy{} kind{} nominal{} copied{} cumulativeNominal{} cumulativeCopied{} scope=process-prefix",
            notice + 1, tag, nominal, copied, nominal_total, copied_total));
    } else if notice == 32 {
        pool_event("occupied-prefix copy diagnostic truncated after32; not full-route totals");
    }
}

#[cfg(test)]
mod tests {
    use super::{GeometryPool, RangeAllocator, VERT_SIZE, grown_cap};
    use crate::ffi::WgrMeshVertex;
    use bytemuck::Zeroable;

    // A mesh whose first vertex carries `tag` in pos.x, so a readback can prove THIS
    // allocation's bytes are the ones still sitting at its base after a shrink.
    fn tagged_mesh(n: u32, tag: f32) -> (Vec<WgrMeshVertex>, Vec<u32>) {
        let mut verts = vec![WgrMeshVertex::zeroed(); n as usize];
        for (i, v) in verts.iter_mut().enumerate() {
            v.pos.x = tag + i as f32;
        }
        (verts, (0..n).collect())
    }

    fn read_first_vertex_x(
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        pool: &GeometryPool,
        vbase: u32,
        count: u32,
    ) -> Vec<f32> {
        let bytes = count as u64 * VERT_SIZE;
        let staging = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("pool_readback"),
            size: bytes,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        let mut enc =
            device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        enc.copy_buffer_to_buffer(pool.vbuf(), vbase as u64 * VERT_SIZE, &staging, 0, bytes);
        queue.submit(std::iter::once(enc.finish()));
        let slice = staging.slice(..);
        let (tx, rx) = std::sync::mpsc::channel();
        slice.map_async(wgpu::MapMode::Read, move |r| {
            let _ = tx.send(r);
        });
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
        rx.recv().unwrap().unwrap();
        let data = slice.get_mapped_range();
        let verts: &[WgrMeshVertex] = bytemuck::cast_slice(&data);
        verts.iter().map(|v| v.pos.x).collect()
    }

    #[test]
    fn occupied_prefix_includes_holes_and_handles_empty_full_and_u32_end() {
        let mut ranges = RangeAllocator::new(128);
        assert_eq!(ranges.occupied_prefix_units(), 0);
        let a = ranges.alloc(13).unwrap();
        let hole = ranges.alloc(17).unwrap();
        let b = ranges.alloc(29).unwrap();
        assert_eq!(b, 30);
        ranges.free(hole, 17);
        assert_eq!(ranges.used(), 42);
        assert_eq!(ranges.occupied_prefix_units(), 59); // NOT42: B crosses that sum.
        assert!(ranges.shrink(80)); // Free headroom still omitted from copy.
        assert_eq!(ranges.occupied_prefix_units(), 59);
        ranges.free(b, 29);
        assert_eq!(ranges.occupied_prefix_units(), 13);
        ranges.free(a, 13);
        assert_eq!(ranges.occupied_prefix_units(), 0);
        let mut full = RangeAllocator::new(u32::MAX);
        assert_eq!(full.alloc(u32::MAX), Some(0));
        assert_eq!(full.occupied_prefix_units(), u32::MAX);
        assert_eq!(u64::from(full.occupied_prefix_units()) * VERT_SIZE,
                   u64::from(u32::MAX) * std::mem::size_of::<WgrMeshVertex>() as u64);
        full.free(u32::MAX - 7, 7);
        assert_eq!(full.occupied_prefix_units(), u32::MAX - 7);
        assert_eq!(RangeAllocator::new(0).occupied_prefix_units(), 0);
    }

    fn read_pool_bytes(device: &wgpu::Device, queue: &wgpu::Queue,
                       buffer: &wgpu::Buffer, offset: u64, size: u64) -> Vec<u8> {
        let staging = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("occupied_prefix_readback"), size,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor {label: None});
        encoder.copy_buffer_to_buffer(buffer, offset, &staging, 0, size);
        queue.submit(std::iter::once(encoder.finish()));
        let slice = staging.slice(..);let (tx, rx) = std::sync::mpsc::channel();
        slice.map_async(wgpu::MapMode::Read, move |result| { let _ = tx.send(result); });
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
        rx.recv().unwrap().unwrap();
        let bytes = slice.get_mapped_range().to_vec();staging.unmap();bytes
    }

    #[test]
    fn occupied_prefix_growth_and_shrink_preserve_separated_gpu_vertex_and_index_spans() {
        let Some((device, queue)) = crate::gfx3d::cull::tests::headless() else {
            eprintln!("SKIP occupied-prefix GPU readback: no headless adapter/device");return;
        };
        let error_scope = device.push_error_scope(wgpu::ErrorFilter::Validation);
        let mut pool = GeometryPool::new(&device);
        let (av, ai) = tagged_mesh(19, 1024.0);
        let a = pool.alloc(&device, &queue, &av, &ai).unwrap();
        let (hv, hi) = tagged_mesh(37, 2048.0);
        let hole = pool.alloc(&device, &queue, &hv, &hi).unwrap();
        let (bv, mut bi) = tagged_mesh(29, 4096.0);bi.reverse();
        let b = pool.alloc(&device, &queue, &bv, &bi).unwrap();
        let (tv, ti) = tagged_mesh(41, 8192.0);
        let tail = pool.alloc(&device, &queue, &tv, &ti).unwrap();
        pool.free(&hole, 37, 37);pool.free(&tail, 41, 41);
        assert_eq!(pool.valloc.occupied_prefix_units(), b.vbase + 29);
        assert_eq!(pool.ialloc.occupied_prefix_units(), b.ibase + 29);
        assert!(pool.valloc.occupied_prefix_units() > pool.valloc.used());
        let verify = |pool: &GeometryPool| {
            for (allocation, vertices, indices) in [(&a, &av, &ai), (&b, &bv, &bi)] {
                assert_eq!(read_pool_bytes(&device, &queue, pool.vbuf(),
                    u64::from(allocation.vbase) * VERT_SIZE, vertices.len() as u64 * VERT_SIZE),
                    bytemuck::cast_slice::<_, u8>(vertices.as_slice()));
                assert_eq!(read_pool_bytes(&device, &queue, pool.ibuf(),
                    u64::from(allocation.ibase) * super::INDEX_SIZE, indices.len() as u64 * super::INDEX_SIZE),
                    bytemuck::cast_slice::<_, u8>(indices.as_slice()));
            }
        };
        verify(&pool);
        let initial_caps = (pool.valloc.cap, pool.ialloc.cap);
        let (large_v, large_i) = tagged_mesh(600_000, 16384.0);
        let large = pool.alloc(&device, &queue, &large_v, &large_i).unwrap();
        assert!(pool.valloc.cap > initial_caps.0 && pool.ialloc.cap > initial_caps.1);
        assert_eq!(pool.epoch, 2);verify(&pool); // Both real growth copies preserved A/B.
        pool.free(&large, 600_000, 600_000);
        pool.shrink_streak = GeometryPool::SHRINK_FRAMES - 1;
        assert!(pool.frame_submitted(&device, &queue));
        assert_eq!((pool.valloc.cap, pool.ialloc.cap), initial_caps);
        verify(&pool); // Both real shrink copies preserved holes/offsets/full bytes.
        let refill = pool.alloc(&device, &queue, &hv, &hi).unwrap();
        assert_eq!((refill.vbase, refill.ibase), (hole.vbase, hole.ibase));
        assert_eq!(read_pool_bytes(&device, &queue, pool.vbuf(),
            u64::from(refill.vbase) * VERT_SIZE, hv.len() as u64 * VERT_SIZE),
            bytemuck::cast_slice::<_, u8>(hv.as_slice()));
        assert_eq!(read_pool_bytes(&device, &queue, pool.ibuf(),
            u64::from(refill.ibase) * super::INDEX_SIZE, hi.len() as u64 * super::INDEX_SIZE),
            bytemuck::cast_slice::<_, u8>(hi.as_slice()));
        verify(&pool); // New full-span upload does not corrupt either adjacent survivor.
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
        assert!(pollster::block_on(error_scope.pop()).is_none());
    }

    // REN-RES-002 — the central hysteresis property, on a real device.
    //
    // Measured motivation: 682 MB live against 1,344 MB of capacity, because the pool
    // doubled and never gave anything back. This drives the sequence that produces exactly
    // that shape (grow, then free the big allocation) and asserts three things at once:
    // SHRINK_FRAMES - 1 quiet frames do nothing, the SHRINK_FRAMES'th reallocates down, and
    // the allocation that survived still reads back byte-for-byte at its original base.
    #[test]
    fn shrink_fires_only_on_the_full_streak_and_preserves_live_ranges() {
        let Some((device, queue)) = crate::gfx3d::cull::tests::headless() else {
            return;
        };
        let mut pool = GeometryPool::new(&device);

        // A small survivor at base 0, then a large one that forces a vertex growth.
        let (sv, si) = tagged_mesh(100, 1000.0);
        let keep = pool.alloc(&device, &queue, &sv, &si).expect("survivor");
        assert_eq!(keep.vbase, 0);
        let (bv, bi) = tagged_mesh(400_000, 0.0);
        let big = pool.alloc(&device, &queue, &bv, &bi).expect("big");

        let grown = pool.residency().capacity_bytes;
        assert!(
            grown > GeometryPool::INIT_VERTS as u64 * VERT_SIZE,
            "the setup must actually grow the pool, else the test proves nothing"
        );

        let retired = pool.residency().retired_bytes;
        assert!(retired > 0);
        pool.frame_submitted(&device, &queue);
        // Without polling the device, dropping application handles must not announce
        // this overlap as free. A completion callback, not a frame count, clears it.
        assert_eq!(pool.residency().retired_bytes, retired);
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
        assert_eq!(pool.residency().retired_bytes, 0);
        pool.shrink_streak = 0;

        // Hand the big allocation back: live collapses to 100 vertices against a capacity
        // sized for 400,100 — the doubling slack this change exists to recover.
        pool.free(&big, 400_000, 400_000);
        assert_eq!(
            pool.residency().capacity_bytes,
            grown,
            "freeing alone must not shrink; only the streak may"
        );

        // ONE frame under the mark must not be enough, and neither must N-1.
        for frame in 0..GeometryPool::SHRINK_FRAMES - 1 {
            pool.frame_submitted(&device, &queue);
            assert_eq!(
                pool.residency().capacity_bytes,
                grown,
                "shrank after only {} consecutive quiet frames",
                frame + 1
            );
        }

        assert!(
            pool.frame_submitted(&device, &queue),
            "the {}th consecutive quiet frame must shrink",
            GeometryPool::SHRINK_FRAMES
        );
        let shrunk = pool.residency().capacity_bytes;
        assert!(
            shrunk < grown,
            "capacity did not come down ({grown} -> {shrunk})"
        );

        // The whole point: the survivor keeps its offset AND its bytes. Reading pos.x back
        // out of the new buffer is the only thing that proves the copy was right — the
        // free-list bookkeeping would look identical if the copy had been skipped.
        let xs = read_first_vertex_x(&device, &queue, &pool, keep.vbase, 100);
        for (i, x) in xs.iter().enumerate() {
            assert_eq!(*x, 1000.0 + i as f32, "vertex {i} was corrupted by the shrink");
        }

        // And it must not immediately shrink again or start oscillating. Two independent
        // brakes hold here: the 1.25 headroom, and the INIT floor that this pool has now
        // landed on (a further shrink would not halve the buffer, so it is declined).
        for _ in 0..GeometryPool::SHRINK_FRAMES * 2 {
            pool.frame_submitted(&device, &queue);
        }
        assert!(
            pool.residency().capacity_bytes <= shrunk,
            "capacity grew back on its own"
        );
    }

    // A growth is the signal that the working set is expanding, so it must restart the
    // hysteresis from zero rather than letting a nearly-complete streak fire one frame later.
    #[test]
    fn a_growth_resets_the_shrink_streak() {
        let Some((device, queue)) = crate::gfx3d::cull::tests::headless() else {
            return;
        };
        let mut pool = GeometryPool::new(&device);
        let (sv, si) = tagged_mesh(100, 1.0);
        pool.alloc(&device, &queue, &sv, &si).expect("survivor");
        let (bv, bi) = tagged_mesh(400_000, 0.0);
        let big = pool.alloc(&device, &queue, &bv, &bi).expect("big");
        pool.free(&big, 400_000, 400_000);

        // Get to one frame short of firing.
        for _ in 0..GeometryPool::SHRINK_FRAMES - 1 {
            assert!(!pool.frame_submitted(&device, &queue));
        }

        // Now grow. Without the reset, the very next frame would shrink.
        let (gv, gi) = tagged_mesh(700_000, 0.0);
        let grow = pool.alloc(&device, &queue, &gv, &gi).expect("grow");
        let after_growth = pool.residency().capacity_bytes;
        pool.free(&grow, 700_000, 700_000);
        assert!(
            !pool.frame_submitted(&device, &queue),
            "a growth must reset the streak, not leave it one frame from firing"
        );
        assert_eq!(pool.residency().capacity_bytes, after_growth);

        // A full fresh streak still works.
        for _ in 0..GeometryPool::SHRINK_FRAMES - 1 {
            pool.frame_submitted(&device, &queue);
        }
        assert!(
            pool.residency().capacity_bytes < after_growth,
            "a complete streak after the growth must still shrink"
        );
    }

    // The tail is the only thing that can be given back. If the last live range sits at the
    // top of the space, there is no trailing free block and a shrink must decline rather
    // than trim capacity out from under it — that would be silent memory corruption.
    #[test]
    fn shrink_declines_when_the_tail_is_live() {
        let mut ranges = RangeAllocator::new(1024);
        let low = ranges.alloc(16).unwrap();
        let high = ranges.alloc(1024 - 16).unwrap();
        assert_eq!(high, 16);
        assert!(ranges.trailing_free().is_none(), "nothing free at the top");
        assert_eq!(GeometryPool::shrink_target(&ranges, 0), None);

        // Free the top and it becomes expressible; the low range's offset is untouched.
        ranges.free(high, 1024 - 16);
        let target = GeometryPool::shrink_target(&ranges, 0).expect("tail is now free");
        assert!(
            target >= low + 16,
            "a shrink must never cut into the surviving range"
        );
        assert!(target <= 1024 / 2, "a shrink must at least halve the buffer");
        assert!(ranges.shrink(target));
        assert_eq!(ranges.cap, target);
        assert_eq!(ranges.used(), 16, "the live range survived the trim");
    }

    // The anti-ping-pong argument, stated as a test rather than only as a comment: after a
    // shrink the pool sits at 1/HEADROOM = 80% utilisation, which is far above the 50%
    // low-water mark, so the streak cannot re-arm and a second shrink cannot follow.
    // (`used` starts at 17, not 1: at a handful of units `ceil(used * 1.25)` rounds to a
    // ratio the fraction no longer describes. The real pool cannot get there — the INIT
    // floor keeps its capacity in the hundreds of thousands of units.)
    #[test]
    fn a_shrink_leaves_the_pool_above_its_own_low_water_mark() {
        for used in [17u32, 1000, 65_536, 262_144] {
            let cap = used.saturating_mul(64).max(4096);
            let mut ranges = RangeAllocator::new(cap);
            ranges.alloc(used).unwrap();
            let Some(target) = GeometryPool::shrink_target(&ranges, 0) else {
                continue;
            };
            assert!(ranges.shrink(target));
            assert!(
                f64::from(ranges.used()) > GeometryPool::SHRINK_LOW_WATER * f64::from(ranges.cap),
                "used {} of {} is still under the low-water mark, so it would shrink again",
                ranges.used(),
                ranges.cap
            );
            assert_eq!(GeometryPool::shrink_target(&ranges, 0), None);
        }
    }

    // A pool sitting exactly at its initial capacity has nothing worth reclaiming; the floor
    // stops a fresh, nearly-empty pool from reallocating itself down to nothing and then
    // paying growth copies on the very first map load.
    #[test]
    fn shrink_never_goes_below_the_initial_capacity() {
        let ranges = RangeAllocator::new(GeometryPool::INIT_VERTS);
        assert_eq!(
            GeometryPool::shrink_target(&ranges, GeometryPool::INIT_VERTS),
            None
        );
    }

    // Whatever WGR_GEO_POOL_GROWTH is set to, growth must (a) always serve the allocation that
    // triggered it and (b) never wrap. `cap + n` used to be a plain add: at a capacity near
    // u32::MAX that is an overflow panic in a debug build instead of a saturating refusal.
    #[test]
    fn pressure_growth_counts_overlap_without_losing_required_capacity() {
        // Current allocation includes the old pool; the full new pool overlaps it.
        assert_eq!(super::pressure_cap(100, 1, 200, 4, 1000, 1800), 200);
        assert_eq!(super::pressure_cap(100, 1, 200, 4, 1000, 1799), 150);
        assert_eq!(super::pressure_cap(100, 90, 200, 4, 1000, 1799), 190);
        assert_eq!(super::pressure_cap(100, 1, 200, 4, 0, 100), 200);
        assert_eq!(super::pressure_cap(100, 1, 200, 4, 1000, 0), 200);
        assert_eq!(super::pressure_cap(u32::MAX-1, 4, u32::MAX, 36, u64::MAX, 1), u32::MAX);
    }

    #[test]
    fn growth_always_fits_the_pending_alloc_and_never_overflows() {
        for &(cap, n) in &[
            (0u32, 1u32),
            (1 << 18, 1),
            (1 << 25, 1 << 26), // request far larger than a scaled cap
            (u32::MAX - 4, 8),  // saturates rather than wrapping
            (u32::MAX, 1),
        ] {
            let grown = grown_cap(cap, n);
            assert!(grown >= cap, "growth must not shrink ({cap} -> {grown})");
            assert!(
                grown == u32::MAX || grown - cap >= n,
                "growth {cap} -> {grown} cannot serve {n} units"
            );
        }
    }

    #[test]
    fn used_tracks_alloc_free_and_growth() {
        let mut ranges = RangeAllocator::new(16);
        let a = ranges.alloc(5).unwrap();
        let b = ranges.alloc(3).unwrap();
        assert_eq!(ranges.used(), 8);
        ranges.free(a, 5);
        assert_eq!(ranges.used(), 3);
        ranges.grow(32);
        assert_eq!(ranges.used(), 3);
        ranges.free(b, 3);
        assert_eq!(ranges.used(), 0);
    }
}
