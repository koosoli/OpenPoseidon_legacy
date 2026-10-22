// REN-THR-009 — the upload path that does not need the exclusive renderer handle.
//
// Every other producer-side mutation now goes through the drain (REN-THR-006/007/008):
// the producer records an op, and the consumer applies it at one point per frame. That
// is the right shape for anything structural, and the wrong shape for the CPU-path
// mesh pipeline, because `wgr_mesh_update` is *upload traffic* — queuing it would copy a
// full vertex array every frame for every skinned character.
//
// The audit (REN-THR-009 part 1) says it does not have to be queued, because it does not
// have to be exclusive:
//
//   Gfx3d::mesh_update  (gfx3d/mod.rs)  takes &mut self and never uses it mutably
//   Pool::update_verts  (gfx3d/pool.rs) takes &self and issues one Queue::write_buffer
//
// `wgpu::Queue` is Send + Sync and internally refcounted. So the only thing making an
// update exclusive is the `&mut *renderer` that the FFI entry point forms. This module
// is the second surface that does not form it.
//
// TWO TRAPS, and what this type does about each.
//
// 1. Can an in-place update ever move an allocation? **No.** `MeshAlloc.vbase` is written
//    once by `Pool::alloc` and never rewritten; pool growth copies the entire old capacity
//    forward before installing the new buffer, and pool shrink only ever trims *trailing*
//    free space (`shrink_target` bails without a `trailing_free()` offset, and everything
//    live is below it). There is no compaction path in the pool.
//
//    But the buffer OBJECT is replaced by both events — `std::mem::replace(&mut self.vbuf,
//    new_buf)` — and writing into a retired buffer is the "Buffer with
//    'wgr_geo_pool_vbuf' label is invalid" failure `pool.rs` already documents. So this
//    type may cache `vbase`, and may NOT cache the buffer without a republish rule.
//    `publish_pool` is that rule, called by the renderer at the only two production sites
//    where the pool epoch can move: `Gfx3d::mesh_create` and `Gfx3d::frame_submitted`.
//
// 2. What if a mesh is destroyed while an upload through this type is in flight?
//    Destroy is structural (it removes both `bake_bind_cache` entries, which the draw path
//    reads, and returns ranges to the allocator's free lists), so it cannot happen here at
//    all. `enqueue_destroy` drops the metadata entry synchronously and parks the handle;
//    the renderer runs the real destroy at the top of its own frame. A *subsequent* update
//    on that handle finds no metadata and is ignored — the same null-tolerance
//    `wgr_mesh_update` already documents for unknown handles.
//
//    Update and destroy racing on the SAME handle from two threads is NOT defended against.
//    It is forbidden by the contract in `wgpu_renderer.hpp`: mesh lifetime is producer-owned
//    and single-threaded. Everything is serial today; writing a lock for concurrency that
//    does not exist yet would be untestable and would pre-empt a design the thread slice
//    has not made. This comment is here so the next person knows it is an assumption.

use std::collections::HashMap;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Mutex, RwLock};

use crate::ffi::WgrMeshVertex;

// Must match `VERT_SIZE` in gfx3d/pool.rs — the pool's stride for one WgrMeshVertex.
const VERT_SIZE: u64 = std::mem::size_of::<WgrMeshVertex>() as u64;

/// Everything an upload needs to know about one mesh. Both fields are reads that
/// `Gfx3d::mesh_update` makes today; neither can change for the life of the mesh.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MeshUpload {
    /// First vertex of this mesh in the shared pool buffer. Never moves (trap 1).
    pub vbase: u32,
    /// The mesh's allocated vertex count — the bound `Gfx3d::mesh_update` checks.
    pub vert_count: u32,
}

/// The pool state an upload writes through, and the epoch it belongs to.
struct Published {
    vbuf: Option<wgpu::Buffer>,
    epoch: u64,
    meshes: HashMap<u64, MeshUpload>,
    // Allocated only for the private target-revision diagnostic. Mesh updates
    // already hold this record's read lock through Queue::write_buffer.
    mutation_serial: Option<AtomicU64>,
}

