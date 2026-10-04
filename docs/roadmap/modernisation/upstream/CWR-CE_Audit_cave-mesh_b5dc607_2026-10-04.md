# CWR-CE → OpenPoseidon: Main- und PR-Audit

**Stand: 2026-10-04. Ziel: `codex/cave-mesh-20261004` bei `b5dc6070cf5a5b2feff0bafd612ed8567529c919`.**

Upstream `main`: `b36cddc7ef745da86d308038108986b1641b574a`. **Kein neuer Main-Commit seit dem Audit vom 01.10.2026.**

## Ergebnis vorweg

Kein großer Upstream-Merge, kein neuer GL33-/Metal-Renderer und kein erneuter Off-map-Ozean-Umbau. Die wertvollen offenen Arbeiten liegen vor allem in kleinen Laufzeit-Schutzmaßnahmen, KI-/Mod-Kompatibilität, Tests und in ausgewählten backend-unabhängigen Teilen größerer PRs.

Besonders wichtig: **PR #92 enthält einen auch für WGPU relevanten PAC/PAA-Decoder-Fix**, obwohl der PR-Titel einen Metal-Renderer ankündigt. Umgekehrt verschiebt **PR #172 auch Explosionsschaden** in den vermeintlich kosmetischen RNG. Beides zeigt, warum Titel und fehlende SHAs als Port-Entscheidung nicht reichen.

## Prüfungsumfang und Grenzen

Die 102 Main-Commit-Einzelbewertungen des vorhandenen Audits wurden erhalten. Upstream-main ist gegenüber 2026-10-01 unverändert.

Offene Kernempfehlungen und ausdrücklich annotierte Produktionsstellen wurden am neuen Ziel-SHA erneut gelesen; CI-Fortgeltung zusätzlich über identische .github-Tree-SHA belegt.

Alle 54 offenen PR-Patches wurden eingesehen. Kandidaten mit direkter Implementierungsempfehlung wurden gegen die genannten Zielcode-Stellen geprüft; bedingte/optionale Entwürfe sind nicht als fehlende, fertig portierbare Fixes behauptet.

12 geschlossene/unmerged PRs sind separat referenziert. Vier davon wurden auf Diff-Ebene gelesen; acht nur auf Metadatenebene. Bereits gemergte historische PRs sind über main erfasst, nicht alle erneut als PR-Review durchgeführt.

**Grenze:** Statischer Review. Keine Builds, Tests, Benchmarks, RenderDoc-Captures oder Laufzeit-Dumps von mir ausgeführt. Keine Repository- oder Workflow-Änderungen.

**Grenze:** Kein neuer vollständiger patch-id-/Cherry-Pick-Abgleich über die private Gesamthistorie. 102 ist die fortgeführte Audit-Range, nicht die behauptete Zahl aktuell fehlender Implementierungen.

**Grenze:** Nicht jede alte historische Äquivalenzbehauptung wurde auf diesem neuen Branch neu bewiesen. Per-Commit current_review.scope zeigt exakt den erneuten Prüfungsumfang.

**Grenze:** Ein gelesener PR-Diff ist keine Freigabe sämtlicher Hunks. Besonders #92, #168, #172, #238, #294 und #3 brauchen Aufteilung.

**Grenze:** Quellcode vorhandenen Verhaltens beweist nicht, dass alle Upstream-Testfixtures übernommen wurden. Fehlender Funktionsname beweist allein keine fehlende äquivalente Implementierung.

**Grenze:** Upstream-Messwerte sind keine Fork-Prognose. Numerische Optimierungen brauchen eigene Determinismus-/Architekturtests.

Die Ref-Abfragen wurden während des Reviews erneut gelesen; beide oben genannten SHAs waren weiterhin aktuell. `.github` ist als kompletter Tree identisch zum letzten Zielstand: `5412568222e10f6e84277b72f2d648dcb72f9c3b`.

## Empfohlene Arbeitspakete

### P1-PAC — PR #92 (nur Nicht-Metal-Anteile)

**Relevanz: hoch.** Formatmarker-gesteuerte PAC/PAA-Decodierung samt Fixture; Streaming-Provenance erhalten.

**Abnahmetests:** AI88/LZSS .pac, PBO + loose, Mip-Kette, Resident/Reload/Remount; AI88-8bit und LZO-DXT nicht regressieren.

### P1-LIFETIME — PR #239; PR #168 (Teilfixes)

**Relevanz: hoch.** ModCollection-Lifetime; Harness-Thread-Join mit Abbruchpfaden; atomare Flags und Lock-Diagnostik.

**Abnahmetests:** Modded join, Stop während accept/response/send, Wiederholstarts, Sanitizer soweit verfügbar.

### P1-AI — PR #237; fehlende Guards aus #238; PR #209

**Relevanz: hoch.** Endliche/sichere Pfadkoordinaten, Magazine-/Muzzle-Grenzen, stabile statische Waffenbesatzung.

**Abnahmetests:** NaN/Inf/beide Endpunkte, Index -1/N, verlorene Magazine, zwei MGs/ein getöteter Schütze, Mission/MP-Regression.

### P1-DUMP — main a13cac07

**Relevanz: hoch für Diagnostik.** Vollständigen lesbaren Crash-Kontext kopieren; eigene DIAG-001-Callbacks erhalten.

**Abnahmetests:** PAGE_NOACCESS-Grenzfall, MinidumpWriteDump-Erfolg; keine Behebung des eigentlichen Crashs behaupten.

### P2-COMPAT — PR #305, #300, #73, #72, #290

**Relevanz: mittel bis hoch.** Fahrzeug-Beinkorrektur, drop-ATL, Turretwinkel, Mod-Fernglas/NVG, Desktop-Schließen bei disableUserInput.

**Abnahmetests:** Hang-Einstieg, Partikel ungebunden/objektrelativ, verschiedene Azimute, Legacy-/Custom-Klassen, Eingabe an/aus.

### P2-SMALL — PR #173 Tank-Teil; PR #79 vier Reststellen; PR #284 Logfile-Teil

**Relevanz: mittel.** Tank-Flag initialisieren, Center-Radio-Diagnosen vervollständigen, explizites Logfile optional append.

**Abnahmetests:** Deterministische Konstruktion, Fremdgruppe im Center-/Gruppenkanal, Neustartmarker im selben Logfile.

### P2-TEST — main 02c0d741,7d69cbe3,694f7497; historisch f9057657

**Relevanz: hoch für Messqualität.** LOD-Freeze, simulationszeitbasierte Trident-Tests, echter negativer Up-Vektor; historische Copy-/Langpfad-Tests separat nachprüfen.

**Abnahmetests:** CPU/GPU/Shadow-Lod gemeinsam fixiert, gleiche Sim-Zeit bei unterschiedlicher Rendergeschwindigkeit.

### P3-RNG — PR #172

**Relevanz: hoch als Entwurf, kein Drop-in.** Nur kosmetischen RNG sauber trennen; Explosionsschaden zwingend im Simulationspfad lassen.

**Abnahmetests:** Sim-Hashes mit anderer Kamera/FX-Last, Save/Load, Zuständigkeit/Ownership, Kollisionsanimation.

### P3-PERF — main cc4bd699/3541affa; PR #318; bedingt #95/#105/#111/#112

**Relevanz: messungsabhängig.** Surface-Liste vorfiltern und Karten-Batching messen; numerische und Container-Optimierungen nur nach Vertragstest.

**Abnahmetests:** Sortierties/Alpha/Overlay-Reihenfolge, Marker-Mutationen/Deserialize, float-Grenzen/Aliasing/Architekturen.

### P3-PLATFORM — main Build-/CI-Pakete; PR #92 OpenAL

**Relevanz: mittel, bedingt.** CI/Härtung inkrementell, macOS OpenAL-Bundled-Suche/EFX-Vertrag prüfen.

**Abnahmetests:** Compilerflags, unveränderte WGPU/Trident-Gates, macOS ohne/mit EFX.

## Bereits erledigt / nicht nochmals portieren

Die konkrete Straßenlampen-Korrektur (#310), Mod-Verzeichnis-Aliase (#88), der zentrale zeitbasierte Kartenrand-Scrollpfad (#64) und Shape::Reverse/phase[p] aus #238 sind bereits im aktuellen Ziel vorhanden. ScenePreloader-Reset (#86/main a61130ca) ist historisch im Fork nachgewiesen. Es wird nicht behauptet, dass sämtliche zusätzlichen Testfixtures dieser PRs vorhanden sind.

WGPU-GPU-Skinning, eigener CDLOD-Terrain-/Wasserpfad und der schon früher nachgewiesene Off-map-Ozean begründen keinen weiteren GL33-Port. Große Rendererpakete nur auf davon unabhängige Regeln prüfen. Den alten Off-map-Fund nicht erneut als fehlendes Feature verkaufen.

Bei #173 sind die Head-Floatfelder bereits initialisiert; Time hat einen Null-initialisierenden Konstruktor. Deshalb kein erfundener uninitialisierter Head-Timer. Der Tank-Bitfeld-Default ist ein getrennter, tatsächlich verbleibender Punkt.

## Alle 54 offenen PRs: Einzelbewertung

Legende: **PORT_ADAPTIERT** = konkrete Zielstelle geprüft, klein und passend umsetzen; **TEILWEISE_VORHANDEN** = Hunks aufteilen; **BEDINGT_PRUEFEN** = erst Ziel-/Vertragstest oder Profiling, nicht als fehlender fertiger Fix bestätigt; **KONZEPT_NEU_UMSETZEN** = Idee ja, vorliegendes Paket nein; übrige Status sind wörtlich zu verstehen.

### PR #319 — Add MacOS build docs.

**OPTIONAL · Relevanz: niedrig**

Bauanleitung, kein Laufzeitfix. Nur auf die tatsächlichen WGPU-, Rust-, SDK- und Distributionsvoraussetzungen des Forks zugeschnitten übernehmen; GL33/Metal-Anleitung nicht als OpenPoseidon-Anleitung ausgeben.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/319)

### PR #318 — perf: batch draw map objects by type

**BEDINGT_PRUEFEN · Relevanz: mittel**

Backend-unabhängige Vorbereitung der 2D-Karte, nicht der 3D-Terrainrenderer. Im Ziel gibt es weiterhin den zellenweisen DrawObjects-Aufruf. Der PR gruppiert Kartensymbole nach Typ; das ist nicht automatisch ein einzelner GPU-Draw. Gegen vorhandenes WGPU-2D-Batching messen und Reihenfolge von Symbolen, Beschriftung und Overlays bewahren. Den angegebenen Upstream-FPS-Faktor nicht auf den Fork übertragen.

**Prüfumfang:** PR-Diff und konkret genannte Produktionsstellen am Ziel-SHA verglichen; keine Laufzeittests.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/318)

**Zielstellen:** [`engine/Poseidon/UI/Map/UIMap.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/UI/Map/UIMap.cpp)

### PR #310 — Survive script-created street lamps

**VORHANDEN · Relevanz: kein Produktionsport**

Die drei Produktionskorrekturen sind vorhanden: tatsächliches GetShape() im Konstruktor, Null-Shape-Guard in StreetLampType::InitShape und Ablehnung modellloser Lampen in NewNonAIVehicleQuiet. Den PR nicht nochmals als Crash-Fix implementieren. Vollständige Testfixture-Gleichheit wurde nicht festgestellt.

**Prüfumfang:** PR-Diff und konkret genannte Produktionsstellen am Ziel-SHA verglichen; keine Laufzeittests.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/310)

**Zielstellen:** [`engine/Poseidon/World/Scene/ObjectClasses.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/World/Scene/ObjectClasses.cpp); [`engine/Poseidon/World/Entities/Vehicles/VehicleTypes.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/World/Entities/Vehicles/VehicleTypes.cpp)

### PR #309 — Fix FreeType glyph bounds and text clipping (Issue #97)

**TEST_NUTZEN · Relevanz: mittel**

Descender-Testfälle (g/p/q/y) sind nützlich. Die vorgeschlagene pauschale Erweiterung eines Clip-Rechtecks darf jedoch den übergeordneten UI-Clip nicht aushebeln. Pro-String-Zentrierung kann die Grundlinie zwischen Texten verschieben und kostet erneutes Layout. Kein ungeprüfter Ersatz der eigenen WGPU/FreeType-Positionierung; erst reproduzierbaren Fehler im Ziel nachweisen.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/309)

### PR #305 — Fix leg transform on sloped vehicles

**PORT_ADAPTIERT · Relevanz: mittel bis hoch**

Im Ziel wird die Boden-/Beinkorrektur noch unter if (change) neu berechnet, ohne Ausschluss einer Fahrzeug-Hierarchie. Den Hierarchie-Guard des PR gezielt übernehmen, damit nach dem Einsteigen keine Bodenschräglage in die Sitzanimation zurückgeschrieben wird. Ein-/Aussteigen bergauf und bergab sowie Fahrrad, Schwimmen und die neuen Bewegungszweige testen.

**Prüfumfang:** PR-Diff und konkret genannte Produktionsstellen am Ziel-SHA verglichen; keine Laufzeittests.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/305)

**Zielstellen:** [`engine/Poseidon/World/Entities/Infantry/SoldierOldSim.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/World/Entities/Infantry/SoldierOldSim.cpp)

### PR #300 — Read drop positions as heights above the surface

**PORT_ADAPTIERT · Relevanz: mittel bis hoch**

Die ungebundene drop-Position wird im Ziel weiterhin mit GetVector3 als absolute Position gelesen; die objektgebundene Variante wird dagegen korrekt über PositionModelToWorld umgerechnet. Nur den ungebundenen Zweig auf die Script-Positions-/Höhenkonvention umstellen. Objektrelative Partikel nicht doppelt anheben. Test auf erhöhtem Gelände plus Objekt-Referenz; Terrainloch-/Höhlenverhalten gesondert prüfen.

**Prüfumfang:** PR-Diff und konkret genannte Produktionsstellen am Ziel-SHA verglichen; keine Laufzeittests.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/300)

**Zielstellen:** [`engine/Poseidon/Graphics/Rendering/Effects/Smokes.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/Graphics/Rendering/Effects/Smokes.cpp)

### PR #295 — Gameplay enhancement: Reloading redundancy

**NICHT_PORTIEREN · Relevanz: keine pauschale Übernahme**

Ändert bewusst das Gameplay: Nachladen wird unter anderem bei weniger gefüllten Ersatzmagazinen unterdrückt. Das ist keine allgemeine OFP-Kompatibilitätskorrektur. Zusätzliche Dereferenzierungen von Magazin-/Typdaten im vorgeschlagenen Eingabepfad müssen abgesichert werden. Nicht mit Crash-Guards aus #238 vermischen.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/295)

### PR #294 — Fixes missing button focus/highlight in the key capture conflict confirmation page.

**BEDINGT_PRUEFEN · Relevanz: niedrig bis mittel**

Nur den UI-Fokus-Hunk beurteilen/übernehmen. Der aktuelle PR-Diff enthält zusätzlich die Reload-Gameplay-Änderungen aus #295; daher ausdrücklich kein Whole-PR-Cherry-Pick. Ziel-UI zunächst mit Tastatur und Controller reproduzieren.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/294)

### PR #293 — Fixes ESCAPE key not working in key capturing conflict confirmation page

**BEDINGT_PRUEFEN · Relevanz: mittel**

Ein sinnvoller Escape-Testfall, aber der Patch entfernt auch eine Schutzbehandlung für synthetisches Gamepad-Escape. Physische Escape-Taste und Controller-Capture-Abbruch getrennt testen/implementieren. Kein gesicherter pauschaler Fix für den Zielbranch.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/293)

### PR #290 — Let Alt+F4 quit while a mission has user input disabled

**PORT_ADAPTIERT · Relevanz: mittel**

Die von WGPU verwendete gemeinsame SDLEventWindow.hpp ruft weiterhin ShouldHonorWindowClose nur mit altDown und IsInGameplay auf. disableUserInput wird dort nicht berücksichtigt. Den dritten Zustand in die gemeinsame Fensterlogik integrieren, nicht nur in den GL33-Header. Bei normal aktivierter Eingabe die bisherige Alt+F4-Spielkombination erhalten; bei gesperrter Spieleingabe Desktop-Schließen zulassen.

**Prüfumfang:** PR-Diff und konkret genannte Produktionsstellen am Ziel-SHA verglichen; keine Laufzeittests.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/290)

**Zielstellen:** [`engine/Poseidon/Graphics/Shared/SDLEventWindow.hpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/Graphics/Shared/SDLEventWindow.hpp)

