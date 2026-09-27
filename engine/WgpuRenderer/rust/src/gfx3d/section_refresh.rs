//! Constant-cost validity for section-address resolution, not geometry/content freshness.
//! Sources are immutable after append; live mesh offsets never move. Structural map/table
//! writes invalidate explicitly. Pool generation additionally covers buffer replacements.
#[derive(Clone, Copy, Debug)]
pub(super) struct SectionRefreshValidity {
    dirty: bool,
    force_legacy: bool, // fills existing padding; exact startup setting only
    pool_generation: u64,
}

impl Default for SectionRefreshValidity {
    fn default() -> Self {
        Self::from_setting(None)
    }
}

impl SectionRefreshValidity {
    // Unset/default optimizes; ONLY exact "0" forces the original every-frame body.
    pub(super) fn from_setting(value: Option<&str>) -> Self {
        Self {
            dirty: true,
            force_legacy: value == Some("0"),
            pool_generation: 0,
        }
    }
    pub(super) fn invalidate(&mut self) {
        self.dirty = true;
    }
    pub(super) fn needs_refresh(&self, pool_generation: u64) -> bool {
        self.force_legacy || self.dirty || self.pool_generation != pool_generation
    }
    // The live renderer and tests use this same closure dispatch. A clean frame never
    // invokes work; only a COMPLETE returned callback publishes validity. The caller
    // assigns the returned Copy state afterward, so panic keeps its original state.
    pub(super) fn run_if_needed(mut self, pool_generation: u64, work: impl FnOnce()) -> Self {
        if self.needs_refresh(pool_generation) {
            work();
            self.refreshed(pool_generation);
        }
        self
    }
    // Only the successful end of a COMPLETE refresh may publish validity.
    pub(super) fn refreshed(&mut self, pool_generation: u64) {
        self.pool_generation = pool_generation;
        self.dirty = false;
    }
}

// Intentional default scalar footprint, no heap state or per-section tax.
const _: () = assert!(std::mem::size_of::<SectionRefreshValidity>() == 16);

#[cfg(test)]
mod tests {
    use super::super::cull::SectionGpu;
    use super::*;
    use slotmap::{new_key_type, Key, KeyData, SlotMap};
    new_key_type! { struct MeshKey; }
    #[derive(Clone, Copy)]
    struct Source {
        mesh: u64,
        begin: u32,
        count: u32,
        variant: u32,
    }
    fn resolve(s: &Source, meshes: &SlotMap<MeshKey, (u32, u32)>) -> SectionGpu {
        match meshes.get(KeyData::from_ffi(s.mesh).into()) {
            Some(&(vbase, ibase)) => SectionGpu {
                first_index: ibase + s.begin,
                index_count: s.count,
                base_vertex: vbase,
                variant: s.variant,
            },
            None => SectionGpu {
                first_index: 0,
                index_count: 0,
                base_vertex: 0,
                variant: s.variant,
            },
        }
    }
    fn frame(
        state: &mut SectionRefreshValidity,
        generation: u64,
        sources: &[Source],
        meshes: &SlotMap<MeshKey, (u32, u32)>,
        cached: &mut Vec<SectionGpu>,
        calls: &mut usize,
    ) {
        *state = state.run_if_needed(generation, || {
            *cached = sources
                .iter()
                .map(|s| {
                    *calls += 1;
                    resolve(s, meshes)
                })
                .collect();
        });
        let baseline: Vec<_> = sources.iter().map(|s| resolve(s, meshes)).collect();
        assert!(
            cached.as_slice() == baseline.as_slice(),
            "exact always-resolve baseline, including missing generations"
        );
    }

    #[test]
    fn section_refresh_clean_frames_skip_and_source_append_and_retire_invalidate() {
        let mut state = SectionRefreshValidity::default();
        let mut meshes: SlotMap<MeshKey, (u32, u32)> = SlotMap::with_key();
        let key = meshes.insert((12, 24));
        let mut sources = Vec::new();
        let mut cached = Vec::new();
        let mut calls = 0;
        frame(&mut state, 1, &sources, &meshes, &mut cached, &mut calls);
        assert!(!state.needs_refresh(1));
        sources.push(Source {
            mesh: key.data().as_ffi(),
            begin: 2,
            count: 3,
            variant: 7,
        });
        state.invalidate(); // production source append
        frame(&mut state, 1, &sources, &meshes, &mut cached, &mut calls);
        assert_eq!(calls, 1);
        for _ in 0..64 {
            frame(&mut state, 1, &sources, &meshes, &mut cached, &mut calls);
        }
        assert_eq!(calls, 1);
        state.invalidate(); // production retirement retains exact address values
        frame(&mut state, 1, &sources, &meshes, &mut cached, &mut calls);
        assert_eq!(calls, 2);
        assert_eq!(
            (
                cached[0].base_vertex,
                cached[0].first_index,
                cached[0].variant
            ),
            (12, 26, 7)
        );
    }