/// A `Send + Sync` upload surface for CPU-path meshes. Handed out by `wgr_uploader_get`
/// and valid until `wgr_destroy`; see the lifetime contract in `wgpu_renderer.hpp`.
pub struct Uploader {
    // Cloned from the renderer. `wgpu::Queue` is Send + Sync and internally refcounted,
    // so this clone is a refcount bump, not a second queue.
    //
    // The device is deliberately NOT held. The only mesh call that needs one is
    // `mesh_set_skin`, which creates a buffer — and the audit found that call structural,
    // so it stayed on the renderer handle. Holding a device here would advertise a
    // capability this surface does not have.
    queue: wgpu::Queue,
    published: RwLock<Published>,
    pending_destroy: Mutex<Vec<u64>>,
}

impl Uploader {
    pub fn new(queue: wgpu::Queue) -> Self {
        Self {
            queue,
            published: RwLock::new(Published {
                vbuf: None,
                epoch: u64::MAX, // no epoch yet; the first publish always takes.
                meshes: HashMap::new(),
                mutation_serial: None,
            }),
            pending_destroy: Mutex::new(Vec::new()),
        }
    }

    /// Republish the pool's current vertex buffer. Cheap and idempotent: it clones only
    /// when the epoch actually moved (or on the very first call), so the renderer can call
    /// it unconditionally at its two sites without thinking about it.
    pub fn publish_pool(&self, epoch: u64, vbuf: &wgpu::Buffer) {
        {
            let cur = self.published.read().expect("uploader published lock");
            if cur.epoch == epoch && cur.vbuf.is_some() {
                return;
            }
        }
        let mut cur = self.published.write().expect("uploader published lock");
        cur.epoch = epoch;
        cur.vbuf = Some(vbuf.clone());
    }

    /// Record a mesh's upload metadata, so `mesh_update` can find it without the renderer.
    pub fn register_mesh(&self, id: u64, meta: MeshUpload) {
        if id == 0 {
            return;
        }
        self.published
            .write()
            .expect("uploader published lock")
            .meshes
            .insert(id, meta);
    }

    /// Drop a mesh's metadata without queueing a destroy — used when the renderer destroys
    /// a mesh through its own (drained) path, so the two tables do not drift apart.
    pub fn forget_mesh(&self, id: u64) {
        self.published
            .write()
            .expect("uploader published lock")
            .meshes
            .remove(&id);
    }

    /// The per-frame path: rewrite an existing mesh's vertices in place. Returns whether
    /// a write was issued, which is what the unit tests assert on (a silent no-op and a
    /// successful upload look identical from the outside otherwise).
    pub fn mesh_update(&self, id: u64, verts: &[WgrMeshVertex]) -> bool {
        if id == 0 || verts.is_empty() {
            return false;
        }
        let cur = self.published.read().expect("uploader published lock");
        let Some(meta) = cur.meshes.get(&id).copied() else {
            return false;
        };
        // The same bound Gfx3d::mesh_update enforces: an update may rewrite a prefix of the
        // mesh, never past the end of its allocation into the next mesh's vertices.
        if verts.len() as u32 > meta.vert_count {
            return false;
        }
        let Some(vbuf) = cur.vbuf.as_ref() else {
            return false;
        };
        self.queue.write_buffer(
            vbuf,
            u64::from(meta.vbase) * VERT_SIZE,
            bytemuck::cast_slice(verts),
        );
        if let Some(serial) = &cur.mutation_serial {
            // A successful CPU queue write precedes the Release publication.
            // Zero is a permanent overflow sentinel; never alias an old sample.
            let _ = serial.fetch_update(Ordering::AcqRel, Ordering::Acquire,
                |n| Some(if n == 0 { 0 } else { n.checked_add(1).unwrap_or(0) }));
        }
        true
    }