### PR #284 — Keep --log-file across restarts, and stop writing an empty one

**PORT_ADAPTIERT · Relevanz: mittel**

Der explizite logFile-Sink wird im Ziel noch mit basic_file_sink_mt(logFile, true), also Truncate, geöffnet. Append für einen explizit wiederverwendeten Pfad ist sinnvoll; pro Lauf eindeutig benannte Dateien dürfen weiter neu beginnen. Eigene QueuedConsole-/Crash-Diagnostik erhalten. Der zusätzliche PR-Hunk zur Entfernung eines leeren Server-Logs wurde nicht separat gegen die aktuelle Server-App bestätigt.

**Prüfumfang:** PR-Diff und konkret genannte Produktionsstellen am Ziel-SHA verglichen; keine Laufzeittests.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/284)

**Zielstellen:** [`engine/Poseidon/Foundation/Logging/Logging.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/Foundation/Logging/Logging.cpp)

### PR #277 — Fix for Issues #193 and #131: open vehicle sights

**BEDINGT_PRUEFEN · Relevanz: mittel**

Offene Fahrzeugvisiere sollen keine schwarze 4:3-Pillarbox bekommen. Der PR verwendet vorhandene Blend-Sections als Heuristik für optische Masken; diese Gleichsetzung ist bei Mods nicht universell bewiesen. Gegen den eigenen WGPU-Optikpfad und echte offene/geschlossene Visiermodelle testen. Kein bestätigter fehlender Ziel-Fix allein aus dem PR-Titel.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/277)

### PR #265 — Windows temp-path fixes for World state and PreviewImage tests

**TEST_NUTZEN · Relevanz: mittel**

Betrifft World-state-/PreviewImage-Tests und damit andere Dateien als den schon übernommenen PAA-Encoder-Temp-Pfad-Fix. Portabler TestFixtures-Temp-Pfad ist sinnvoll. Aktuelle Zieltests wurden in diesem Lauf nicht vollständig verglichen; deshalb Test-Kandidat, nicht pauschal als fehlend markiert.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/265)

### PR #258 — Add numeric option entry

**OPTIONAL · Relevanz: niedrig**

Praktische direkte Zahleneingabe für Einstellungen. Keine Laufzeit-/Streamingkorrektur. Nur die passenden Controls einpassen, endliche Werte/Bereichsgrenzen und Fokus-/Abbruchverhalten testen. Nicht die eigene Optionsarchitektur überschreiben.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/258)

### PR #250 — Let players keep night in color

**NICHT_PORTIEREN · Relevanz: niedrig**

Der konkrete Eingriff steuert die Legacy-/GL33-Nachtsicht-Entsättigung. WGPU hat eine eigene HDR-/Look-Pipeline; einen funktionslosen Legacy-Schalter nicht übernehmen. Allenfalls eine bewusst entworfene WGPU-Look-Option, kein Kompatibilitätsfix.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/250)

### PR #239 — Fix UB in DisplayMultiplayer::BeginModdedJoin

**PORT_ADAPTIERT · Relevanz: hoch**

Bestätigter C++20-Lifetime-Fall: der Range-for verwendet ActiveModsFromMountPath(...).All(); All liefert eine Referenz auf den Inhalt einer temporären ModCollection. Die Collection vor der Schleife in einer benannten lokalen Variable halten. Kein Netzwerkformat ändern. Kleine Korrektur mit Mod-Join-/Lifetime-Test.

**Prüfumfang:** PR-Diff und konkret genannte Produktionsstellen am Ziel-SHA verglichen; keine Laufzeittests.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/239)

**Zielstellen:** [`engine/Poseidon/UI/DisplayUIMultiplayer.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/UI/DisplayUIMultiplayer.cpp); [`engine/Poseidon/Core/ModCollection.hpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/Core/ModCollection.hpp); [`CMakeLists.txt`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/CMakeLists.txt)

### PR #238 — TZK CWA-CE 2.03 crash fixes

**TEILWEISE_VORHANDEN · Relevanz: hoch für fehlende Guards**

Gemischtes Paket. Shape::Reverse verwendet im Ziel bereits phase[p]; diesen Hunk nicht erneut ändern. ReloadMagazine/AutoReload greifen dagegen weiter ohne vollständige Bereichsprüfung auf GetMagazineSlot zu. Index >= 0 UND < NMagazineSlots, Null-Muzzle/Typ und Lebenszeit vor Zugriff/Nebenwirkungen prüfen. Die vorgeschlagene Antwort GetWeaponLoaded(invalid)==true nicht blind übernehmen. Weitere UI-/AI-Null-Guards einzeln einpassen; keine ganze Fahrzeug-/Scriptdatei ersetzen.

**Prüfumfang:** PR-Diff und konkret genannte Produktionsstellen am Ziel-SHA verglichen; keine Laufzeittests.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/238)

**Zielstellen:** [`engine/Poseidon/AI/VehicleAICombat.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/AI/VehicleAICombat.cpp); [`engine/Poseidon/AI/EntityAI.hpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/AI/EntityAI.hpp); [`engine/Poseidon/Graphics/Rendering/Shape/ShapeFile.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/Graphics/Rendering/Shape/ShapeFile.cpp)

### PR #237 — Add NaN check in AIUnit::IsSimplePath

**PORT_ADAPTIERT · Relevanz: hoch**

Der aktuelle IsSimplePath-Pfad lehnt nur große XZ-Abstände ab und prüft Endpunkte nicht auf Endlichkeit, bevor Koordinaten in Raster-/Map-Berechnungen fließen. Nicht nur _isnan(pos) kopieren: from UND pos mit std::isfinite prüfen, Inf und unvertretbar große endliche Koordinaten vor Konversion und PerformUnlock abweisen. Das ist ein Schutz gegen Hänger, kein Nachweis, dass der Erzeuger falscher Koordinaten damit behoben wird.

**Prüfumfang:** PR-Diff und konkret genannte Produktionsstellen am Ziel-SHA verglichen; keine Laufzeittests.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/237)

**Zielstellen:** [`engine/Poseidon/AI/AIUnit.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/AI/AIUnit.cpp)

### PR #210 — AISubgroupFSM: drop invalid assignment assert in GetIn point helpers

**BEDINGT_PRUEFEN · Relevanz: niedrig bis mittel**

Queued-GetIn-Befehl und inzwischen aufgehobene Zuordnung können legitimerweise auseinanderlaufen. Die Diagnose soll echte Unit-/Vehicle-Voraussetzungen prüfen und nicht diesen Zwischenzustand verbieten. Ablehnung zerstörter/falscher Fahrzeuge in CheckGetInDone unbedingt erhalten; kein pauschales Entfernen funktionaler Prüfungen. Noch kein vollständiger Zielcode-Abgleich dieses FSM-Hunks.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/210)

### PR #209 — AIGroupCmd: keep static-weapon crew assigned in AssignVehicles

**PORT_ADAPTIERT · Relevanz: hoch**

Im Ziel hängt der Settled-Skip weiterhin an beweglichem Fahrzeug plus zugewiesenem Fahrer. Bei statischen Waffen fehlt dieser Fahrer grundsätzlich. Einen Soldaten, der bereits seinen zugewiesenen Sitz besetzt, aus der Neuverteilung herausnehmen. Zwei gruppierte MGs, ein getöteter Schütze, zerstörtes Fahrzeug und bewusster Ausstiegsbefehl testen. Die neuen Malprave-AI-Änderungen des Forks erhalten.

**Prüfumfang:** PR-Diff und konkret genannte Produktionsstellen am Ziel-SHA verglichen; keine Laufzeittests.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/209)

**Zielstellen:** [`engine/Poseidon/AI/AIGroupCmd.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/AI/AIGroupCmd.cpp)

### PR #208 — Add unit loadout scripting commands

**KONZEPT_NEU_UMSETZEN · Relevanz: mittel für spätere Kompatibilität**

Der vorgeschlagene kompakte Drei-Teile-Datensatz ist nicht automatisch das ArmA-3-getUnitLoadout-Format. Außerdem darf fehlerhafte Eingabe nicht erst nach dem Löschen vorhandener Ausrüstung auffallen. Versioniertes/kompatibles Format und vollständige Vorabvalidierung entwerfen; MP-Locality, geladene Magazine und Munition mit Roundtrip-Tests sichern. Kein direkter Kompatibilitätsport.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/208)

### PR #178 — GL33 GPU skinning for infantry view LODs

**NICHT_PORTIEREN · Relevanz: kein neuer Rendererpfad**

Der konkrete PR fügt einen GL33-Bone-Palette-Pfad hinzu. Der Fork besitzt bereits eigenes WGPU-GPU-Skinning und eigene Mesh-/Upload-Lebenszeiten. Nur die Trennung visueller und CPU-gelesener Kollisions-/Memory-Geometrie als Testregel behalten. Kein GL33-GPU-Skinning nachrüsten.

**Prüfumfang:** PR-Diff plus zuvor nachgewiesene WGPU-Architektur; keine erneute vollständige Skinning-Pfadvalidierung dieses Branches.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/178)

**Zielstellen:** [`engine/WgpuRenderer/EngineWgpu.hpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/WgpuRenderer/EngineWgpu.hpp)

### PR #173 — Fix uninitialised per-entity heap fields read during simulation

**TEILWEISE_VORHANDEN · Relevanz: mittel**

Tank::_doGearSound ist im aktuellen Header ein Bitfeld ohne Initialisierung; der Konstruktor setzt es ebenfalls nicht. Diesen kleinen false-Default ergänzen. Die Head-Floatfelder sind schon initialisiert. Besonders wichtig: Time/AbstractTime wird im Ziel standardmäßig mit 0 konstruiert, also ist der fehlende explizite Head-Timer-Initializer hier KEIN Beweis für einen uninitialisierten Timer. Eine TIME_MAX-Ruhephase wäre eine getrennte Verhaltens-/RNG-Entscheidung, nicht derselbe UB-Fix.

**Prüfumfang:** PR-Diff und konkret genannte Produktionsstellen am Ziel-SHA verglichen; keine Laufzeittests.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/173)

**Zielstellen:** [`engine/Poseidon/World/Entities/Vehicles/Ground/Tank.hpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/World/Entities/Vehicles/Ground/Tank.hpp); [`engine/Poseidon/World/Entities/Vehicles/Ground/Tank.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/World/Entities/Vehicles/Ground/Tank.cpp); [`engine/Poseidon/World/Entities/Infantry/Head.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/World/Entities/Infantry/Head.cpp); [`engine/Poseidon/Foundation/Time/Time.hpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/Foundation/Time/Time.hpp)

### PR #172 — Separate cosmetic-effect RNG from the simulation RNG stream

**KONZEPT_NEU_UMSETZEN · Relevanz: hoch als gezielte Architekturarbeit**

Im Ziel konsumieren sichtbarkeits-/kameragesteuerte Rauchpartikel weiterhin GRandGen. Die Trennung rein visueller Zufallswerte ist sinnvoll. Der tatsächliche PR verschiebt aber auch SmokeSourceVehicle::SimulateExplosion: dessen randomFactor bestimmt hit/indirectHit, also SPIELSCHADEN. Diesen Hunk keinesfalls auf einen ungesicherten FX-Stream verschieben. Eigene RNG-Thread-/Zählerregeln bewahren; reine FX, Audio/UI und simulationswirksame Animation/Kollision unterscheiden. Gleiches Sim-Ergebnis mit anderer Kamera/FX-Dichte, Save/Load und Server/Client getrennt testen.

**Prüfumfang:** PR-Diff und konkret genannte Produktionsstellen am Ziel-SHA verglichen; keine Laufzeittests.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/172)

**Zielstellen:** [`engine/Poseidon/Graphics/Rendering/Effects/Smokes.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/Graphics/Rendering/Effects/Smokes.cpp); [`engine/Random/randomGen.hpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Random/randomGen.hpp)

### PR #168 — Fix issues while running macOS tests with sanitizers

**PORT_ADAPTIERT · Relevanz: hoch**

Renderer-unabhängige Teilfixes: HarnessServer::Stop wartet im Ziel begrenzt und setzt dann _running=false ohne gesicherten Thread-Join; NetPeer hält Listening/Sending-Flags als plain bool; PoCritical schreibt Status in gemeinsamem Zustand vor Lock/nach Unlock. Thread-Lebenszeit, Sichtbarkeit und fehlerfreie Diagnose separat absichern. Ein bloßes join() ist unzureichend, solange Socket-/Antwort-Wartepfade nicht abbrechbar sind. Test-Assertions aus Worker-Threads auf den Test-Thread verlagern. Keine sanitizerfreie WGPU-Laufzeit behauptet.

**Prüfumfang:** PR-Diff und konkret genannte Produktionsstellen am Ziel-SHA verglichen; keine Laufzeittests.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/168)

**Zielstellen:** [`engine/Poseidon/Dev/Harness/HarnessServer.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/Dev/Harness/HarnessServer.cpp); [`engine/Poseidon/Network/Legacy/NetPeer.hpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/Network/Legacy/NetPeer.hpp); [`engine/Poseidon/Foundation/Threads/PoCritical.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/Foundation/Threads/PoCritical.cpp)

### PR #112 — perf: SSE Vector3P::SetFastTransform

**BEDINGT_PRUEFEN · Relevanz: mittel, nur mit Messung**

SSE-Transform ist grundsätzlich backend-unabhängig. 12-Byte-Vector3P-Layout, Alias-Fall und genaue Rechenreihenfolge beachten; kein 16-Byte-Überlesen des letzten Feldes. Zuerst aktuellen Zielcode/Compileroutput messen und CPU- sowie plattformübergreifende numerische Tests ausführen. PR-Messung ist kein gemessener Fork-Gewinn.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/112)

### PR #111 — perf: gate heavy AI validators out of release builds

**BEDINGT_PRUEFEN · Relevanz: mittel, nur mit Messung**

Teure Strukturprüfungen sind ein möglicher CPU-Kandidat, aber NDEBUG-Abschaltung darf Debug-/Test-/Diagnose-Builds nicht entkernen. Neue AI-Portierung und Streaming-Lebenszeiten machen Validatoren gerade wertvoll. Erst Aufrufkosten im aktuellen Fork messen; dann explizite Diagnose-Policy statt wahlloser Assertion-Entfernung.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/111)

### PR #110 — Fix --benchmark on POSIX

**BEDINGT_PRUEFEN · Relevanz: mittel**

Boot-/Pfadkorrekturen für den Legacy-Benchmark sind brauchbar, sofern dieser CLI-Pfad noch verwendet wird. Eigene Benchmark-/Trident-Abläufe nicht ersetzen. Vier Sekunden Warm-up sind insbesondere mit dem neuen Streaming kein Beweis für einen stabilen residenten Zustand. No-op-/leere-Szene-Benchmarks durch Zustandskriterien verhindern.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/110)

