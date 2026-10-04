# OpenPoseidon: gezielte Upstream-Arbeit

Ziel: `codex/cave-mesh-20261004` @ `b5dc6070cf5a5b2feff0bafd612ed8567529c919`. Statischer Audit; keine Implementierung erfolgt.

## P1-PAC — PR #92 (nur Nicht-Metal-Anteile)

Priorität: hoch. Formatmarker-gesteuerte PAC/PAA-Decodierung samt Fixture; Streaming-Provenance erhalten.

Abnahme: AI88/LZSS .pac, PBO + loose, Mip-Kette, Resident/Reload/Remount; AI88-8bit und LZO-DXT nicht regressieren.

## P1-LIFETIME — PR #239; PR #168 (Teilfixes)

Priorität: hoch. ModCollection-Lifetime; Harness-Thread-Join mit Abbruchpfaden; atomare Flags und Lock-Diagnostik.

Abnahme: Modded join, Stop während accept/response/send, Wiederholstarts, Sanitizer soweit verfügbar.

## P1-AI — PR #237; fehlende Guards aus #238; PR #209

Priorität: hoch. Endliche/sichere Pfadkoordinaten, Magazine-/Muzzle-Grenzen, stabile statische Waffenbesatzung.

Abnahme: NaN/Inf/beide Endpunkte, Index -1/N, verlorene Magazine, zwei MGs/ein getöteter Schütze, Mission/MP-Regression.

## P1-DUMP — main a13cac07

Priorität: hoch für Diagnostik. Vollständigen lesbaren Crash-Kontext kopieren; eigene DIAG-001-Callbacks erhalten.

Abnahme: PAGE_NOACCESS-Grenzfall, MinidumpWriteDump-Erfolg; keine Behebung des eigentlichen Crashs behaupten.

## P2-COMPAT — PR #305, #300, #73, #72, #290

Priorität: mittel bis hoch. Fahrzeug-Beinkorrektur, drop-ATL, Turretwinkel, Mod-Fernglas/NVG, Desktop-Schließen bei disableUserInput.

Abnahme: Hang-Einstieg, Partikel ungebunden/objektrelativ, verschiedene Azimute, Legacy-/Custom-Klassen, Eingabe an/aus.

## P2-SMALL — PR #173 Tank-Teil; PR #79 vier Reststellen; PR #284 Logfile-Teil

Priorität: mittel. Tank-Flag initialisieren, Center-Radio-Diagnosen vervollständigen, explizites Logfile optional append.

Abnahme: Deterministische Konstruktion, Fremdgruppe im Center-/Gruppenkanal, Neustartmarker im selben Logfile.

## P2-TEST — main 02c0d741,7d69cbe3,694f7497; historisch f9057657

Priorität: hoch für Messqualität. LOD-Freeze, simulationszeitbasierte Trident-Tests, echter negativer Up-Vektor; historische Copy-/Langpfad-Tests separat nachprüfen.

Abnahme: CPU/GPU/Shadow-Lod gemeinsam fixiert, gleiche Sim-Zeit bei unterschiedlicher Rendergeschwindigkeit.

## P3-RNG — PR #172

Priorität: hoch als Entwurf, kein Drop-in. Nur kosmetischen RNG sauber trennen; Explosionsschaden zwingend im Simulationspfad lassen.

Abnahme: Sim-Hashes mit anderer Kamera/FX-Last, Save/Load, Zuständigkeit/Ownership, Kollisionsanimation.

## P3-PERF — main cc4bd699/3541affa; PR #318; bedingt #95/#105/#111/#112

Priorität: messungsabhängig. Surface-Liste vorfiltern und Karten-Batching messen; numerische und Container-Optimierungen nur nach Vertragstest.

Abnahme: Sortierties/Alpha/Overlay-Reihenfolge, Marker-Mutationen/Deserialize, float-Grenzen/Aliasing/Architekturen.

## P3-PLATFORM — main Build-/CI-Pakete; PR #92 OpenAL

Priorität: mittel, bedingt. CI/Härtung inkrementell, macOS OpenAL-Bundled-Suche/EFX-Vertrag prüfen.

Abnahme: Compilerflags, unveränderte WGPU/Trident-Gates, macOS ohne/mit EFX.

Keine Whole-PR-Cherry-Picks für #92/#168/#172/#238/#294/#3. Insbesondere Simulationsschaden nie in FX-RNG verlagern und _isPaa nicht ohne Prüfung der Streaming-Provenance umdeuten.
