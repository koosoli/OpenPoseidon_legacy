# Tidewater W12d integration, 28 September 2026

The `dec/tidewater-w12d-mesh-waterline` range was checked against `codex/streaming-residency`. Its older boat-drag patch `bccf4b40` is already represented by streamer commit `975d4aff`, with the streamer's buoyancy diagnostic guard retained. The two missing patches were applied in source order as `e2396319` (infantry drowning off by default, `POSEIDON_INFANTRY_DROWN=1` restores it) and `8b7148db` (lens waterline follows the rendered mesh). These were committed and pushed before installation.

The isolated port passed 54 Tidewater Rust tests with one pre-existing ignored test, compiled `SoldierOldSim.cpp` with clang-cl, and passed `git diff --check`. The integrated `PoseidonGame` build, paired EXE/DLL deployment to the shared installation, and installed `--render=wgpu --check` startup passed. `DEPLOYED-FROM.txt` records `codex/streaming-residency 8b7148db`. Oliver was invited to inspect the interactive water showcase; that visual judgment and a matched performance comparison remain open.

The streamer remains experimental and default-off. This integration does not establish all-view GeometryPage absence, Fine release/reclaim, or a no-regression performance result.
