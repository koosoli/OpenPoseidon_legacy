# GI sun-proxy target-model COUNT diagnostic (2026-09-28)

The private original-source `WgrMainTargetCountRequest` now also arms a nonblocking COUNT for the GI/RSM sun-proxy cull view (sky view 5). `wgr_gi_target_count_fact` returns the existing 48-byte version-1 identity, status and count. This counter is separate from the main view and four solar cascades; no default-frame readback buffer, copy, map or poll is added.

The fact remains Unknown unless that request's model and source generation match the retained cull epoch, sky view 5 actually dispatches, the RSM indirect draw is recorded and its pass closes, and the final encoder's copy is submitted and the render attempt commits. A disabled GI path, cached RSM with no refresh, or failed draw cannot publish a false zero. This is a view-level surviving-instance count. It does not prove pixels, solve all-view reference closure or authorize Fine geometry release. Runtime and performance acceptance remain separate pending work.