    /// Opt-in at the diagnostic read boundary. The exclusive lock waits for any
    /// in-progress mesh_update read guard to finish, then returns a CPU-only
    /// linearization point. A newly enabled serial requires the caller to
    /// invalidate earlier untracked samples once.
    pub fn enable_mutation_serial_snapshot(&self) -> (u64, bool) {
        let mut cur = self.published.write().expect("uploader published lock");
        let newly_enabled = cur.mutation_serial.is_none();
        let serial = cur.mutation_serial.get_or_insert_with(|| AtomicU64::new(1))
            .load(Ordering::Acquire);
        (serial, newly_enabled)
    }

    /// Same CPU-only lock boundary after a diagnostic getter. Never waits on GPU
    /// completion or scans the mesh table.
    pub fn mutation_serial_snapshot(&self) -> Option<u64> {
        self.published.write().expect("uploader published lock")
            .mutation_serial.as_ref().map(|serial| serial.load(Ordering::Acquire))
    }

    /// Park a destroy for the renderer to run at its own frame start. The metadata entry
    /// goes immediately, so any later update on this handle is a no-op rather than a write
    /// into ranges a subsequent create may already have been given.
    pub fn enqueue_destroy(&self, id: u64) {
        if id == 0 {
            return;
        }
        self.forget_mesh(id);
        self.pending_destroy
            .lock()
            .expect("uploader destroy lock")
            .push(id);
    }

    /// Called by the renderer at the top of its frame, before anything reads
    /// `bake_bind_cache` — which `Gfx3d::mesh_destroy` mutates.
    pub fn take_pending_destroys(&self) -> Vec<u64> {
        std::mem::take(&mut *self.pending_destroy.lock().expect("uploader destroy lock"))
    }

    /// Test/diagnostic accessor: the metadata this surface holds for a handle.
    pub fn mesh_meta(&self, id: u64) -> Option<MeshUpload> {
        self.published
            .read()
            .expect("uploader published lock")
            .meshes
            .get(&id)
            .copied()
    }

    /// Test/diagnostic accessor: the pool epoch this surface last published.
    pub fn published_epoch(&self) -> u64 {
        self.published.read().expect("uploader published lock").epoch
    }
}

// The whole point of the type: it must be usable from a thread that does not hold the
// renderer. If a future field breaks this, the failure is a compile error here rather
// than an aliasing bug at runtime.
const _: fn() = || {
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<Uploader>();
};

#[cfg(test)]
mod tests {
    use super::*;
    use crate::gfx3d::pool::GeometryPool;
    use bytemuck::Zeroable;

    // Vertices tagged so a wrong offset is visible in the readback rather than merely
    // "different bytes": vertex i of a mesh tagged T reads back as T + i.
    fn tagged(n: u32, tag: f32) -> (Vec<WgrMeshVertex>, Vec<u32>) {
        let mut verts = vec![WgrMeshVertex::zeroed(); n as usize];
        for (i, v) in verts.iter_mut().enumerate() {
            v.pos.x = tag + i as f32;
        }
        (verts, (0..n).collect())
    }

