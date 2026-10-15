# Upstream-main-Nachtrag CWR-CE, 2026-10-15

Ergänzt [CWR-CE_Audit_cave-mesh_b5dc607_2026-10-04.md](CWR-CE_Audit_cave-mesh_b5dc607_2026-10-04.md)
und [UPSTREAM-RUNTIME-INTEGRATION-20261004.md](UPSTREAM-RUNTIME-INTEGRATION-20261004.md).

- Upstream: `ofpisnotdead-com/CWR-CE`, main `aa522583b59025921340c68122692154902d0dbf` (07.10.2026).
- Stand des Audits: main `b36cddc7ef745da86d308038108986b1641b574a`. Dazwischen liegen genau drei Commits.
- Gemeinsame Git-Basis der MrDudeX-Branches mit upstream ist weiterhin `a6e3f2fc` (23.08.2026);
  alle späteren Upstream-Commits kommen als einzelne Ports, nicht als Merge.
- Alle 22 Port-Commits von `codex/upstream-runtime-20261004` sind in MrDudeX, MrDudeX-arma1,
  MrDudeX-arma2 und MrDudeX-arma3 patchgleich enthalten (`git cherry`).

| Upstream | Titel | Entscheidung | Hinweise |
|---|---|---|---|
| [8764e0d8](https://github.com/ofpisnotdead-com/CWR-CE/commit/8764e0d8) | Clear the compiler warnings the Windows build emits | übernommen (`cherry-pick -x`) | `#undef _findfirst/_findnext/_findclose` war im Fork schon vorhanden, nur der Kommentar kam dazu. Klammern in `RoleSwapAllowed` (gleiche Präzedenz) und `using NetworkComponent::SendMsg` neu. Kein Verhaltenswechsel. |
| [58142d2c](https://github.com/ofpisnotdead-com/CWR-CE/commit/58142d2c) | Treat compiler warnings as errors on every platform | **nicht übernommen** | Setzt `CMAKE_COMPILE_WARNING_AS_ERROR` global und entfernt `-Wno-error` aus Poseidon/PoseidonGL33. Der Fork-Code (WGPU, Arma-1/2/3-Brücken, Rust-Anbindung) ist nie warnungsfrei gebaut worden; ohne eigenen Warnungs-Census würde das den Build brechen. Plattform-/CI-Entscheidung des Repo-Eigners. |
| [aa522583](https://github.com/ofpisnotdead-com/CWR-CE/commit/aa522583) | Initialize terrain slopes outside the map | übernommen (`cherry-pick -x`) | Behebt Hänger mit ECP und anderen großen Mods. Konflikt in `Landscape::SurfaceY`: die Steigungen werden jetzt **vor** dem Earth-Streaming-Zweig auf 0 gesetzt, damit jeder Rückweg definierte Werte liefert. Neuer Test `World/test_collision_nonfinite.cpp` zusätzlich zu den Fork-eigenen Terrain-Tests in die Testliste aufgenommen. |

## Prüfgrenze

Konflikte nur textuell aufgelöst und gegen den Upstream-Diff verglichen. Kein Build und kein
Unit-Testlauf in dieser Sitzung (kein Windows-Toolchain/vcpkg verfügbar). Vor dem nächsten Deploy:
`scripts\Build.ps1`, dann die Unit-Tests der Suite `PoseidonTests` mit dem Catch2-Tag `[Collision]`.

Offene Upstream-PRs wurden hier nicht neu bewertet.