    #[test]
    fn section_refresh_destroy_recreate_same_capacity_never_resurrects_old_generation() {
        let mut state = SectionRefreshValidity::default();
        let mut meshes: SlotMap<MeshKey, (u32, u32)> = SlotMap::with_key();
        let old = meshes.insert((4, 8));
        let mut sources = vec![Source {
            mesh: old.data().as_ffi(),
            begin: 1,
            count: 3,
            variant: 2,
        }];
        let mut cached = Vec::new();
        let mut calls = 0;
        frame(&mut state, 7, &sources, &meshes, &mut cached, &mut calls);
        assert_eq!(meshes.remove(old), Some((4, 8)));
        state.invalidate();
        frame(&mut state, 7, &sources, &meshes, &mut cached, &mut calls);
        assert!(
            cached[0]
                == SectionGpu {
                    first_index: 0,
                    index_count: 0,
                    base_vertex: 0,
                    variant: 2
                }
        );
        let new = meshes.insert((40, 80));
        state.invalidate();
        assert_eq!(old.data().as_ffi() as u32, new.data().as_ffi() as u32);
        assert_ne!(old.data().as_ffi(), new.data().as_ffi());
        frame(&mut state, 7, &sources, &meshes, &mut cached, &mut calls);
        assert_eq!(cached[0].index_count, 0);
        sources.push(Source {
            mesh: new.data().as_ffi(),
            begin: 1,
            count: 3,
            variant: 2,
        });
        state.invalidate();
        frame(&mut state, 7, &sources, &meshes, &mut cached, &mut calls);
        assert_eq!(
            (
                cached[1].base_vertex,
                cached[1].first_index,
                cached[1].index_count
            ),
            (40, 81, 3)
        );
        let before = calls;
        frame(&mut state, 7, &sources, &meshes, &mut cached, &mut calls);
        assert_eq!(calls, before);
    }

    #[test]
    fn section_refresh_pool_generation_requires_one_refresh_without_mesh_mutation() {
        let mut state = SectionRefreshValidity::default();
        state.refreshed(9);
        assert!(!state.needs_refresh(9));
        assert!(state.needs_refresh(10));
        state.refreshed(10);
        assert!(!state.needs_refresh(10));
        assert!(state.needs_refresh(0)); // comparison has no monotonic/wrap assumption
    }