### PR #106 — MathOpt: use SSE rsqrtss for InvSqrt on x86

**NICHT_PORTIEREN · Relevanz: höchstens separates Experiment**

Ändert die numerische Approximation und lässt Nicht-x86 auf einem anderen Pfad. Kein vermeintlich kostenloser, determinismusneutraler Port für Physik/AI. Nur mit expliziter Rechen-/Cross-Platform-Policy und Messung untersuchen; nicht mit Sicherheitsfixes bündeln.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/106)

### PR #105 — FltOpts: cvtss2si instead of nearbyint

**BEDINGT_PRUEFEN · Relevanz: mittel, numerisch sensibel**

Im Ziel stehen weiterhin std::nearbyint und eigene Sättigungen vor int-Konversion. Beschleunigung begrenzter float→int-Operationen kann sinnvoll sein. Der PR darf jedoch fastRound(float) nicht über einen 32-Bit-int-Zwischenschritt für alle Eingaben ersetzen: große Werte, NaN/Inf und bisherige Clamps/Float-Rückgabe sind andere Semantik. Compileroutput zuerst prüfen, exakte Grenzfalltests, kein pauschaler FPS-Faktor.

**Prüfumfang:** PR-Diff und konkret genannte Produktionsstellen am Ziel-SHA verglichen; keine Laufzeittests.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/105)

**Zielstellen:** [`engine/Poseidon/Foundation/Common/FltOpts.hpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/Foundation/Common/FltOpts.hpp)

### PR #102 — Options: Replace controls reset with presets page

**OPTIONAL · Relevanz: niedrig**

Bedienkomfort, kein Engine-Bugfix. Presets über stabile Aktionsnamen pflegen, nicht blind über parallel zur Enum liegende Arrays; der Fork hat zusätzliche/geänderte Aktionen. Kein Zurücksetzen vorhandener Nutzerprofile ohne eindeutige Bestätigung. UI-Tests und Controller-Capture berücksichtigen.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/102)

### PR #95 — Modify markersMap with an extra hash-map

**BEDINGT_PRUEFEN · Relevanz: mittel für CTI**

Hash-Lookup kann viele CTI-Markerzugriffe beschleunigen. Der Patch braucht aber eine vollständige Invalidation-/Mutationsprüfung: Deserialize/Update vorhandener Marker, Umbenennen, Löschen, Clear, doppelte Namen und erste/letzte Treffersemantik. Nicht als nachgewiesen fehlende Beschleunigung oder Drop-in-Port ausgeben; aktuellen Container vollständig auditieren und messen.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/95)

### PR #93 — Use _this for UserAction caller

**BEDINGT_PRUEFEN · Relevanz: mittel für Mods**

Interessante Script-Erweiterung: this bleibt Ziel, _this liefert Aufrufer. Vor Übernahme den aktuellen CfgUserActions-Auswertungspfad und Variablenscope vergleichen, MP-Locality und null/wechselnden Caller prüfen. Keine Gleichsetzung mit dem addAction-Argumentarray.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/93)

### PR #92 — Add a native Metal renderer for macOS

**TEILWEISE_VORHANDEN · Relevanz: hoch für PAC-Teil; mittel für macOS-Audio**

KEIN zusätzlicher Metal-Renderer. Wertvoller nicht-rendererspezifischer Teil: Bei formatmarkierten .pac-Dateien muss der Header den PAA-Level-Decoder wählen, nicht die Endung. Das fehlt im Ziel sowohl in DecodePAABuffer/DecodePAAFileMip als auch TextureSourcePac::Init. Achtung: _isPaa entscheidet im Fork zusätzlich über Streaming-/ArchiveSourceBinding-Handoffs und RetailPacReadScope; Decoderformat und Provenance nicht versehentlich vermischen. AI88-8-Bit-Erhalt und LZO-DXT sind im Fork schon selbst gelöst: bewahren, keinen Decoder-Großrefactor darüberkopieren. Zweiter Kandidat: Apple-OpenAL lädt noch nur libopenal.dylib und verlangt sämtliche EFX-Symbole. Bundled-Library-Suche und optionales EFX gezielt prüfen/portieren, nur bei abgesicherten Callern. Testfixture für LZSS-AI88-.pac übernehmen/adaptieren.

**Prüfumfang:** PR-Diff und konkret genannte Produktionsstellen am Ziel-SHA verglichen; keine Laufzeittests.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/92)

**Zielstellen:** [`engine/Poseidon/Graphics/Textures/PAADecoder.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/Graphics/Textures/PAADecoder.cpp); [`engine/Poseidon/Graphics/Rendering/Font/Pactext.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/Graphics/Rendering/Font/Pactext.cpp); [`engine/PoseidonOpenAL/OpenALRuntime.hpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/PoseidonOpenAL/OpenALRuntime.hpp)

### PR #88 — QIFStreamB: try mod dirs when loose-file open fails on POSIX

**VORHANDEN · Relevanz: kein neuer Port**

ResolveModRootAlias und ModSystem::EnumDirectories existieren im aktuellen QBStream. Sowohl AutoOpen als auch FileExist benutzen den Alias-Fallback, zusätzlich nach CollapseParentDirs. Nicht erneut implementieren; keine alte PBO-/Streamingdatei darüberkopieren.

**Prüfumfang:** PR-Diff und konkret genannte Produktionsstellen am Ziel-SHA verglichen; keine Laufzeittests.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/88)

**Zielstellen:** [`engine/Poseidon/IO/Streams/QBStream.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/IO/Streams/QBStream.cpp)

### PR #86 — UnloadGameData: reset ScenePreloader on re-mount

**VORHANDEN · Relevanz: kein neuer Port**

Bereits im Main-Audit durch a61130ca und im Fork nachgewiesen. Die offene PR-Nummer bedeutet nicht fehlende Implementierung. Historischen Nachweis bewahren; dieser Lauf validiert nicht erneut jedes Remount-Testfixture.

**Prüfumfang:** PR-Diff plus historischer Zielcode-Nachweis (a61130ca); kein frischer vollständiger Remount-Lauf.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/86)

**Zielstellen:** [`engine/Poseidon/Foundation/Platform/Shutdown.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/Foundation/Platform/Shutdown.cpp)

### PR #85 — Guard null preloaded CraterShell

**TEST_NUTZEN · Relevanz: mittel**

Die unmittelbar gefährliche BoundingCenter-Dereferenz ist im Ziel bereits mit shape ? ... : VZero abgesichert. Null-shape-Crater-Erzeugung bleibt ein gezielter Test-/Robustheitspunkt; ohne Konstruktor-/Laufzeitnachweis kein bestätigter Rest-Crash. Bei fehlendem Kosmetikmodell ausschließlich Kosmetik überspringen, AI Disclose sowie eigene Wasser-/Kraterpfade nicht durch einen frühen Return unterdrücken.

**Prüfumfang:** PR-Diff und konkret genannte Produktionsstellen am Ziel-SHA verglichen; keine Laufzeittests.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/85)

**Zielstellen:** [`engine/Poseidon/World/Simulation/Collisions.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/World/Simulation/Collisions.cpp)

### PR #84 — Rotate markers and Alt-big steps

**OPTIONAL · Relevanz: niedrig**

Editor-/Kartenkomfort. Wrap-/Modulo-Schleifen brauchen einen sicheren Empty-Config-Fall; bei null Einträgen darf die Wrap-Schleife nicht endlos laufen. Kein Laufzeit-Sicherheitsport des gesamten PR und keine automatische Änderung bestehender Markerrichtung.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/84)

### PR #82 — VehicleSupply::SupplyStarted: drop duplicate-entry assert

**BEDINGT_PRUEFEN · Relevanz: niedrig**

PR beseitigt eine Diagnose, die legitimer Supply→Move→Supply-Wiederkehr widerspricht; AddUnique trägt bereits Idempotenz. Nicht als Reparatur der Versorgungs-Gameplaylogik verkaufen. Mit kurzem Reentry-Test übernehmen, sofern der aktuelle Ziel-Assert noch existiert.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/82)

### PR #81 — Man::KilledBy: null-brain corpses

**BEDINGT_PRUEFEN · Relevanz: niedrig bis mittel**

Respawn kann dem alten Körper den Brain entziehen; vorhandener früher Null-Return macht den davorstehenden Assert fragwürdig. Zusätzliche Transport-Dispatch-Guards gezielt prüfen. Eigene Tod-/Ragdoll-/Respawn-Nebenwirkungen nicht versehentlich überspringen; kein pauschales Vehicle-Damage-Refactoring.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/81)

### PR #80 — Object setVariable/getVariable

**KONZEPT_NEU_UMSETZEN · Relevanz: mittel für spätere Kompatibilität**

Ein Objekt-Variablenspeicher ist nützlich, aber der PR ist kein vollständiger ArmA-Vertrag: Save/Load, Netzwerk-public-Parameter, Defaults/nil, Namens-/Scope-Regeln und Objekt-Lebenszeiten brauchen eigene Spezifikation und Tests. Nur als Entwurf verwenden, nicht als vollständige Kompatibilität deklarieren.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/80)

### PR #79 — Skip foreign-sender check on shared center radio

**TEILWEISE_VORHANDEN · Relevanz: niedrig bis mittel**

Im Ziel schützt ReportForeignRadioCommand schon zwei Prüfstellen vor legitimen fremden Gruppen am Center-Kanal. Die zwei anderen CommandSent-Überladungen enthalten noch vier ungeguardete AI_ERROR(msgCmd->GetFrom()==this). Bestehenden Helper konsistent verwenden; eigener Gruppenkanal bleibt geprüft. Diagnose-/Logqualität, kein Beweis einer bislang falschen Zielzuordnung.

**Prüfumfang:** PR-Diff und konkret genannte Produktionsstellen am Ziel-SHA verglichen; keine Laufzeittests.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/79)

**Zielstellen:** [`engine/Poseidon/AI/AIGroupImpl.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/AI/AIGroupImpl.cpp)

### PR #78 — EnableAI and checkAIFeature commands

**KONZEPT_NEU_UMSETZEN · Relevanz: mittel**

Die Feature-Abfrage im Patch interpretiert das Disabled-Bit in Gegenrichtung zur beschriebenen Enabled-Semantik. Vor Übernahme Vertrag für enableAI/disableAI/checkAIFeature und unbekannte Feature-Namen klären, Roundtrip testen. Befehle nicht nur registrieren und als fertig melden.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/78)

### PR #77 — Expose LOAD MAGAZINE action to scripts

**BEDINGT_PRUEFEN · Relevanz: mittel**

Sinnvolle Script-Steuerung, aber der Patch muss leere/zu kurze Argumentarrays vor Array[0] prüfen. Alte Aktionsschreibweise nicht durch neue ungetrennte Schreibweise ersetzen, sondern bewusst Alias-/Kompatibilitätsregeln erhalten. Mit #238-Indexprüfung koordinieren.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/77)

### PR #76 — Add Ctrl state to onMapSingleClick

**OPTIONAL · Relevanz: niedrig bis mittel**

Kleine Mod-/UI-Erweiterung. Beide Ctrl-Tasten sowie das bestehende Argument-/Scope-Verhalten berücksichtigen; die PR-Variante betrachtet nur die linke Taste. Kein Engine-Kernfix.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/76)

### PR #75 — pressingKey scripting command

**OPTIONAL · Relevanz: niedrig**

Community-spezifischer Befehl, kein allgemein nachgewiesener ArmA-Kompatibilitätsbaustein. SDL-/Legacy-Keycodes und Name/Index-Grenzen sauber abbilden, nicht Tabellen fremder Input-Implementierungen über den Fork legen.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/75)

### PR #73 — Fix getTurretDirAndElev angle

**PORT_ADAPTIERT · Relevanz: mittel**

Die Elevationsberechnung verwendet im Ziel noch abs(dir.X()) als horizontalen Anteil und hängt dadurch vom Azimut ab. Der vollständige XZ-Anteil ist nötig. Robuster Vorschlag atan2(y, hypot(x,z)) statt ungeprüfter sqrt(1-y*y)-Annahme. Gleiche Elevation bei verschiedenen Himmelsrichtungen sowie senkrechte/degenerierte Vektoren testen. Script-Ausgabe, nicht pauschal Fahrzeug-Aim-Physik.

**Prüfumfang:** PR-Diff und konkret genannte Produktionsstellen am Ziel-SHA verglichen; keine Laufzeittests.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/73)

**Zielstellen:** [`engine/Poseidon/Game/Commands/GameStateExtGrp.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/Game/Commands/GameStateExtGrp.cpp)

### PR #72 — Support isBinocular/isNVG weapon parameters

**PORT_ADAPTIERT · Relevanz: mittel**

Bestätigt: IsBinocular verlangt im Ziel noch den Literalnamen binocular und ScanNVG sucht ausschließlich NVGoggles. Die Config-Flags mit Legacy-Fallback ergänzen; vorhandenen FindBinocularWeapon-Fallback erhalten. Anzeige/Optik muss anschließend zur tatsächlich ausgerüsteten Klasse passen; leere Muzzle-/Optikmodelle absichern.

**Prüfumfang:** PR-Diff und konkret genannte Produktionsstellen am Ziel-SHA verglichen; keine Laufzeittests.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/72)

**Zielstellen:** [`engine/Poseidon/World/Entities/Weapons/Weapons.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/World/Entities/Weapons/Weapons.cpp); [`engine/Poseidon/World/Entities/Infantry/SoldierOldSim.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/World/Entities/Infantry/SoldierOldSim.cpp)

### PR #64 — Make map edge scrolling frame-rate independent

**VORHANDEN · Relevanz: kein neuer Port**

Im Ziel gibt es bereits ScrollOnEdges mit uiTime-Delta und Referenz-Zeitschritt; DisplayMap ruft den Helper bereits auf. Den Hauptfix nicht noch einmal implementieren. Die vollständige Abdeckung aller Wizard-/Editor-Callsites wurde hier nicht erneut festgestellt.

**Prüfumfang:** PR-Diff und konkret genannte Produktionsstellen am Ziel-SHA verglichen; keine Laufzeittests.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/64)

**Zielstellen:** [`engine/Poseidon/UI/Map/UIMap.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/UI/Map/UIMap.cpp); [`engine/Poseidon/UI/Map/UIMapDisplayBriefing.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/UI/Map/UIMapDisplayBriefing.cpp)

### PR #51 — FreeBSD support

**OPTIONAL · Relevanz: niedrig ohne FreeBSD-Ziel**

Plattformarbeit, kein WGPU-Feature. Eigene Abhängigkeiten, Allocator-/Sanitizer-Kontrakte und inzwischen zusätzliche Physik-/Scriptkomponenten berücksichtigen. Nicht für einen SDK-Buildfix die globale Allokator-Instrumentierung unkontrolliert ändern.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/51)

### PR #49 — Add selectPlayer scripting command

**BEDINGT_PRUEFEN · Relevanz: mittel für Scripts**

Im gelesenen GameStateExtGrp-Pfad gibt es Player/CameraOn, aber keine entsprechende SelectPlayer-Implementierung; das ist noch kein Vollbeweis über alle Registrierungsdateien. Spielerwechsel muss Eingabe, Kamera, Gruppe, Besitz/Locality, Respawn und Save/Load zusammen behandeln. Nicht bloß drei Weltzeiger umsetzen und vollständige MP-Unterstützung behaupten.

**Prüfumfang:** PR-Diff plus partieller Zielcode-Abgleich; globale Befehlsabwesenheit nicht bewiesen.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/49)

**Zielstellen:** [`engine/Poseidon/Game/Commands/GameStateExtGrp.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/Game/Commands/GameStateExtGrp.cpp)