    fn read_back(
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        pool: &GeometryPool,
        vbase: u32,
        count: u32,
    ) -> Vec<f32> {
        let bytes = u64::from(count) * VERT_SIZE;
        let staging = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("uploader_readback"),
            size: bytes,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        let mut enc =
            device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        enc.copy_buffer_to_buffer(
            pool.vbuf(),
            u64::from(vbase) * VERT_SIZE,
            &staging,
            0,
            bytes,
        );
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

    // Construction, and the metadata bookkeeping an upload depends on.
    #[test]
    fn metadata_registers_forgets_and_gates_updates() {
        let Some((_device, queue)) = crate::gfx3d::cull::tests::headless() else {
            return;
        };
        let up = Uploader::new(queue);

        // Nothing published yet: an update has no buffer and no metadata to write through.
        let (v, _) = tagged(4, 0.0);
        assert!(!up.mesh_update(7, &v), "update before register must be a no-op");

        up.register_mesh(
            7,
            MeshUpload {
                vbase: 12,
                vert_count: 4,
            },
        );
        assert_eq!(
            up.mesh_meta(7),
            Some(MeshUpload {
                vbase: 12,
                vert_count: 4
            })
        );
        // Handle 0 is the failure sentinel of wgr_mesh_create and must never be recorded.
        up.register_mesh(
            0,
            MeshUpload {
                vbase: 0,
                vert_count: 1,
            },
        );
        assert_eq!(up.mesh_meta(0), None);

        up.forget_mesh(7);
        assert_eq!(up.mesh_meta(7), None);
    }

    // The load-bearing property: bytes handed to the uploader land at the right mesh, and
    // ONLY at the right mesh. The second assertion is the one that catches a wrong offset --
    // a scaled or shifted vbase still writes plausible-looking data somewhere, and only the
    // neighbour's survival distinguishes "correct" from "corrupted something else".
    #[test]
    fn an_update_reaches_the_pool_at_the_right_offset_and_leaves_the_neighbour_alone() {
        let Some((device, queue)) = crate::gfx3d::cull::tests::headless() else {
            return;
        };
        let mut pool = GeometryPool::new(&device);

        // Two meshes, so "wrote at the wrong base" has somewhere wrong to land.
        let (av, ai) = tagged(64, 1000.0);
        let a = pool.alloc(&device, &queue, &av, &ai).expect("mesh a");
        let (bv, bi) = tagged(64, 5000.0);
        let b = pool.alloc(&device, &queue, &bv, &bi).expect("mesh b");
        assert_ne!(a.vbase, b.vbase);

        let up = Uploader::new(queue.clone());
        up.publish_pool(pool.generation(), pool.vbuf());
        up.register_mesh(
            2,
            MeshUpload {
                vbase: b.vbase,
                vert_count: 64,
            },
        );

        assert_eq!(up.mutation_serial_snapshot(), None, "normal path has no serial");
        assert_eq!(up.enable_mutation_serial_snapshot(), (1, true));
        assert_eq!(up.enable_mutation_serial_snapshot(), (1, false));
        assert!(!up.mesh_update(999, &av), "unknown handle must not advance serial");
        assert_eq!(up.mutation_serial_snapshot(), Some(1));

        let (fresh, _) = tagged(64, 7777.0);
        assert!(up.mesh_update(2, &fresh), "a registered update must upload");
        assert_eq!(up.mutation_serial_snapshot(), Some(2),
            "only the completed queue write advances the serial");

        let got_b = read_back(&device, &queue, &pool, b.vbase, 64);
        assert_eq!(got_b[0], 7777.0, "mesh b did not receive the new vertices");
        assert_eq!(got_b[63], 7777.0 + 63.0, "mesh b's tail is wrong");

        let got_a = read_back(&device, &queue, &pool, a.vbase, 64);
        assert_eq!(
            got_a[0], 1000.0,
            "mesh a was overwritten -- the upload went to the wrong offset"
        );
        assert_eq!(got_a[63], 1000.0 + 63.0, "mesh a's tail was overwritten");
    }

    // The bound Gfx3d::mesh_update enforces, kept here so an off-handle upload cannot run
    // past the end of its allocation into whatever mesh the pool put next to it.
    #[test]
    fn an_oversized_update_is_refused_rather_than_clipped() {
        let Some((device, queue)) = crate::gfx3d::cull::tests::headless() else {
            return;
        };
        let mut pool = GeometryPool::new(&device);
        let (av, ai) = tagged(32, 1000.0);
        let a = pool.alloc(&device, &queue, &av, &ai).expect("mesh a");
        let (bv, bi) = tagged(32, 5000.0);
        let b = pool.alloc(&device, &queue, &bv, &bi).expect("mesh b");

        let up = Uploader::new(queue.clone());
        up.publish_pool(pool.generation(), pool.vbuf());
        up.register_mesh(
            1,
            MeshUpload {
                vbase: a.vbase,
                vert_count: 32,
            },
        );
        assert_eq!(up.enable_mutation_serial_snapshot(), (1, true));

        let (too_many, _) = tagged(64, 9999.0);
        assert!(!up.mesh_update(1, &too_many), "oversized update must refuse");
        assert_eq!(up.mutation_serial_snapshot(), Some(1),
            "refused update cannot appear as a successful mutation");
        assert_eq!(
            read_back(&device, &queue, &pool, b.vbase, 1)[0],
            5000.0,
            "the refused update still spilled into the next mesh"
        );
    }

    // Destroy is deferred, not done: it removes bake_bind_cache entries the draw path reads
    // and returns ranges to the pool's free lists. What the uploader owes is that the handle
    // stops accepting uploads AT ENQUEUE, and that the renderer gets it exactly once.
    #[test]
    fn a_deferred_destroy_drains_once_and_stops_further_uploads() {
        let Some((_device, queue)) = crate::gfx3d::cull::tests::headless() else {
            return;
        };
        let up = Uploader::new(queue);
        up.register_mesh(
            9,
            MeshUpload {
                vbase: 0,
                vert_count: 8,
            },
        );

        assert!(up.take_pending_destroys().is_empty(), "nothing parked yet");
        up.enqueue_destroy(9);
        assert_eq!(
            up.mesh_meta(9),
            None,
            "metadata must go at enqueue, not at drain"
        );

        let (v, _) = tagged(8, 0.0);
        assert!(
            !up.mesh_update(9, &v),
            "an upload after destroy must be ignored, not written into reused ranges"
        );

        assert_eq!(up.take_pending_destroys(), vec![9]);
        assert!(
            up.take_pending_destroys().is_empty(),
            "a drained destroy must not be handed out twice"
        );
        // The 0 sentinel is never a real mesh and must not reach Gfx3d::mesh_destroy.
        up.enqueue_destroy(0);
        assert!(up.take_pending_destroys().is_empty());
    }

    // The republish rule (trap 1): the pool's buffer OBJECT is replaced by a growth, and an
    // uploader still holding the retired one would write into a buffer wgpu rejects at
    // submit. This drives a real growth and asserts the epoch moved and the write still lands.
    #[test]
    fn a_pool_growth_republishes_and_uploads_keep_landing() {
        let Some((device, queue)) = crate::gfx3d::cull::tests::headless() else {
            return;
        };
        let mut pool = GeometryPool::new(&device);
        let (sv, si) = tagged(100, 1000.0);
        let keep = pool.alloc(&device, &queue, &sv, &si).expect("survivor");

        let up = Uploader::new(queue.clone());
        up.publish_pool(pool.generation(), pool.vbuf());
        let before = pool.generation();
        up.register_mesh(
            5,
            MeshUpload {
                vbase: keep.vbase,
                vert_count: 100,
            },
        );

        // Big enough to force a vertex-buffer growth, which reallocates vbuf.
        let (bv, bi) = tagged(400_000, 0.0);
        pool.alloc(&device, &queue, &bv, &bi).expect("growth");
        assert_ne!(pool.generation(), before, "the growth did not move the epoch");

        // What Renderer::mesh_create does at this point.
        up.publish_pool(pool.generation(), pool.vbuf());
        assert_eq!(up.published_epoch(), pool.generation());

        let (fresh, _) = tagged(100, 4242.0);
        assert!(up.mesh_update(5, &fresh));
        let got = read_back(&device, &queue, &pool, keep.vbase, 100);
        assert_eq!(got[0], 4242.0, "post-growth upload did not reach the pool");
        assert_eq!(got[99], 4242.0 + 99.0);
    }
}