    #[test]
    fn section_refresh_incomplete_work_cannot_publish_validity() {
        let mut state = SectionRefreshValidity::default();
        state.refreshed(3);
        state.invalidate();
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            state = state.run_if_needed(3, || {
                panic!("resolution/allocation failed before successful final publication");
            });
        }));
        assert!(result.is_err());
        assert!(state.needs_refresh(3));
        state.refreshed(3);
        assert!(!state.needs_refresh(3));
    }

    #[test]
    fn section_refresh_actual_pool_content_upload_and_same_capacity_recreation() {
        use super::super::pool::GeometryPool;
        use crate::ffi::WgrMeshVertex;
        use bytemuck::Zeroable;
        let Some((device, queue)) = super::super::cull::tests::headless() else {
            eprintln!("[section-refresh] SKIP actual GPU upload/readback: no adapter");
            return;
        };
        let mut pool = GeometryPool::new(&device);
        let mut verts = vec![WgrMeshVertex::zeroed(); 3];
        for v in &mut verts {
            v.pos.x = 1.0;
        }
        let alloc = pool.alloc(&device, &queue, &verts, &[0, 1, 2]).unwrap();
        let generation = pool.generation();
        let mut meshes: SlotMap<MeshKey, (u32, u32)> = SlotMap::with_key();
        let old = meshes.insert((alloc.vbase, alloc.ibase));
        let mut sources = vec![Source {
            mesh: old.data().as_ffi(),
            begin: 0,
            count: 3,
            variant: 0,
        }];
        let mut state = SectionRefreshValidity::default();
        let mut cached = Vec::new();
        let mut calls = 0;
        frame(
            &mut state,
            generation,
            &sources,
            &meshes,
            &mut cached,
            &mut calls,
        );
        let initial = cached.clone();
        // The actual producer upload changes GPU vertex content, NOT section addresses.
        for v in &mut verts {
            v.pos.x = 7.0;
        }
        pool.update_verts(&queue, alloc.vbase, &verts);
        frame(
            &mut state,
            pool.generation(),
            &sources,
            &meshes,
            &mut cached,
            &mut calls,
        );
        assert_eq!(calls, 1);
        assert!(cached == initial);
        let bytes = std::mem::size_of_val(verts.as_slice()) as u64;
        let staging = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("section_refresh_content_readback"),
            size: bytes,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor::default());
        encoder.copy_buffer_to_buffer(
            pool.vbuf(),
            alloc.vbase as u64 * std::mem::size_of::<WgrMeshVertex>() as u64,
            &staging,
            0,
            bytes,
        );
        queue.submit([encoder.finish()]);
        let slice = staging.slice(..);
        let (tx, rx) = std::sync::mpsc::channel();
        slice.map_async(wgpu::MapMode::Read, move |r| {
            let _ = tx.send(r);
        });
        device.poll(wgpu::PollType::wait_indefinitely()).unwrap();
        rx.recv().unwrap().unwrap();
        {
            let data = slice.get_mapped_range();
            let uploaded: &[WgrMeshVertex] = bytemuck::cast_slice(&data);
            assert!(uploaded.iter().all(|v| v.pos.x == 7.0));
        }
        staging.unmap();
        meshes.remove(old);
        state.invalidate();
        pool.free(&alloc, 3, 3);
        frame(
            &mut state,
            pool.generation(),
            &sources,
            &meshes,
            &mut cached,
            &mut calls,
        );
        assert_eq!(cached[0].index_count, 0);
        let replacement = pool.alloc(&device, &queue, &verts, &[0, 1, 2]).unwrap();
        assert_eq!(
            pool.generation(),
            generation,
            "same-capacity creation must still invalidate"
        );
        let new = meshes.insert((replacement.vbase, replacement.ibase));
        state.invalidate();
        assert_eq!(new.data().as_ffi() as u32, old.data().as_ffi() as u32);
        assert_ne!(new.data().as_ffi(), old.data().as_ffi());
        sources.push(Source {
            mesh: new.data().as_ffi(),
            begin: 0,
            count: 3,
            variant: 0,
        });
        state.invalidate();
        frame(
            &mut state,
            pool.generation(),
            &sources,
            &meshes,
            &mut cached,
            &mut calls,
        );
        assert_eq!(cached[0].index_count, 0);
        assert_eq!(cached[1].index_count, 3);
        pool.free(&replacement, 3, 3);
        eprintln!("[section-refresh] actual GPU upload/readback PASS; same-capacity full-generation recreation PASS");
    }

    #[test]
    fn section_refresh_legacy_setting_is_exact_and_uses_existing_padding() {
        for value in [
            None,
            Some("1"),
            Some("00"),
            Some("0junk"),
            Some(" 0"),
            Some(""),
        ] {
            let mut state = SectionRefreshValidity::from_setting(value);
            state.refreshed(4);
            assert!(!state.needs_refresh(4), "only exact zero can force legacy");
        }
        let mut legacy = SectionRefreshValidity::from_setting(Some("0"));
        legacy.refreshed(4);
        assert!(legacy.needs_refresh(4));
        assert_eq!(std::mem::size_of::<SectionRefreshValidity>(), 16);
    }

    #[test]
    fn section_refresh_legacy_runs_every_body_default_skips_with_exact_same_addresses() {
        let mut optimized = SectionRefreshValidity::default();
        let mut legacy = SectionRefreshValidity::from_setting(Some("0"));
        let mut meshes: SlotMap<MeshKey, (u32, u32)> = SlotMap::with_key();
        let key = meshes.insert((32, 64));
        let sources = [Source {
            mesh: key.data().as_ffi(),
            begin: 2,
            count: 3,
            variant: 5,
        }];
        let mut opt_cache = Vec::new();
        let mut legacy_cache = Vec::new();
        let (mut opt_calls, mut legacy_calls) = (0, 0);
        for _ in 0..64 {
            frame(
                &mut optimized,
                3,
                &sources,
                &meshes,
                &mut opt_cache,
                &mut opt_calls,
            );
            frame(
                &mut legacy,
                3,
                &sources,
                &meshes,
                &mut legacy_cache,
                &mut legacy_calls,
            );
            assert!(opt_cache == legacy_cache);
        }
        assert_eq!(opt_calls, 1);
        assert_eq!(legacy_calls, 64);
        meshes.remove(key);
        optimized.invalidate();
        legacy.invalidate();
        frame(
            &mut optimized,
            3,
            &sources,
            &meshes,
            &mut opt_cache,
            &mut opt_calls,
        );
        frame(
            &mut legacy,
            3,
            &sources,
            &meshes,
            &mut legacy_cache,
            &mut legacy_calls,
        );
        assert!(opt_cache == legacy_cache);
        assert_eq!(opt_cache[0].index_count, 0);
        frame(
            &mut optimized,
            3,
            &sources,
            &meshes,
            &mut opt_cache,
            &mut opt_calls,
        );
        frame(
            &mut legacy,
            3,
            &sources,
            &meshes,
            &mut legacy_cache,
            &mut legacy_calls,
        );
        assert_eq!(opt_calls, 2);
        assert_eq!(legacy_calls, 66);
    }
}
