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
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct GeometryResidency {
    // Bytes suballocated to live meshes. Only `free()` (i.e. mesh_destroy) returns bytes here.
    pub live_bytes: u64,
    // Bytes the two backing buffers occupy. Monotonically non-decreasing: the pool grows and
    // has NO shrink path, so `capacity - live` is permanent slack once a peak has been touched.
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
    // Bumped whenever vbuf or ibuf is reallocated by a growth. Cached bind groups /
    // pass state referencing a pool buffer compare against this to know when to rebuild.
    epoch: u64,
}

impl GeometryPool {
    // Initial capacities (grown by doubling on demand). OFP's whole resident geometry
    // working set is a modest budget; these keep load-time growth to a few copies.
    const INIT_VERTS: u32 = 1 << 18; // 256K verts (~9 MB)
    const INIT_INDICES: u32 = 1 << 19; // 512K indices (2 MB)

    pub fn new(device: &wgpu::Device) -> Self {
        Self {
            vbuf: Self::make_vbuf(device, Self::INIT_VERTS),
            ibuf: Self::make_ibuf(device, Self::INIT_INDICES),
            valloc: RangeAllocator::new(Self::INIT_VERTS),
            ialloc: RangeAllocator::new(Self::INIT_INDICES),
            epoch: 0,
            retired: Vec::new(),
        }
    }

    // STORAGE on both: the vbuf is the compute skin-bake's rest-pose source, and a
    // later cull/indirect compute reads the ibuf; VERTEX/INDEX for the draw; COPY_* for
    // uploads and the growth copy.
    fn make_vbuf(device: &wgpu::Device, verts: u32) -> wgpu::Buffer {
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
        self.retired.push(old);
    }

    // Called immediately after the renderer's frame command buffer has been
    // submitted. This is the precise lifetime boundary required by retire().
    pub fn frame_submitted(&mut self) {
        self.retired.clear();
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
            retired_bytes: self.retired.iter().map(wgpu::Buffer::size).sum(),
        }
    }

    // Suballocate one mesh and upload it. Indices are widened u16 -> Uint32 (values
    // unchanged; still 0-based mesh-local). Grows the backing buffer(s) if a load
    // exhausts them. Returns None for an empty mesh (mesh_create's 0-handle case).
    pub fn alloc(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        verts: &[WgrMeshVertex],
        indices: &[u16],
    ) -> Option<MeshAlloc> {
        if verts.is_empty() || indices.is_empty() {
            return None;
        }
        let vbase = self.alloc_verts(device, queue, verts.len() as u32);
        let ibase = self.alloc_indices(device, queue, indices.len() as u32);
        queue.write_buffer(
            &self.vbuf,
            vbase as u64 * VERT_SIZE,
            bytemuck::cast_slice(verts),
        );
        let widened: Vec<u32> = indices.iter().map(|&i| i as u32).collect();
        queue.write_buffer(
            &self.ibuf,
            ibase as u64 * INDEX_SIZE,
            bytemuck::cast_slice(&widened),
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

    fn alloc_verts(&mut self, device: &wgpu::Device, queue: &wgpu::Queue, n: u32) -> u32 {
        if let Some(o) = self.valloc.alloc(n) {
            return o;
        }
        let cap = self.valloc.cap;
        let new_cap = grown_cap(cap, n);
        let new_buf = Self::make_vbuf(device, new_cap);
        Self::copy_prefix(
            device,
            queue,
            &self.vbuf,
            &new_buf,
            cap as u64 * VERT_SIZE,
            "v",
        );
        let old_v = std::mem::replace(&mut self.vbuf, new_buf);
        self.retire(old_v);
        self.valloc.grow(new_cap);
        self.epoch += 1;
        self.valloc.alloc(n).expect("post-grow vertex alloc")
    }

    fn alloc_indices(&mut self, device: &wgpu::Device, queue: &wgpu::Queue, n: u32) -> u32 {
        if let Some(o) = self.ialloc.alloc(n) {
            return o;
        }
        let cap = self.ialloc.cap;
        let new_cap = grown_cap(cap, n);
        let new_buf = Self::make_ibuf(device, new_cap);
        Self::copy_prefix(
            device,
            queue,
            &self.ibuf,
            &new_buf,
            cap as u64 * INDEX_SIZE,
            "i",
        );
        let old_i = std::mem::replace(&mut self.ibuf, new_buf);
        self.retire(old_i);
        self.ialloc.grow(new_cap);
        self.epoch += 1;
        self.ialloc.alloc(n).expect("post-grow index alloc")
    }

    // Copy the used prefix of `old` into `new` (growth preserves every existing
    // allocation at its offset; the appended tail is fresh free space). Load-time only.
    fn copy_prefix(
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        old: &wgpu::Buffer,
        new: &wgpu::Buffer,
        bytes: u64,
        tag: &str,
    ) {
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

#[cfg(test)]
mod tests {
    use super::{RangeAllocator, grown_cap};

    // Whatever WGR_GEO_POOL_GROWTH is set to, growth must (a) always serve the allocation that
    // triggered it and (b) never wrap. `cap + n` used to be a plain add: at a capacity near
    // u32::MAX that is an overflow panic in a debug build instead of a saturating refusal.
    #[test]
    fn growth_always_fits_the_pending_alloc_and_never_overflows() {
        for &(cap, n) in &[
            (0u32, 1u32),
            (1 << 18, 1),
            (1 << 25, 1 << 26),      // request far larger than a scaled cap
            (u32::MAX - 4, 8),       // saturates rather than wrapping
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