### PR #10 — Refactor network messages to macros

**NICHT_PORTIEREN · Relevanz: keine pauschale Übernahme**

Großer Refactor über viele Nachrichtentypen ohne unmittelbaren WGPU-Nutzen. Feldreihenfolge, Indices, Vererbung und Seitentabellen sind Wire-/Save-Kompatibilitätsrisiken; der harte Seitenzähler im Diff braucht besondere Prüfung. Erst Protokoll-/Golden-Packet-Tests, nicht mit Gameplay-Crashfixes bündeln.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/10)

### PR #6 — Status Quo multiplayer fixes

**BEDINGT_PRUEFEN · Relevanz: mittel bis hoch bei reproduziertem MP-Fehler**

Gunner-/Commander-turnIn/turnOut und Zerstörungsrauch sind relevante Multiplayer-Semantik. Der PR ergänzt/ändert jedoch Netzwerk-Nachrichten und ist ausdrücklich wire-inkompatibel zu ungepatchten Builds. Zuerst aktuellen Fork-Transferpfad, Versions-/Capability-Gate und Late-Join testen; keinen Komplettport als harmlose Grafikreparatur empfehlen.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/6)

### PR #3 — Status Quo simple fixes

**BEDINGT_PRUEFEN · Relevanz: mittel, pro Hunk**

Mehrere unabhängige Themen: Votes bei Disconnect, manuelles Dauerfeuer, Fernglaswahl, Kompassrichtung, Hülsen bei hohen FPS und Zielauswahl gruppenfremder Gunner. Nicht als ein Paket übernehmen. Der Fork hat bereits eigene Fernglas-/AI-/Physikänderungen; tatsächliche Lücken pro Hunk nachweisen. Testideen nützlich, aber kein behaupteter vollständiger aktueller Fehlbestand.

**Prüfumfang:** PR-Diff gelesen; noch kein belastbarer vollständiger Zielcode-Abgleich – kein bestätigter fehlender Fix.

[Upstream-PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/3)

## Alle 102 bisher auditierten Main-Commits

**Wichtig:** Die historischen Originalbewertungen sind absichtlich nicht neu etikettiert. Der Eintrag „HISTORISCH_UEBERNOMMEN“ bedeutet, dass der alte Nachweis erhalten ist, nicht dass diese Stelle am neuen Branch nochmals vollständig geprüft wurde. Aktuelle Ergänzungen stehen jeweils separat darunter. 102 ist kein Zähler fehlender Fixes.

### 001. `e4a18a10` — Ref move constructors

**Historische Einordnung:** VORHANDEN

Ref<T> Move-Konstruktor und -Zuweisung identisch vorhanden.

**Historische Evidenz:** ["turn59file0", "turn60file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/e4a18a10b048c72a6a0b14f199cc8003a8bca69d)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/Foundation/Types/Pointers.hpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 002. `d8870c11` — Ref move tests

**Historische Einordnung:** VORHANDEN

Alle vier Ref-Move-Testfälle vorhanden.

**Historische Evidenz:** ["turn61file0", "turn62file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/d8870c1170d97a7eb3f6ea71b8ead5bb28f1a107)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/tests/unit/engine/Poseidon/Foundation/Types/test_pointers.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 003. `d7a89671` — Linux ARM64

**Historische Einordnung:** TEILWEISE_OPTIONAL

Netzwerk-Signed-Char und PAC/PAA-Prüfsummenfix vorhanden. ARM64-spezifische Sanitizer-Toolchain fehlt; Buildausweitung nur bei Bedarf.

**Historische Evidenz:** ["turn63file0", "turn64file0", "turn65file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/d7a89671bccbfd252fb0904a5f3b48ab9a716362)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/Network/NetworkMsgFormat.hpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/Graphics/Rendering/Font/Pactext.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 004. `6bc729ab` — Sanitizer builds fix

**Historische Einordnung:** OPTIONAL

Nur Ergänzung der ARM64-Sanitizer-Toolchains. Diese sind im Ziel nicht vorhanden; GL33-RTTI-Workaround nicht blind übernehmen.

**Historische Evidenz:** ["turn66file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/6bc729ab17799cb1b20b8d55ce9e82a91a80e7bf)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/tree/f4ffc5106745d0491489f52108bf93fa5658a6ff/cmake/toolchains)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 005. `694f7497` — ARM64 signed-char tests

**Historische Einordnung:** TEST_ERGAENZEN

Rot8b-Fälle samt CMake und PAA-Signed-Checksum-Test vorhanden. Präziser negativer Up-Vektor-Test fehlt: Ziel nutzt noch SetRotationY(PI); Up.Z bleibt dabei 0. Nur gezielten Test ergänzen, Produktivfix bereits vorhanden.

**Historische Evidenz:** ["turn67file0", "turn68file0", "turn69file0", "turn70file0", "turn71file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/694f749724a9264ad83e4eb34da5e1ebd7d86caa)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/tests/unit/engine/Poseidon/Network/test_encoded_matrix3.cpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/tests/unit/engine/Poseidon/Network/test_rot8b.cpp); [Beleg 3](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/tests/unit/engine/Poseidon/Graphics/Rendering/test_pactext.cpp)

**Prüfung 2026-10-04: AKTUELL_GEPRUEFT.** Der Negative-Up-Test verwendet weiterhin SetRotationY(H_PI); damit wird der negative Up-Vektor nicht erzeugt. Explizite Vektoren nötig. Die übrigen Tests dieses Commits wurden nicht sämtlich neu verglichen.

**Aktuelle Zielbelege:** [`tests/unit/engine/Poseidon/Network/test_encoded_matrix3.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/tests/unit/engine/Poseidon/Network/test_encoded_matrix3.cpp)

### 006. `8f3de01c` — Cached sort keys and radix sort

**Historische Einordnung:** VORHANDEN

Packed-key-Sortierung und gemeinsamer Radixsort in SceneDraw; Algorithmus und Tests vorhanden.

**Historische Evidenz:** ["turn72file0", "turn74file0", "turn75file0", "turn76file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/8f3de01c8a7b7a24d7bacb2f74491abc40066125)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/World/Scene/SceneDraw.cpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/Foundation/Algorithms/RadixSort.hpp)

**Prüfung 2026-10-04: AKTUELL_GEPRUEFT.** Aktueller SceneDraw-Pfad besitzt eigene Packed-Key-/Radix-Vorbereitung. Der gesamte alte Commit wurde nicht erneut bitweise validiert.

**Aktuelle Zielbelege:** [`engine/Poseidon/World/Scene/SceneDraw.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/World/Scene/SceneDraw.cpp)

### 007. `775a2625` — Land-clipped animated bounds cache

**Historische Einordnung:** VORHANDEN

AnimBBox-Cache, Beschädigungsausschluss und beide Move-Invalidierungen vorhanden; erweitert auf ForestPlain.

**Historische Evidenz:** ["turn77file0", "turn80file0", "turn84file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/775a26259b59a2ec6653c457e705f4fd522fdaf0)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/World/Scene/Object.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 008. `90f90ec4` — Land clip method extraction

**Historische Einordnung:** NICHT_PORTIEREN

Reines Aufteilen von LandClip-Funktionen; Ziel verwendet eigene Conform-State-Implementierung und behält CPU-Restore korrekt nur bei CPU-Deformation. Kein fehlendes Verhalten.

**Historische Evidenz:** ["turn78file0", "turn84file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/90f90ec4dde16e3d85e444ac74538720c826c4fb)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/World/Scene/Object.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 009. `816e615d` — Radix sort stability

**Historische Einordnung:** VORHANDEN

Invertierte Schlüssel statt rückwärts auslesen; Equal-Key-Stabilitätstest vorhanden.

**Historische Evidenz:** ["turn73file0", "turn74file0", "turn75file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/816e615ddbc759dc17afe8080dccdec5af306293)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/Foundation/Algorithms/RadixSort.hpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/tests/unit/engine/Poseidon/Foundation/Algorithms/test_radixsort.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 010. `c8229780` — Land-clipped bounds cache tests

**Historische Einordnung:** VORHANDEN

Cache-/Schaden-Test vorhanden und um korrekte Allokation des größeren Testobjekts angepasst.

**Historische Evidenz:** ["turn79file0", "turn81file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/c8229780a404e7ba49d86e33e8fc38165834f6ff)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/tests/unit/engine/Poseidon/World/test_object_animated_bounds.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 011. `406aaff0` — Small discardable buffers not dynamic

**Historische Einordnung:** NICHT_PORTIEREN

Einzige Änderung betrifft VertexBufferGL33::_dynamic; kein WGPU-Code.

**Historische Evidenz:** ["turn85file0"]

**Historischer Prüfumfang:** Geänderte Module und Diff auf WGPU-/Engine-Relevanz eingeordnet

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/406aaff09eb655d7dc1263dacf72ed31691d1636)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 012. `03413fe9` — Remove discardable vertex buffer types

**Historische Einordnung:** NICHT_PORTIEREN

Upstream entfernt Discardable-VB-Enums. Ziel nutzt VBBigDiscardable und eigene VB-Typ-/Lifetime-Verwaltung weiterhin. Im Streaming-/Residency-Branch ist das kein nützlicher Fix; nur bei bewusstem späterem API-Aufräumen erwägen.

**Historische Evidenz:** ["turn86file0", "turn191file0", "turn208file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/03413fe91ca3c2fc0087c1f136fec4d7a39b4092)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/WgpuRenderer/EngineWgpu.hpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/World/Terrain/Landscape.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 013. `a9cdfc29` — Map mouse-wheel bindings

**Historische Einordnung:** VORHANDEN

Mausradgerätecode und profilabhängiges Kartenzoom vorhanden; Migration belegt vorhandene freie Slots ohne Overrides.

**Historische Evidenz:** ["turn87file0", "turn90file0", "turn93file0", "turn94file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/a9cdfc2907dfb87662102f7de36c453fe08d0d16)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/Input/InputSubsystem.cpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/UI/Settings/ContextControlsConfig.cpp); [Beleg 3](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/UI/Map/UIMap.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 014. `4ffc0511` — GL texture unit call reduction

**Historische Einordnung:** NICHT_PORTIEREN

GL33-Tex2DForSampling vermeidet glActiveTexture; keine WGPU-Änderung.

**Historische Evidenz:** ["turn97file0"]

**Historischer Prüfumfang:** Geänderte Module und Diff auf WGPU-/Engine-Relevanz eingeordnet

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/4ffc0511972a704e94918d26939f3946bd032e2b)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 015. `f5d08d3a` — UBO bind cache

**Historische Einordnung:** NICHT_PORTIEREN

Nur GL33-UniformBuffer-Bindcache.

**Historische Evidenz:** ["turn98file0"]

**Historischer Prüfumfang:** Geänderte Module und Diff auf WGPU-/Engine-Relevanz eingeordnet

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/f5d08d3ab52ac32945ea20ca3e18aed69166128c)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 016. `4b3a2070` — Incremental GL pipeline state

**Historische Einordnung:** NICHT_PORTIEREN

Nur inkrementelle OpenGL-Depth/Blend/Cull/FrontFace-States.

**Historische Evidenz:** ["turn99file0"]

**Historischer Prüfumfang:** Geänderte Module und Diff auf WGPU-/Engine-Relevanz eingeordnet

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/4b3a207085f3c451587209dc5b6c1e2fe786512a)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 017. `8a338067` — Redundant UBO uploads

**Historische Einordnung:** NICHT_PORTIEREN

Nur GL33-Matrix/Uniform-Uploads. Kein belegter WGPU-Nachrüstbedarf; nicht pauschal als Performance-Aufgabe übernehmen.

**Historische Evidenz:** ["turn100file0"]

**Historischer Prüfumfang:** Geänderte Module und Diff auf WGPU-/Engine-Relevanz eingeordnet

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/8a338067693e8f728ff1ac429b45aadaf690b27b)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 018. `e19872ca` — Skip polygon offset state updates

**Historische Einordnung:** NICHT_PORTIEREN

Nur OpenGL-Polygonoffset-State und Formatierung.

**Historische Evidenz:** ["turn101file0"]

**Historischer Prüfumfang:** Geänderte Module und Diff auf WGPU-/Engine-Relevanz eingeordnet

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/e19872ca7a4699a3c2bc710d87aafdaa4233dc29)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 019. `780f50c1` — Reset GL33 upload cache

**Historische Einordnung:** NICHT_PORTIEREN

Nur Initialisierungsinvalidierung des GL33-Upload-Snapshots.

**Historische Evidenz:** ["turn102file0"]

**Historischer Prüfumfang:** Geänderte Module und Diff auf WGPU-/Engine-Relevanz eingeordnet

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/780f50c1375bfc119cd8d1296da33150f747e640)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 020. `4bc72d42` — GL33 bind cache tests

**Historische Einordnung:** NICHT_PORTIEREN

Mock-glad-Tests des GL33-Bindcaches; nicht auf WGPU portieren.

**Historische Evidenz:** ["turn103file0"]

**Historischer Prüfumfang:** Geänderte Module und Diff auf WGPU-/Engine-Relevanz eingeordnet

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/4bc72d42829fa5bb56387ce01d545d8c895797a0)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 021. `6db0d1aa` — Testable GL33 state caches

**Historische Einordnung:** NICHT_PORTIEREN

GL33-UploadSnapshot/PipelineChanges-Refactoring und GL33-Tests.

**Historische Evidenz:** ["turn104file0"]

**Historischer Prüfumfang:** Geänderte Module und Diff auf WGPU-/Engine-Relevanz eingeordnet

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/6db0d1aa16e7e1c8dd72f81df2777148102ffc53)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 022. `29b6c5be` — Linux build docs output directory

**Historische Einordnung:** DOKU_ERGAENZEN

Linux-Anleitung im Ziel nennt noch dist/linux-x64-clang-rwdi; an tatsächliche CMake-Dist-Ausgabe anpassen (Upstream dist/x64-linux-rwdi).

**Historische Evidenz:** ["turn105file0", "turn107file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/29b6c5bea1f88d988e6eec643879819083f9a598)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/docs/build/linux.md)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 023. `9d362dd3` — Shared input bindings

**Historische Einordnung:** VORHANDEN

CapturePage.cpp vollständig blobidentisch mit Upstream; Use anyway und separater Replace-Pfad vorhanden.

**Historische Evidenz:** ["turn89file0", "turn91file0", "turn92file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/9d362dd3588cf7cf737cdb8bedfa8608c5b725dd)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/UI/Options/CapturePage.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 024. `16d39136` — Bindable chat controls

**Historische Einordnung:** VORHANDEN

Vier Chat-Aktionen, Profilversion5, Default-Ergänzung und PollChatInputActions in World::Simulate vorhanden.

**Historische Evidenz:** ["turn88file0", "turn90file0", "turn94file0", "turn96file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/16d39136eabd5e53cf3d8476577dae9886323f56)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/Input/InputSubsystem.cpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/UI/Settings/ContextControlsConfig.cpp); [Beleg 3](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/World/World.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 025. `3147548f` — Remove dead gitattributes

**Historische Einordnung:** NICHT_PORTIEREN

Reine Repository-Attribute-Bereinigung anhand des Upstream-Dateibestands. Nicht pauschal auf den anders bestückten Fork anwenden.

**Historische Evidenz:** ["turn108file0"]

**Historischer Prüfumfang:** Geänderte Module und Diff auf WGPU-/Engine-Relevanz eingeordnet

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/3147548fa5dd48822c4a1fcf803a87ebea455993)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 026. `589b4123` — P3D viewer error handling

**Historische Einordnung:** VORHANDEN

Viewer verwendet TryLoadP3D und protokolliert Fehler statt Throw. Zielwrapper fängt zusätzlich unbekannte Exceptions ab.

**Historische Evidenz:** ["turn109file0", "turn112file0", "turn114file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/589b41230ae194f4e97460aeab2f9641fe00adc6)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/World/Viewer.cpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/Asset/Formats/P3D/P3DModelLoad.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 027. `ea7602ce` — Dummy texture initialization

**Historische Einordnung:** VORHANDEN

Alle betroffenen TextureDummy-Skalarfelder im Ziel initialisiert.

**Historische Evidenz:** ["turn110file0", "turn113file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/ea7602ced6fc83d81fd184f643d3479264420bc3)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/Graphics/Dummy/TextureDummy.hpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 028. `fa3a65dd` — Entity type name lifetime

**Historische Einordnung:** VORHANDEN

Viewer-Klassenname und TriHornPlayerVehicle halten RString-Lebensdauer im Ziel korrekt.

**Historische Evidenz:** ["turn111file0", "turn112file0", "turn115file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/fa3a65dd0753bba22b0cfc5a90e59e70e0abebbb)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/World/Viewer.cpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/Game/Commands/GameStateExtTest.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 029. `08e67e93` — VertexTable minMaxDirty initialization

**Historische Einordnung:** VORHANDEN

Beide VertexTable-Konstruktoren initialisieren _minMaxDirty; DoConstruct kopiert Flag.

**Historische Evidenz:** ["turn116file0", "turn117file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/08e67e93e5da57aa663b15dbbe9384f0787070fb)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/Graphics/Rendering/Primitives/Vertex.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 030. `f351ad38` — fileCopy open creation mode

**Historische Einordnung:** VORHANDEN

open(O_CREAT) setzt 0666 im Zielcode.

**Historische Evidenz:** ["turn42file0", "turn43file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/f351ad382a601ba679fd2b544ff0ac73f3cf63d2)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/Foundation/Common/Platform.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 031. `9d8b7c54` — ParamFile realpath buffer overflow

**Historische Einordnung:** VORHANDEN

Linux-realpath-Puffer PATH_MAX und NUL-Fallback vorhanden.

**Historische Evidenz:** ["turn44file0", "turn45file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/9d8b7c542afac1423c839ab08495aff0b118feac)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/IO/ParamFile/ParamFileParse.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 032. `f9057657` — Copy permission and long path tests

**Historische Einordnung:** TEST_ERGAENZEN

Tests für fileCopy-Zielmodus nach umask und Config-Pfad >512 Bytes fehlen in beiden Zieldateien. Produktivfixes bereits vorhanden.

**Historische Evidenz:** ["turn118file0", "turn122file0", "turn123file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/f90576572433f9aed366c7a3b4782076ca4a5931)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/tests/unit/engine/Poseidon/Foundation/Common/test_platform.cpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/tests/unit/engine/Poseidon/IO/ParamFile/test_paramfile_parsing.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 033. `73302d1d` — Linux FORTIFY_SOURCE

**Historische Einordnung:** BUILD_ABSICHERN

Zentraler _FORTIFY_SOURCE=2-Block fehlt im Ziel-CMake; die native Linux-Clang-Toolchain setzt nur -m64. Für optimierte Linux/glibc-C++-Builds eine explizite, compilergeprüfte Policy ergänzen. Tatsächliche resultierende Compiler-/Distributionsdefaults wurden nicht mit einem Build gemessen; keine pauschale Behauptung ungeschützter Binaries.

**Historische Evidenz:** ["turn119file0", "turn121file0", "turn227file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/73302d1df4d06bbb606f1fc1b0b634013f505c96)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/CMakeLists.txt); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/cmake/toolchains/linux-x64-clang.cmake)

**Prüfung 2026-10-04: AKTUELL_GEPRUEFT.** Explizite FORTIFY-Einstellung in geprüfter Root-CMake-/Linux-Clang-Toolchain weiterhin nicht vorhanden; keine Aussage über extern injizierte Flags oder tatsächlich gebaute Binaries.

**Aktuelle Zielbelege:** [`CMakeLists.txt`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/CMakeLists.txt); [`cmake/toolchains/linux-x64-clang.cmake`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/cmake/toolchains/linux-x64-clang.cmake)

### 034. `718ab3c9` — Linux stack protector

**Historische Einordnung:** BUILD_ABSICHERN

Zentrale -fstack-protector-strong-Option fehlt im Ziel-CMake und in der nativen Linux-Clang-Toolchain. Plattform-/compilergeprüft ergänzen, wo nicht bereits durch externe Buildflags aktiv. Rust- und Windows-Builds nicht ungeprüft verändern.

**Historische Evidenz:** ["turn120file0", "turn121file0", "turn227file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/718ab3c979e66d769bc30a1853e5fa362cfbf9c4)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/CMakeLists.txt); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/cmake/toolchains/linux-x64-clang.cmake)

**Prüfung 2026-10-04: AKTUELL_GEPRUEFT.** Expliziter stack-protector-strong-Schalter in geprüfter Root-CMake-/Linux-Clang-Toolchain weiterhin nicht vorhanden. Compiler-/Plattform-/Sanitizer-Policy beachten.

**Aktuelle Zielbelege:** [`CMakeLists.txt`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/CMakeLists.txt); [`cmake/toolchains/linux-x64-clang.cmake`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/cmake/toolchains/linux-x64-clang.cmake)

### 035. `2054dbc8` — Soldier motion edge cost overflow

**Historische Einordnung:** VORHANDEN

MotionEdge::cost ist short; Initialisierung und SHRT-Sättigung sind im Ziel vorhanden. Produktionsfix geprüft, vollständige Parität des zugehörigen Upstream-Regressionstests nicht nachgewiesen.

**Historische Evidenz:** ["turn46file0", "turn47file0", "turn48file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/2054dbc870978c0eef48b58cbeece4817862df59)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/World/Entities/Infantry/SoldierOld.cpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/World/Entities/Infantry/SoldierOld.hpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 036. `881f9669` — MapFile EOF char narrowing

**Historische Einordnung:** VORHANDEN

Linker-Map-Parser: int vor EOF-Vergleich und ParseMapStream vorhanden. Kein Terrain-Format.

**Historische Evidenz:** ["turn124file0", "turn125file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/881f96691652ac6ab038d793393f0f4e5b1bcef9)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/IO/MapFile.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 037. `19db42bb` — Parent-relative mod paths

**Historische Einordnung:** VORHANDEN

CollapseParentDirs sowie normalisierter Pfad und Modalias in AutoOpen und FileExist vorhanden.

**Historische Evidenz:** ["turn126file0", "turn127file0", "turn127file1", "turn127file2"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/19db42bba842eaa96632f9e93a4cf2713899fa63)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/IO/Streams/QBStream.cpp)

**Prüfung 2026-10-04: AKTUELL_GEPRUEFT.** CollapseParentDirs und Alias-Fallback bestehen aktuell in AutoOpen/FileExist. Eigenen Archiv-/Streamingcode bewahren.

**Aktuelle Zielbelege:** [`engine/Poseidon/IO/Streams/QBStream.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/IO/Streams/QBStream.cpp)

### 038. `4bcddd8f` — Dedup mod multiplayer missions

**Historische Einordnung:** VORHANDEN

ModMPMissionsDir und einmaliger UI-Scan vorhanden; keine doppelte Groß-/Kleinschreibungs-Auflistung.

**Historische Evidenz:** ["turn128file0", "turn140file0", "turn141file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/4bcddd8f22a01f249bd28dd742a4d36a2d458877)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/Network/NetworkServerMission.cpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/UI/DisplayUIMultiplayer.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 039. `fe737ea0` — Mission row extension

**Historische Einordnung:** VORHANDEN

PBO-Zeilen starten mit nameNoExt und behalten einen Anzeigenamen-Fallback.

**Historische Evidenz:** ["turn129file0", "turn137file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/fe737ea0eb0515398ca26a0eef81c8a66ee2f905)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/UI/OptionsUIImpl.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 040. `e88327cb` — Mission PBO stringtable

**Historische Einordnung:** VORHANDEN

Der Ziel-Missionsbrowser fragt PBO-lokale Stringtables vor der globalen Lokalisierung ab. Betroffener Produktionspfad vorhanden; kein erneuter Feature-Port. Nicht jeder Helper-/Testfall wurde separat laufzeitgeprüft.

**Historische Evidenz:** ["turn130file0", "turn137file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/e88327cb1ee1dc61667e8e1b6dfbeb1569ee0e7e)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/UI/OptionsUIImpl.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 041. `6c35ff79` — Missing mission island

**Historische Einordnung:** VORHANDEN

Fehlende Inseln durch MissionBlockedReason/WorldInstalled abgedeckt. Eigenen expliziten TestWorldPath-Override erhalten.

**Historische Evidenz:** ["turn133file0", "turn137file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/6c35ff7970cefe8a1dba8fe78a634c8003c44d5f)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/UI/OptionsUIImpl.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 042. `f6572308` — Legacy codepage mission names

**Historische Einordnung:** VORHANDEN

Alle drei DecodeLegacyTextToRString-Anzeigenpfade vorhanden; interne Dateinamen bleiben unverändert.

**Historische Evidenz:** ["turn134file0", "turn137file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/f6572308c72f31a7923d4e506041d7cf35bfb3db)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/UI/OptionsUIImpl.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 043. `230dd173` — Mission warning level

**Historische Einordnung:** VORHANDEN

WarningLevel = level statt level + 1 ist vorhanden.

**Historische Evidenz:** ["turn135file0", "turn138file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/230dd173d1b76a87edc056a0d36a6dba12e53ba9)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/World/Simulation/Animation/FrameFunctions.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 044. `04189666` — Missing mission addons

**Historische Einordnung:** VORHANDEN

MissionBlockedReason mit ReadMissionAddons/FindMissingAddons ist im Ziel vorhanden; fehlende Addons werden im Startpfad berücksichtigt. Kein erneuter Produktions-Port. Vollständige UI-/Fixture-Parität des umfangreichen Commits nicht durch Testlauf nachgewiesen.

**Historische Evidenz:** ["turn136file0", "turn137file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/04189666c54a5cbc8beaefc03193432904c242b4)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/UI/OptionsUIImpl.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 045. `b54e468e` — MP category folder browsing

**Historische Einordnung:** VORHANDEN

Rekursiver MP-Lookup mit Tiefengrenze, MissionSubdir und Unterordner-Startpfaden ist im Ziel vorhanden. Kein erneuter Feature-Port. Vollständige Browser-/Fixture-Parität nicht durch Testlauf nachgewiesen.

**Historische Evidenz:** ["turn139file0", "turn140file0", "turn141file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/b54e468e81e5f72c053c32f444d5624919f14001)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/Network/NetworkServerMission.cpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/UI/DisplayUIMultiplayer.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 046. `df5d30f5` — UTF-8 Windows directory names

**Historische Einordnung:** VORHANDEN

UTF8-Win32-Scan vorhanden und gegenüber Upstream erweitert: keine abgeschnittenen Namen, Attribute und Zeitstempel erhalten.

**Historische Evidenz:** ["turn142file0", "turn143file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/df5d30f531c09dc08aa5453d59a896c407f775e8)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/IO/Filesystem/platform/Utf8Find_win.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 047. `33fd6ce1` — UTF-8 mod install paths

**Historische Einordnung:** VORHANDEN

ModArchive nutzt FilesystemPathFromUtf8 und OpenFileUtf8 samt UTF8-Fehlermeldungen.

**Historische Evidenz:** ["turn144file0", "turn146file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/33fd6ce19b1965d54475c30e7af25f6e7bf65f0a)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/Core/ModArchive.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 048. `90f38746` — Mods menu feedback

**Historische Einordnung:** VORHANDEN

Beide extra.SetBase(&Res) und Cursor-Bewegungswache beim animierten Notebook vorhanden.

**Historische Evidenz:** ["turn145file0", "turn148file0", "turn149file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/90f387466f8c913bf57d512e6d933cd96efcbbb6)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/Asset/Addon/ConfigParsers.cpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/UI/Controls/UIControlsBase.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 049. `bab72482` — Music fades and offsets

**Historische Einordnung:** VORHANDEN

Alle drei Produktionsfixes vorhanden: Startoffset bleibt erhalten, kleine Gain-Schritte akkumulieren, Skip berücksichtigt Kanalzahl.

**Historische Evidenz:** ["turn147file0", "turn150file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/bab72482280bed7ab6e0648f36da36c680b6eac6)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/PoseidonOpenAL/WaveOAL.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 050. `d9192a25` — Gear page arrow layout

**Historische Einordnung:** VORHANDEN

HTML-Bildfelder verwenden tableWidth als Advance und relative 1e-4-Zeilentoleranz; UIControlsExt.cpp FormatSection ist bereits korrigiert.

**Historische Evidenz:** ["turn151file0", "turn152file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/d9192a252d15002307817fea34c0cc5b66f6f67b)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/UI/Controls/UIControlsExt.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 051. `409d14d9` — Malformed mod resources fallback

**Historische Einordnung:** VORHANDEN

ConfigParsers.cpp ParseResource bricht erfolgloses Preprocessing als Kandidatenfehler ab; Base-Fallback vorhanden.

**Historische Evidenz:** ["turn153file0", "turn148file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/409d14d9c4f696e37dc4c89714e97aab64844e0d)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/Asset/Addon/ConfigParsers.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 052. `c0226c5d` — Log missing preprocessor includes

**Historische Einordnung:** VORHANDEN

Preproc.cpp meldet fehlende Includes mit LOG_ERROR(Config), zusätzlich eigene UTF-8-BOM-Behandlung.

**Historische Evidenz:** ["turn154file0", "turn155file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/c0226c5d5fb1320cc03ddefb82fe872e7a0c752a)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/IO/PreprocC/Preproc.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 053. `f2b39cd9` — Resume downloads

**Historische Einordnung:** VORHANDEN

DownloadWorker und MasterServerServiceClient unterstützen bereits Retry, .part.validator, Range/If-Range und Prüfung von HTTP206/Offset/Validator. Kein erneuter Produktiv-Port.

**Historische Evidenz:** ["turn156file0", "turn157file0", "turn158file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/f2b39cd99f94ff84fdb36678285a607ebcb7907a)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/Core/DownloadWorker.cpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/Network/MasterServerServiceClient.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 054. `613f5fde` — Generic localized download feedback

**Historische Einordnung:** VORHANDEN

DownloadDialogView verwendet generische lokalisierbare Labels, Install-Fehler und ETA. Worker/Transport nutzen bereits benannte Retry-/HTTP-Konstanten.

**Historische Evidenz:** ["turn159file0", "turn160file0", "turn157file0", "turn158file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/613f5fde45bce6ebfda88382063f46aee3d72271)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/Core/DownloadDialogView.cpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/Core/DownloadWorker.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 055. `de34f16e` — Log abstract entity once per type

**Historische Einordnung:** VORHANDEN

EntityAI-Konstruktor protokolliert abstrakte scope-0-Typen bereits einmal je Typ mit LOG_WARN_ONCE_PER.

**Historische Evidenz:** ["turn161file0", "turn162file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/de34f16eaa011057d1835ab2571aa76777328f68)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/AI/VehicleAI.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 056. `d3f93039` — Stop logging foreign radio commands on the center channel

**Historische Einordnung:** VORHANDEN

Nur Logging-Änderung, keine Änderung der Befehlsauswahl! AIGroupImpl hat die beiden Aufrufe und unterdrückt erwarteten fremden Center-Funk sogar vollständig.

**Historische Evidenz:** ["turn163file0", "turn164file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/d3f930394b88bdf393ffeedc3097eeb0b6bee851)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/AI/AIGroupImpl.cpp)

**Prüfung 2026-10-04: AKTUELL_GEPRUEFT.** Die zwei Änderungen des Commits sind vorhanden. Vier andere Prüfstellen in zwei weiteren Überladungen bleiben ungeschützt; das ist die weitergehende PR #79, kein Fehlen dieses Zweizeilen-Commits.

**Aktuelle Zielbelege:** [`engine/Poseidon/AI/AIGroupImpl.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/AI/AIGroupImpl.cpp)

### 057. `02c0d741` — Fixed benchmark LOD

**Historische Einordnung:** IMPLEMENTIEREN

POSEIDON_LOD_FIX fehlt im aktuellen SceneDraw.cpp; eigener adaptiver 60-FPS-/Retained-Governor vorhanden, aber kein Freeze am Einstieg. Kleine Benchmark-Option anpassen, eigene Governor-Logik erhalten; CPU, Retained-GPU und Schatten mit identischer LOD-Dichte prüfen.

**Historische Evidenz:** ["turn165file0", "turn166file0", "turn76file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/02c0d741c3a436252c6e7b5282229309c8c8fb09)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/World/Scene/SceneDraw.cpp)

**Prüfung 2026-10-04: AKTUELL_GEPRUEFT.** Kein POSEIDON_LOD_FIX in aktuellem SceneDraw. Eigenen retained-aware Governor erhalten; optionalen gemeinsamen Freeze-Punkt mit CPU-/GPU-/Shadow-Lod-Verifikation ergänzen.

**Aktuelle Zielbelege:** [`engine/Poseidon/World/Scene/SceneDraw.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/World/Scene/SceneDraw.cpp)

### 058. `ef6534b1` — GPU land clipping and instancing

**Historische Einordnung:** EIGENE_LOESUNG

Der gemischte GL33-GPU-Landclip-/Instancing-Commit wird funktional durch eigene WGPU-Conform-Daten, ForestPlain-Plane und Retained-Instancing abgedeckt. Keine GL33-Schnittstellen oder Batch-Prädikate blind übernehmen; die Trennung visueller GPU-Anpassung von CPU-Geometrie erhalten.

**Historische Evidenz:** ["turn167file0", "turn175file0", "turn191file0", "turn205file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/ef6534b1ea9c0ffa62a0ce94aa997c4f4e3cb699)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/World/Scene/Object.cpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/World/Scene/ObjectClasses.cpp); [Beleg 3](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/WgpuRenderer/EngineWgpu.hpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 059. `f845a731` — Per-LOD instancing land clip

**Historische Einordnung:** EIGENE_LOESUNG

Upstream korrigiert GL33-Instancing-Zulässigkeit auf den tatsächlichen LOD. Object::Draw des Zielbranches verwendet ebenfalls das konkrete LevelOpaque(forceLOD) bei PublishConformPlane; kein gleicher GL33-Batchpfad im WGPU-Backend.

**Historische Evidenz:** ["turn168file0", "turn205file0", "turn191file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/f845a7318cfb29f349b3446f00362292bcc620a1)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/World/Scene/Object.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 060. `315733ab` — Heightmap invalidation setTerrainGrid

**Historische Einordnung:** VORHANDEN

Exakt an allen drei Upstream-Stellen ResampleTerrain, SubdivideTerrainOneStep und LoadSubdivCache wird bereits HeightRevision erhöht (UP-315733ab-Kommentare). TerrainWgpu invalidiert anhand dieser Revision.

**Historische Evidenz:** ["turn169file0", "turn172file0", "turn172file1", "turn172file2", "turn171file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/315733ab32b2e06d9be31274fcf3aa396abcba00)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/World/Terrain/LandSave.cpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/WgpuRenderer/TerrainWgpu.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 061. `b04ae1ae` — Land clip mode tests

**Historische Einordnung:** NICHT_PORTIEREN

GL33-spezifische Vertexmodus-Klassifikation und Tests. Für WGPU keine neue Implementierung; dort gelten eigene Conform-Daten. Asset-Regeln allenfalls als Testidee.

**Historische Evidenz:** ["turn173file0"]

**Historischer Prüfumfang:** Geänderte Module und Diff auf WGPU-/Engine-Relevanz eingeordnet

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/b04ae1ae54983672f6473215f9200b9521d86126)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 062. `9a02f8ec` — ForestPlain GPU land clip

**Historische Einordnung:** EIGENE_LOESUNG

ForestPlain hat bereits ComputeConformPlane/GpuConformPlane und scoped Draw-Zustand; t1/t2 bleiben starr. Eigene WGPU-Lösung statt GL33-Shader/LandClipMode portieren.

**Historische Evidenz:** ["turn174file0", "turn175file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/9a02f8eca8887bbb589797110b613fb522b9b733)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/World/Scene/ObjectClasses.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 063. `7ca93a57` — Destructive deformation instancing guard

**Historische Einordnung:** VORHANDEN

DeformsSharedShape in Head- und Run-Mitglied-Prädikat; DestructTree-Ausnahme und Tests vorhanden.

**Historische Evidenz:** ["turn83file0", "turn80file0", "turn76file0", "turn81file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/7ca93a57fe6fec150140d5f3f26b630721387fde)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/World/Scene/Object.cpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/World/Scene/SceneDraw.cpp); [Beleg 3](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/tests/unit/engine/Poseidon/World/test_object_animated_bounds.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 064. `9a298d09` — Animated bounds state

**Historische Einordnung:** VORHANDEN

Warm-cache-BSphere und isAnimated-Guard sowie Nested-Animation-Regressionstest vorhanden.

**Historische Evidenz:** ["turn82file0", "turn84file0", "turn81file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/9a298d09fd225c7019ce6195dc012950ece4833a)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/World/Scene/Object.cpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/tests/unit/engine/Poseidon/World/test_object_animated_bounds.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 065. `a918a023` — Release GL33 heightmap

**Historische Einordnung:** NICHT_PORTIEREN

Nur GL33-Heightmap-Textur beim Shutdown löschen und UBO-Kommentar korrigieren.

**Historische Evidenz:** ["turn176file0"]

**Historischer Prüfumfang:** Geänderte Module und Diff auf WGPU-/Engine-Relevanz eingeordnet

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/a918a0232a7642377bafb31553a2a14951166967)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 066. `bc3014a9` — Uniform buffer binding cache

**Historische Einordnung:** NICHT_PORTIEREN

Nur glBindBuffer durch GL33Bind-Cachefunktion ersetzen.

**Historische Evidenz:** ["turn177file0"]

**Historischer Prüfumfang:** Geänderte Module und Diff auf WGPU-/Engine-Relevanz eingeordnet

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/bc3014a96eee37d39b0c083322b9f4caffacbb59)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 067. `078bb2f7` — Widescreen cursor

**Historische Einordnung:** VORHANDEN

Durch vollständige CursorLayout-Lösung im Ziel bereits überholt; kein Einzelzeilen-Port.

**Historische Evidenz:** ["turn178file0", "turn181file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/078bb2f7714f57139262dca0c606a5410df79142)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/UI/Map/UIContainers.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 068. `4c6aff84` — All cursor layout scaling

**Historische Einordnung:** VORHANDEN

CursorLayout.cpp und der aktive ControlsContainer::DrawCursor-Pfad sind im Ziel vorhanden; der frühere einfache Breiten-Fix ist dadurch überholt. Kein erneuter Cursor-Feature-Port; kein visueller Laufzeittest aller Karten-/Zielcursor durchgeführt.

**Historische Evidenz:** ["turn179file0", "turn180file0", "turn181file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/4c6aff849d1bee101ea8d1c813b1866ba138e58e)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/UI/Controls/CursorLayout.cpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/UI/Map/UIContainers.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 069. `7d69cbe3` — Flaky Trident tests

**Historische Einordnung:** IMPLEMENTIEREN

vehicle_turbo.test.sqf nutzt noch zweimal 90 Frames statt gleicher Simulationszeit; GameInstance hat keinen LogTap. Trident-Port angepasst an vorhandenes Process-Tree-Cleanup; UI-Text-/Audioassertionen prüfen.

**Historische Evidenz:** ["turn182file0", "turn183file0", "turn185file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/7d69cbe3eb15899d631e11bf5b796042b96a13ee)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/tests/integration/ingame/vehicles/vehicle_turbo.test.sqf); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Trident/src/client/instance.rs)

**Prüfung 2026-10-04: AKTUELL_GEPRUEFT.** vehicle_turbo bleibt auf zweimal triSimFrames 90; GameInstance besitzt noch keinen LogTap. Simulationszeit und passende Log-Assertions ergänzen; eigenes Prozessbaum-Cleanup erhalten.

**Aktuelle Zielbelege:** [`tests/integration/ingame/vehicles/vehicle_turbo.test.sqf`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/tests/integration/ingame/vehicles/vehicle_turbo.test.sqf); [`engine/Trident/src/client/instance.rs`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Trident/src/client/instance.rs)

### 070. `6a63f96d` — Fedora perl-open

**Historische Einordnung:** DOKU_ERGAENZEN

Fedora-Anleitung enthält perl-open noch nicht. Kleine Build-Dokumentationsergänzung, keine Engine-Funktion.

**Historische Evidenz:** ["turn106file0", "turn107file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/6a63f96dd91e1526d25fcc91e5ea97d4581c343a)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/docs/build/linux.md)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 071. `a61130ca` — Reset scene preloader

**Historische Einordnung:** VORHANDEN

UnloadGameData setzt ScenePreloader nach GWorld/GScene-Abbau bereits zurück. Eigenes zusätzliches Audio-Lifetime-Fix bewahren.

**Historische Evidenz:** ["turn186file0", "turn187file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/a61130ca3d78e5e96b284bc6f1675eb1eaa847a9)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/Foundation/Platform/Shutdown.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 072. `ed6119f9` — Reload detail textures

**Historische Einordnung:** NICHT_PORTIEREN

Der Upstream-Fix leert den GL33-spezifischen CfgDetailTextures-Satz. TextureBankWgpu hat diesen Satz nicht; sie verwaltet eigene Textur-/Material- und Residency-Caches. Kein 1:1-Port. Allgemeiner Modwechsel-/Cache-Invalidierungstest bleibt optional; aus diesem Diff allein folgt kein WGPU-Bug.

**Historische Evidenz:** ["turn188file0", "turn189file0", "turn192file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/ed6119f9f65244231ed0cc393e5e238bc98d3c5f)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/WgpuRenderer/TextureBankWgpu.cpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/WgpuRenderer/TextureBankWgpu.hpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 073. `c551e783` — Locally lit instancing

**Historische Einordnung:** EIGENE_LOESUNG

WGPU übernimmt lokale Lichter bereits über den PresentationSnapshot in einen gemeinsamen GPU-Lichtbestand. EngineWgpu überschreibt nicht die alte InstancedRun-Schnittstelle. Der GL33-UBO-/8-Lichtindex-Port ist daher keine fehlende WGPU-Instancing-Funktion; den eigenen Retained-Pfad bewahren.

**Historische Evidenz:** ["turn193file0", "turn190file1", "turn191file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/c551e7838816ad63302e261c22e96dd6d1613068)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/WgpuRenderer/EngineWgpu.cpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/WgpuRenderer/EngineWgpu.hpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 074. `cc4bd699` — Shape bucketing

**Historische Einordnung:** BEDINGT_PORTIEREN

CPU-Pfad nutzt weiterhin gepackte Quicksort-Schlüssel, kein O(n)-Bucketing. Pass2 sortiert noch die ganze _drawMergers-Liste vor der Auswahl der Surface-Blends. Zuerst nur tatsächlich gezeichnete Surface-Einträge sortieren und messen; Bucketing für verbleibende CPU-Objekte danach bedingt übernehmen. Eigene Pass2-Tie-Reihenfolge und Thread-Ownership sichern; keine veränderlichen Bucket-Epochen blind in geteilte LODShapes kopieren. Keine Aussage über einen gemessenen FPS-Gewinn.

**Historische Evidenz:** ["turn195file0", "turn197file0", "turn229file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/cc4bd699d14d9214a75a694106eea823f0122d86)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/World/Scene/SceneDraw.cpp)

**Prüfung 2026-10-04: AKTUELL_GEPRUEFT.** Pass2-Surface sortiert weiterhin die komplette _drawMergers-Liste. Zuerst passende Surface-Einträge vorfiltern, dann messen. Kein Übernehmen globaler veränderlicher LODShape-Bucket-Epochen ohne Ownership-/Sortiertie-Prüfung.

**Aktuelle Zielbelege:** [`engine/Poseidon/World/Scene/SceneDraw.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/World/Scene/SceneDraw.cpp)

### 075. `6f0d7fec` — Release GL33 instancing buffers

**Historische Einordnung:** NICHT_PORTIEREN

Löscht drei GL33-UBOs beim Renderer-Shutdown und invalidiert GL-Bindings. Diese Ressourcen existieren nicht im WGPU-Pfad.

**Historische Evidenz:** ["turn198file0"]

**Historischer Prüfumfang:** Geänderte Module und Diff auf WGPU-/Engine-Relevanz eingeordnet

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/6f0d7fecf7e85b9c1858c4fb445139ea9c8c5782)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 076. `3541affa` — Extract batching helpers

**Historische Einordnung:** TEST_BEI_PORT

SceneDrawBuckets-Helfer und Zwei-Frame-Gruppierungstest nur zusammen mit einer angepassten Übernahme von cc4bd69 sinnvoll. GL33-Lichtindex-Packing und dessen Tests nicht auf WGPU portieren.

**Historische Evidenz:** ["turn196file0", "turn197file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/3541affa8027e92e072a5ef899b588a31823100b)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/World/Scene/SceneDraw.cpp)

**Prüfung 2026-10-04: AKTUELL_GEPRUEFT.** Gruppierungs-/Folgeframe-Tests nur bei entsprechendem Umbau adaptieren. Das GL33-Lichtindex-Packing ist kein neuer WGPU-Portbedarf.

**Aktuelle Zielbelege:** [`engine/Poseidon/World/Scene/SceneDraw.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/World/Scene/SceneDraw.cpp)

### 077. `0e24d744` — Windows PAA encoder test temp paths

**Historische Einordnung:** VORHANDEN

PAA-Encoder-Tests benutzen im Ziel bereits TestFixtures::GetTempFilePath und CleanupTempFile statt /tmp.

**Historische Evidenz:** ["turn199file0", "turn200file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/0e24d7445a21261e94fb42897e6056b918c631a9)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/tests/unit/engine/Poseidon/Graphics/test_paa_encoder.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 078. `77f1f8ce` — Addon warning logging

**Historische Einordnung:** VORHANDEN

Beide betroffenen AddonSystem-Warnungen verwenden bereits LOG_WARN: zu hohe requiredVersion und fehlendes requiredAddon. Eigene Arma-Asset-Mount-Erweiterungen unangetastet lassen.

**Historische Evidenz:** ["turn201file0", "turn202file0", "turn203file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/77f1f8ce817e1767ac3af80abbd8ded31a2a6068)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/Asset/Addon/AddonSystem.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 079. `8c61786d` — Land clip before proxies

**Historische Einordnung:** EIGENE_LOESUNG

Object::Draw veröffentlicht im Ziel die Conform-Daten des tatsächlich gezeichneten LODs vor Animation und Proxy-Aufrufen; WGPU besitzt dafür eigene Zustands-/Vertexselektoren. Kein belegter identischer Bug und kein GL33-LandClipMode-Port. Ein gezielter WGPU-Test mit Waffen-/Gebäude-Proxies neben terrainangepasstem Wald wäre optional; die Upstream-Tests allein prüfen nicht die volle Proxy-Rekursion.

**Historische Evidenz:** ["turn204file0", "turn205file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/8c61786ddba773dc13fc9ddb1f5e0aaa5ac93a5c)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/World/Scene/Object.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 080. `82147189` — Instanced terrain rendering

**Historische Einordnung:** EIGENE_LOESUNG

WGPU besitzt bereits GPU-Heightmap/CDLOD-Terrain. Das alte CreateNearVBuffers/FillCache wird bei vorhandenem GPU-Terrainrenderer übersprungen. Der im Commit erwähnte Materialfehler betrifft damit den alten, für WGPU nicht benutzten Vorladepfad. Weder GL33-Terrain-Instancing noch dessen Texture-Array-Limits übernehmen.

**Historische Evidenz:** ["turn206file0", "turn208file0", "turn171file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/82147189e8f7ca053414f99aa7fb0a04a15f586c)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/WgpuRenderer/TerrainWgpu.cpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/World/Terrain/Landscape.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 081. `b20613b0` — GLSL validation rules

**Historische Einordnung:** NICHT_PORTIEREN

Ausschließlich glslang-Regeln für OpenGL-3.3-Shader statt versehentlicher OpenGL-4.5/SPIR-V-Anforderungen. Keine WGSL/WGPU-Änderung.

**Historische Evidenz:** ["turn209file0"]

**Historischer Prüfumfang:** Geänderte Module und Diff auf WGPU-/Engine-Relevanz eingeordnet

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/b20613b0d967fb0756bbb77a5c68deb40f3f59e5)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 082. `b534fd79` — Terrain shader count

**Historische Einordnung:** NICHT_PORTIEREN

Nur GL33-Shaderzählung/Enum-Erwartungen im Test an den neuen Terrainshader angepasst.

**Historische Evidenz:** ["turn210file0"]

**Historischer Prüfumfang:** Geänderte Module und Diff auf WGPU-/Engine-Relevanz eingeordnet

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/b534fd79b9e3dacec9e1c451f47a2dd2cd6fdcac)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 083. `c1f76798` — Terrain toggle render tab

**Historische Einordnung:** NICHT_PORTIEREN

Verschiebt ausschließlich den GL33-Terrain-A/B-Schalter im Debugmenü.

**Historische Evidenz:** ["turn211file0"]

**Historischer Prüfumfang:** Geänderte Module und Diff auf WGPU-/Engine-Relevanz eingeordnet

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/c1f76798742789d366c32f4a8a51c035b7963f96)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 084. `ce7c1537` — MP version display

**Historische Einordnung:** VORHANDEN

MP-Browser benutzt bereits genau %.2f %s%s; kein erneut zu übernehmender Versionsanzeige-Fix.

**Historische Evidenz:** ["turn212file0", "turn214file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/ce7c153746d659359260934c5470e46650e81f83)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/UI/DisplayUIMultiplayer.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 085. `825b3b15` — Shader include preprocessor

**Historische Einordnung:** NICHT_PORTIEREN

GLSL-Fragment-/Include-Refactoring im GL33-Renderer. Keine neue Engine-Semantik und kein belegter WGSL-Nachrüstbedarf. Nach geändertem Modul und Diff als GL33-spezifisch eingeordnet.

**Historische Evidenz:** ["turn215file0"]

**Historischer Prüfumfang:** Geänderte Module und Diff auf WGPU-/Engine-Relevanz eingeordnet

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/825b3b15d2c84bf3047eb3f3d4e62022a83f8c09)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 086. `ad332672` — Instanced water rendering

**Historische Einordnung:** EIGENE_LOESUNG

Eigener WGPU-Wasserpfad zeichnet bereits CDLOD-Knoten mit GPU-Wellen/FFT und vergrößertem Ozeangebiet. Die neue GL33-Einzelzellen-Instanzierung löst kein fehlendes Feature dieses Branches.

**Historische Evidenz:** ["turn216file0", "turn217file0", "turn218file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/ad332672ee252426d271ad8db359c969831bd4c2)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/WgpuRenderer/WaterWgpu.cpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 087. `d70fdbd5` — One terrain instance per segment

**Historische Einordnung:** EIGENE_LOESUNG

GL33 wechselt von einzelnen Zellen zu 8x8-Segmentinstanzen. WGPU arbeitet schon mit gröberen, entfernungsabhängigen CDLOD-Knoten, nicht mit der hier ersetzten Einzelzellenliste. Kein pauschaler 8x8-Umbau und kein übertragbarer Faktor-64-Gewinn.

**Historische Evidenz:** ["turn219file0", "turn217file0", "turn228file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/d70fdbd56ed46de6bcca90217f4158bf2ec10fbc)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/WgpuRenderer/TerrainWgpu.cpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/WgpuRenderer/WaterWgpu.cpp); [Beleg 3](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/World/Terrain/TerrainCdlod.hpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 088. `618bdcd4` — Remove redundant terrain sampler rebinding

**Historische Einordnung:** NICHT_PORTIEREN

Nur zwei redundante glBindSampler-Aufrufe im GL33-Wasserpfad entfernt.

**Historische Evidenz:** ["turn220file0"]

**Historischer Prüfumfang:** Geänderte Module und Diff auf WGPU-/Engine-Relevanz eingeordnet

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/618bdcd41b987e79718fa943dfe3dcbfd01ec014)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 089. `235e9181` — Separate shader sources and tests

**Historische Einordnung:** NICHT_PORTIEREN

Verschiebt GL33-Shaderquellen in eigene Translation Unit und passt GLSL-Tests/Enum-Prüfungen an. Kein WGPU-Feature-Port. Große Shader-Verschiebung nach Modulgrenze und relevanten Änderungen eingeordnet, keine neue shaderweise Bildvalidierung durchgeführt.

**Historische Evidenz:** ["turn221file0"]

**Historischer Prüfumfang:** Geänderte Module und Diff auf WGPU-/Engine-Relevanz eingeordnet

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/235e9181e0fbe067be5e5433792ace1340d779c1)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 090. `06bd4a1a` — Water beyond map boundaries

**Historische Einordnung:** EIGENE_LOESUNG

Off-map-Ozean ist im aktuellen Ziel vorhanden: vergrößerter zentrierter Wasserbaum; CdlodHeightBounds(ocean=true) nimmt Meereshöhe in Bounds auf und liest Heightmap nur innerhalb ihrer Grenzen. Terrain darf im eigenen WGPU-Design bewusst Randhöhen als Meeresboden weiterführen; den GL33-Land-Discard nicht pauschal übertragen.

**Historische Evidenz:** ["turn222file0", "turn217file0", "turn218file0", "turn228file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/06bd4a1a0b486bc35c280df7a792bde755c4ab30)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/WgpuRenderer/WaterWgpu.cpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/World/Terrain/TerrainCdlod.hpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 091. `8d878a08` — Harden terrain segment lookup

**Historische Einordnung:** NICHT_PORTIEREN

Härtet ausschließlich den Indexzugriff auf GL33-Segmenttabellen. WGPU nutzt andere CDLOD-Strukturen und deren getrennte Ozean-/Map-Bounds. Kein belegter identischer Indexfehler im Ziel; negative Koordinaten bleiben sinnvolle allgemeine Testwerte, keine neue Pflichtimplementierung aus diesem Commit.

**Historische Evidenz:** ["turn223file0", "turn228file0"]

**Historischer Prüfumfang:** Diff und konkret genannte Zielcode-Pfade statisch verglichen

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/8d878a0898d26fdbd7f6d2e245b7049a84cc8d31)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f4ffc5106745d0491489f52108bf93fa5658a6ff/engine/Poseidon/World/Terrain/TerrainCdlod.hpp)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 092. `3b5849e3` — Shader catalog for cache hashing

**Historische Einordnung:** NICHT_PORTIEREN

GL33-Shader-Binärcache hasht seinen Shaderkatalog statt einer manuellen Liste. Kein WGPU-Cache-Fix.

**Historische Evidenz:** ["turn224file0"]

**Historischer Prüfumfang:** Geänderte Module und Diff auf WGPU-/Engine-Relevanz eingeordnet

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/3b5849e397123ba915a508c119916d7e3bd4a016)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 093. `3ffb0338` — Link instanced ground shaders in tests

**Historische Einordnung:** NICHT_PORTIEREN

Linkt zusätzlich zwei instanzierte GL33-Terrain-/Wasser-Shaderpaare in glslang-Tests. Konkrete Shader/API sind nicht im WGPU-Pfad; daraus folgt kein fehlender WGPU-Test.

**Historische Evidenz:** ["turn225file0"]

**Historischer Prüfumfang:** Geänderte Module und Diff auf WGPU-/Engine-Relevanz eingeordnet

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/3ffb0338c0c7c62d1d4c8bfb9a0dd37923dedebf)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 094. `b67bf3bd` — Enable instanced terrain by default

**Historische Einordnung:** NICHT_PORTIEREN

Reiner GL33-Default-Schalter für instanziertes Terrain.

**Historische Evidenz:** ["turn38file0"]

**Historischer Prüfumfang:** Geänderte Module und Diff auf WGPU-/Engine-Relevanz eingeordnet

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/b67bf3bd623a52d62456c3ef65f7e0e0163dcd9b)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 095. `dc42d84f` — Key the Windows ccache on the MSVC version

**Historische Einordnung:** BUILD_ABSICHERN

Windows-CI/Build-Härtung. Aktuelles Ziel-action.yml ist exakt der Upstream-Pre-Fix-Blob 770297f6…: der ccache-Key enthält OS/Preset/PCH-Hashes, aber keine MSVC/cl.exe-Version. Upstream ergänzt die cl.exe-Dateiversion via vswhere. Sinnvoll bei Windows-CI mit ccache; keine Runtime-/Gameplay-Auswirkung.

**Historische Evidenz:** ["target HEAD b8f61b97: .github/actions/build-cwr/action.yml blob 770297f6ec7fe8710454110229b2105c43d59b1a", "upstream pre-fix b67bf3bd: same blob 770297f6ec7fe8710454110229b2105c43d59b1a", "upstream post-fix dc42d84f: blob 5f651a79aa3f7694b6e2cd973bf27620131c8bf1"]

**Historischer Prüfumfang:** Actual upstream pre/post file and current target file statically compared; no CI run, build, test or benchmark.

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/dc42d84f73808b0fe09d7b61ef987f6aa0c60cd8)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b8f61b97af97183b9444c32f9e50be0e1be45a6a/.github/actions/build-cwr/action.yml)

**Prüfung 2026-10-04: TREE_IDENTITAET.** Die komplette .github-Tree-SHA ist seit 8aca5c43 unverändert (5412568222e10f6e84277b72f2d648dcb72f9c3b). MSVC-Version fehlt daher weiterhin im bisher auditierten ccache-Key.

**Aktuelle Zielbelege:** [`.github/actions/build-cwr/action.yml`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/.github/actions/build-cwr/action.yml)

### 096. `a13cac07` — Give dbghelp a full-size copy of the crash context

**Historische Einordnung:** IMPLEMENTIEREN

Echter Windows-Plattform-Portkandidat. Current target CrashHandler.cpp and crash-handler test are exactly the upstream pre-fix blobs. Port AllocateDumpContext/CopyContext full-sized XState context buffer plus unreadable-boundary regression test together. Fixes minidump reliability for ERROR_INVALID_USER_BUFFER; does not fix the originating crash. Not GL33/WGPU-specific.

**Historische Evidenz:** ["target HEAD b8f61b97: CrashHandler.cpp blob 56df3518669245d0f09a3f7ea0c5646657caa591 == upstream pre-fix", "target HEAD b8f61b97: test_crash_handler.cpp blob db68087b28dc6816833f773bd424505a400f0d28 == upstream pre-fix", "upstream post-fix: CrashHandler.cpp blob 119355985c8c9648e32107d6300230b23cdb7abb; test blob c2fd19266135002b2027cdfa57ab3625fc2d17c0"]

**Historischer Prüfumfang:** Actual upstream pre/post production and regression-test files compared against current target files; no Windows build or crash-handler test executed.

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/a13cac078a6a6be260438266fd869fd184c0a330)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b8f61b97af97183b9444c32f9e50be0e1be45a6a/engine/Poseidon/Foundation/Platform/CrashHandler.cpp); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b8f61b97af97183b9444c32f9e50be0e1be45a6a/tests/unit/engine/Poseidon/Foundation/Platform/test_crash_handler.cpp)

**Prüfung 2026-10-04: AKTUELL_GEPRUEFT.** Full-size CopyContext-Puffer weiterhin nicht im CrashHandler. Dieser hat inzwischen eigene DIAG-001-Crash-Notify-Arbeit: nicht mehr als exakt unveränderter Pre-Fix-Blob bezeichnen! Kontextkopie und PAGE_NOACCESS-Test einpassen, Callback erhalten. Behebt Dump-Erzeugung, nicht Ursache des ursprünglichen Crashes.

**Aktuelle Zielbelege:** [`engine/Poseidon/Foundation/Platform/CrashHandler.cpp`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/engine/Poseidon/Foundation/Platform/CrashHandler.cpp)

### 097. `7bcb5cb6` — Upload also tools

**Historische Einordnung:** OPTIONAL

Realer, aber niedrig priorisierter CI-/Distributions-Portkandidat. Der aktuelle build.yml-Blob 9c6b7ef2… lädt Game, GameDemo und Server hoch, aber weder PoseidonTools noch PoseidonStudio; unter Windows fehlen damit auch deren PDBs im Symbolartefakt. Die aktuellen CMake-Ziele existieren nachweislich: PoseidonTools und PoseidonStudio werden gebaut, und beide CMakeLists rufen dist_copy(...) auf. Upstream ergänzt genau diese Artefakte für Linux, macOS und Windows. Nützlich, wenn Tools/Studio aus CI heruntergeladen oder getestet werden; keine Runtime-/Renderer-Auswirkung.

**Historische Evidenz:** ["target HEAD 8aca5c43: .github/workflows/build.yml blob 9c6b7ef2518e7e09571e9d2b6d4a706a63a7264f uploads Game/GameDemo/Server but not Tools/Studio", "target HEAD 8aca5c43: apps/tools/Tools/CMakeLists.txt blob cff5c8f7ae4d692e18c4cc2f215411b798993721 defines PoseidonTools and calls dist_copy(PoseidonTools)", "target HEAD 8aca5c43: apps/tools/Studio/CMakeLists.txt blob bc077a7ac5606b63ea250e7094b513e49070168d defines PoseidonStudio and calls dist_copy(PoseidonStudio)"]

**Historischer Prüfumfang:** Actual upstream diff plus current target workflow and both current tool target definitions inspected statically; no workflow run/build/artifact download executed.

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/7bcb5cb64f116308b0455b26155c1a32e3309011)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/8aca5c43bd11445a799da76efacd1e41bfef3eb2/.github/workflows/build.yml); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/8aca5c43bd11445a799da76efacd1e41bfef3eb2/apps/tools/Tools/CMakeLists.txt); [Beleg 3](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/8aca5c43bd11445a799da76efacd1e41bfef3eb2/apps/tools/Studio/CMakeLists.txt)

**Prüfung 2026-10-04: TREE_IDENTITAET.** Workflow-Veröffentlichung von Tools/Studio weiterhin offen. Die Workflows sind bytegleich zur letzten Prüfung; Tools-CMake wurde hier nicht erneut vollständig auditiert.

**Aktuelle Zielbelege:** [`.github`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/.github)

### 098. `5883e924` — Update GitHub Actions to Node 24 releases

**Historische Einordnung:** BUILD_ABSICHERN

Realer CI-Portkandidat; im Ziel nur teilweise vorhanden. Game-/Server-Artefakte verwenden bereits upload-artifact@v7, aber der aktuelle Branch enthält weiterhin 9× checkout@v4 in build.yml, 2× in lint.yml, 1× in rust.yml und 1× in pch.yml; außerdem cache@v4, download-artifact@v4, mehrere upload-artifact@v4 sowie docker/setup-buildx-action@v3 und docker/build-push-action@v6. Upstream hebt diese auf die Node-24-Majors an. Nicht blind den gesamten Workflow ersetzen: nur die Action-Majors übernehmen und die vielen WGPU-/Trident-spezifischen Zieländerungen behalten.

**Historische Evidenz:** ["target build.yml blob 9c6b7ef2…: checkout@v4 x9, upload-artifact@v4 x6, download-artifact@v4 x7, docker/setup-buildx-action@v3 x1, docker/build-push-action@v6 x1, cache@v4 x2; upload-artifact@v7 x11 already present", "target lint.yml blob 89f0f464…: checkout@v4 x2", "target rust.yml blob 747ebcb6…: checkout@v4 x1; target pch.yml blob 70ef8d4e…: checkout@v4 x1", "target build-cwr action blob 770297f6…: actions/cache@v4 remains"]

**Historischer Prüfumfang:** Actual upstream diff compared with every affected current target workflow/action and counted current action versions; no GitHub Actions run executed.

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/5883e9244117b8fa00ff856e34978a0158f65674)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/8aca5c43bd11445a799da76efacd1e41bfef3eb2/.github/actions/build-cwr/action.yml); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/8aca5c43bd11445a799da76efacd1e41bfef3eb2/.github/workflows/build.yml); [Beleg 3](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/8aca5c43bd11445a799da76efacd1e41bfef3eb2/.github/workflows/lint.yml); [Beleg 4](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/8aca5c43bd11445a799da76efacd1e41bfef3eb2/.github/workflows/pch.yml); [Beleg 5](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/8aca5c43bd11445a799da76efacd1e41bfef3eb2/.github/workflows/rust.yml)

**Prüfung 2026-10-04: TREE_IDENTITAET.** Action-Version-Mix unverändert. Nur Versions-/Runtime-Updates übernehmen, eigene WGPU-/Trident-/Ledger-Checks erhalten.

**Aktuelle Zielbelege:** [`.github`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/.github)

### 099. `c2fd4c2e` — Install LLVM without install-llvm-action

**Historische Einordnung:** BUILD_ABSICHERN

Realer CI-Portkandidat. Das Ziel benutzt KyleMayes/install-llvm-action@v2 weiterhin sowohl im Windows-Build-Composite als auch im Lint-Workflow. Upstream ersetzt den Windows-Buildpfad durch direkten LLVM-19.1.7-Download/7z-Extraktion und installiert clang-format 21.1.8 im Linux-Lint per pipx. Die Idee passt direkt; die exakten Versionen sollten als bewusst gepinnte Toolchain-Policy übernommen werden. Keine Engine-Runtime-Auswirkung.

**Historische Evidenz:** ["target .github/actions/build-cwr/action.yml blob 770297f6… still uses KyleMayes/install-llvm-action@v2 version 19", "target .github/workflows/lint.yml blob 89f0f464… still uses KyleMayes/install-llvm-action@v2 version 21"]

**Historischer Prüfumfang:** Actual upstream diff and both current target users of install-llvm-action inspected statically; no Windows runner or lint job executed.

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/c2fd4c2e91f19054b2f0ce0f171ac0d5c4346727)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/8aca5c43bd11445a799da76efacd1e41bfef3eb2/.github/actions/build-cwr/action.yml); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/8aca5c43bd11445a799da76efacd1e41bfef3eb2/.github/workflows/lint.yml)

**Prüfung 2026-10-04: TREE_IDENTITAET.** LLVM-Action-Einbindung in unveränderter .github-Struktur weiterhin vorhanden. Aktualisierung nur als Build-Arbeit, kein Engine-FPS-Fix.

**Aktuelle Zielbelege:** [`.github`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/.github)

### 100. `b453df21` — Drop vcpkg_copy_pdbs from the openal-soft port

**Historische Einordnung:** OPTIONAL

Kein eigenständiger Runtime-Port; optionaler Build-Cleanup. Der aktuelle Ziel-Portfile-Blob 99348585… ist exakt der Upstream-Pre-Fix-Blob und enthält vcpkg_copy_pdbs(). Upstream entfernt nur diesen Aufruf, weil für den verwendeten Windows-OpenAL-Build keine PDB erzeugt wird und der Schritt lediglich warnt. Kann bei der nächsten vcpkg-/CI-Aufräumrunde mitgenommen werden, ist aber allein keine dringende Änderung und ändert Audioverhalten oder Debug-Symbole des eigenen Engine-Codes nicht.

**Historische Evidenz:** ["target HEAD 8aca5c43: cmake/vcpkg-overlay-ports/openal-soft/portfile.cmake blob 99348585181c754d0ee6a95c8ec3c0f2e76b9cba", "upstream immediately before b453df21 has the identical blob 99348585181c754d0ee6a95c8ec3c0f2e76b9cba and contains vcpkg_copy_pdbs()", "upstream b453df21 removes only that one call"]

**Historischer Prüfumfang:** Actual one-line upstream diff compared against exact current target/pre-fix blob; no vcpkg build executed.

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/b453df2147b3ed633038b93d89f2a045449d6d23)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/8aca5c43bd11445a799da76efacd1e41bfef3eb2/cmake/vcpkg-overlay-ports/openal-soft/portfile.cmake)

**Prüfung 2026-10-04: HISTORISCH_UEBERNOMMEN.** Historische Einzelbewertung unverändert erhalten. Kein neuer Upstream-Diff, da main unverändert. Relevanz nicht aus bloßer SHA-Abwesenheit abgeleitet; diese Produktionsstellen wurden nicht sämtlich erneut am neuen Branch gelesen.

### 101. `19bc48a0` — Add Dependabot for GitHub Actions

**Historische Einordnung:** OPTIONAL

Realer, optionaler Repository-Wartungsport. Im aktuellen Zielbranch existiert keine .github/dependabot.yml. Upstream lässt GitHub Actions monatlich für Root-Workflows und Composite Actions prüfen und bündelt die Updates. Das ist besonders sinnvoll, weil der Zielbranch derzeit mehrere alte Action-Majors parallel trägt; es erzeugt aber bewusst Bot-PRs und ist deshalb eine Repo-Policy-Entscheidung, kein Engine-Fix.

**Historische Evidenz:** ["target HEAD 8aca5c43: .github/dependabot.yml is absent (GitHub contents API returned 404)", "target workflows/action currently contain several older action majors (checkout@v4, cache@v4, download-artifact@v4 and install-llvm-action@v2)"]

**Historischer Prüfumfang:** Upstream config inspected; target file absence verified and current workflow action versions sampled/count-checked. No Dependabot run or repository write.

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/19bc48a0f9cf4204775feca7f978216ea8675235)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/8aca5c43bd11445a799da76efacd1e41bfef3eb2/.github/workflows/build.yml); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/8aca5c43bd11445a799da76efacd1e41bfef3eb2/.github/actions/build-cwr/action.yml)

**Prüfung 2026-10-04: TREE_IDENTITAET.** Dependabot-Datei weiterhin nicht in der unveränderten .github-Struktur. Repo-Policy: erzeugt neue Update-PRs; nicht ungefragt aktivieren.

**Aktuelle Zielbelege:** [`.github`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/.github)

### 102. `b36cddc7` — Run CI through one workflow with a gate job

**Historische Einordnung:** BEDINGT_PORTIEREN

Bedingter realer CI-Portkandidat, nicht 1:1 übernehmen. Im Ziel existiert kein ci.yml; build.yml, lint.yml und rust.yml besitzen jeweils eigene push/pull_request-Trigger (Push derzeit auf new-renderer-infrastructure) und enthalten zusätzliche WGPU-/Ledger-/Trident-Logik. Upstream macht diese drei Workflows über workflow_call wiederverwendbar und fügt einen einzigen CI-gate-Job hinzu. Das ist nützlich, wenn Branch-Protection/Auto-Merge einen stabilen Required Check braucht. Beim Port müssen die Ziel-Branch-Trigger angepasst und alle eigenen Jobs/Permissions/Concurrency-Regeln erhalten bleiben; upstream main darf nicht blind kopiert werden.

**Historische Evidenz:** ["target HEAD 8aca5c43: .github/workflows/ci.yml is absent (GitHub contents API returned 404)", "target build.yml blob 9c6b7ef2…, lint.yml blob 89f0f464…, rust.yml blob 747ebcb6… each still declare direct push/pull_request triggers rather than workflow_call", "target build/lint workflows contain substantial fork-specific WGPU, Trident and ledger gates that must remain intact"]

**Historischer Prüfumfang:** Actual upstream diff compared with current target workflow triggers and fork-specific jobs; no branch-protection setting or workflow run changed/tested.

[Upstream-Commit](https://github.com/ofpisnotdead-com/CWR-CE/commit/b36cddc7ef745da86d308038108986b1641b574a)

**Original-Zielbelege:** [Beleg 1](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/8aca5c43bd11445a799da76efacd1e41bfef3eb2/.github/workflows/build.yml); [Beleg 2](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/8aca5c43bd11445a799da76efacd1e41bfef3eb2/.github/workflows/lint.yml); [Beleg 3](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/8aca5c43bd11445a799da76efacd1e41bfef3eb2/.github/workflows/rust.yml)

**Prüfung 2026-10-04: TREE_IDENTITAET.** Gemeinsames CI-Gate weiterhin nicht in der unveränderten .github-Struktur. An eigene Branch-Trigger/Validierungen anpassen, keine fremden Workflows darüberkopieren.

**Aktuelle Zielbelege:** [`.github`](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/b5dc6070cf5a5b2feff0bafd612ed8567529c919/.github)

## Geschlossene, nicht gemergte PRs: getrennte Referenzliste

Diese 12 Einträge sind keine zusätzliche Liste fertiger Port-Kandidaten. Acht davon wurden nur anhand der Metadaten eingeordnet; die vier tatsächlich gelesenen Diffs sind entsprechend gekennzeichnet. Historisch gemergte PRs werden über main betrachtet, nicht zusätzlich als neue Änderungen gezählt.

### #206 — Scripting/JSON/database/events/loadout bundle

Geschlossener Sammelentwurf; insbesondere nicht stillschweigend neue Persistenz-/Datenbank-Semantik einführen. Loadout-Teil wird unter offenem #208 separat bewertet.

**Umfang:** Geschlossen/unmerged: nur Metadaten gesichtet; Diff nicht erneut auditiert. [PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/206)

### #190 — Addon conflict/merge-order diagnostics

Tatsächlicher Diff gelesen. Nützliche Idee für Herkunft/Config-Priorität; aktuelles OpDiag und bestehendes Conflicting-addon-Logging zuerst abgleichen. Nicht als bewiesene fehlende Funktion eingestuft.

**Umfang:** Geschlossen/unmerged: PR-Diff gelesen, kein vollständiger aktueller Zielcode-Abgleich. [PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/190)

### #189 — Config/bank/type/slot diagnostic dumps

Tatsächlicher Diff gelesen. Diagnose-/Harness-Werkzeug als Referenz, nicht zweites unkoordiniertes Diagnosesystem neben dem eigenen Fork. Kein aktueller vollständiger Zielabgleich.

**Umfang:** Geschlossen/unmerged: PR-Diff gelesen, kein vollständiger aktueller Zielcode-Abgleich. [PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/189)

### #165 — Application name/path correction

Tatsächlicher Diff gelesen. Branding-/Pfadkonvention des Forks bewahren; CWR-Namen nicht blind übernehmen. Kein priorisierter Laufzeitport.

**Umfang:** Geschlossen/unmerged: PR-Diff gelesen, kein vollständiger aktueller Zielcode-Abgleich. [PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/165)

### #163 — UTF-8 faces / related changes

Tatsächlicher Diff gelesen. Gemischtes Paket mit Entfernung späterer Face-Refresh-Retry-/Testlogik; keinen UTF-8-Fix über einen funktionalen Rückschritt erkaufen. Einzelnen Fehler im aktuellen Fork nachweisen.

**Umfang:** Geschlossen/unmerged: PR-Diff gelesen, kein vollständiger aktueller Zielcode-Abgleich. [PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/163)

### #126 — Older combined performance work

Historisch/superseded; viele Instancing-/Land-Clip-Themen existieren auf main oder sind durch WGPU anders gelöst. Nur Metadata-Sichtung in diesem Lauf.

**Umfang:** Geschlossen/unmerged: nur Metadaten gesichtet; Diff nicht erneut auditiert. [PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/126)

### #94 — ARM64 support predecessor

Geschlossener Vorgänger der späteren ARM64/Main-Arbeit. Nicht zusätzlich neben der bestehenden Commit-Bewertung als neuer Port zählen.

**Umfang:** Geschlossen/unmerged: nur Metadaten gesichtet; Diff nicht erneut auditiert. [PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/94)

### #90 — Overlay/control UI adjustment

Metadata-Sichtung; kein bestätigter aktueller Portbedarf.

**Umfang:** Geschlossen/unmerged: nur Metadaten gesichtet; Diff nicht erneut auditiert. [PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/90)

### #89 — External GL shader organization

Metadata-Sichtung; GL33-Architektur hat bei auslaufender Unterstützung keine eigene Priorität.

**Umfang:** Geschlossen/unmerged: nur Metadaten gesichtet; Diff nicht erneut auditiert. [PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/89)

### #87 — Widescreen UI predecessor

Metadata-Sichtung; spätere LayoutCanvas-/Cursor-Commits und eigene WGPU-Lösung berücksichtigen, nicht doppelt zählen.

**Umfang:** Geschlossen/unmerged: nur Metadaten gesichtet; Diff nicht erneut auditiert. [PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/87)

### #20 — SDL/vcpkg dependency alternative

Metadata-Sichtung; Dependency-/Distributionsthema, kein bestätigter aktueller Laufzeitfix.

**Umfang:** Geschlossen/unmerged: nur Metadaten gesichtet; Diff nicht erneut auditiert. [PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/20)

### #18 — Debian/ibus build prerequisite

Metadata-Sichtung; distributionsabhängige Build-Voraussetzung, kein bestätigter aktueller Engine-Port.

**Umfang:** Geschlossen/unmerged: nur Metadaten gesichtet; Diff nicht erneut auditiert. [PR](https://github.com/ofpisnotdead-com/CWR-CE/pull/18)

## Historie und Weiterführung

Der vorherige Audit `CWR-CE_Audit_streaming-residency_8aca5c4_2026-10-01.json` bleibt unverändert. Seine Metadaten und alle 102 Originaleinträge sind in der neuen JSON enthalten. Für spätere Alerts gelten der aktuelle Zielbranch/HEAD und der aktuelle Upstream-/PR-HEAD, nicht ein dauerhaft eingefrorener alter Branch. Ein PR muss bei Änderung seines Diffs erneut bewertet werden.

Es wurden ausschließlich lokale Audit-Dokumente erzeugt. Keine Branches, Dateien im Repository, PRs, Issues, Actions oder Alarm-Zeitpläne wurden geändert.
