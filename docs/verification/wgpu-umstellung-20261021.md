# GL33 → WGPU: Phase A und Highpoly-Befund, 2026-10-21

Branch `MrDudeX-wgpu`, Basis `feat/arma-original-ui` @ `d707fd1`. Statisch gelesen und an einem
privaten Addon vermessen; **es wurde nichts im Spiel gemessen**. Die Zahlen unten sind Zählungen aus
den Dateien, keine Bildraten.

## Was im Spielordner läuft (Regel 0)

`H:\…\ARMA Cold War Assault\Remastered` am 2026-10-21:

- `DEPLOYED-FROM.txt` sagt `feat/arma-original-ui b972c04 2026-10-17_15:14 github-actions-zip`.
- `wgpu_renderer.dll` ist aber vom 2026-10-17 20:51:49 UTC, `PoseidonGame.exe` vom 20:51:37 UTC. Beide
  sind sechs Stunden jünger als der Stempel. Sie gehören zusammen, aber **welcher Commit sie sind, sagt
  keine Datei**.
- `OpenPoseidon.exe` (Zeit 2026-10-21 04:35) ist byte-gleich mit `PoseidonGame.exe` (md5
  `4e65009a…`), also nur eine Kopie.

Vor jeder Messung den Ordner einmal mit `scripts\Deploy.ps1` aus einem bekannten Build neu stempeln.

## Testobjekt: die komplette Big Shell (privates Addon des Eigentümers)

Gezählt wurde nur aus den Dateien des Addons (eine große Offshore-Anlage aus 90 Modulen); nichts davon
liegt im Repo, und das Addon wird nicht weitergegeben.

| Größe | Wert |
| --- | --- |
| Gesetzte Objekte (ein `place.sqs`-Aufruf) | 615 (90 sichtbare Module, 297 Kollisionsblöcke, 113 Türlicht-Modelle, Türflügel) |
| Format | MLOD SP3X, **je Modell nur eine Auflösungs-LOD (1.0)** plus Geometry |
| Dreiecke LOD 1.0 gesamt | ≈ 322 000 |
| Sektionen (= Draw-Aufrufe pro Pass) | ≈ 10 500, im Mittel ≈ 30 Dreiecke je Sektion |
| Texturen | 5 370 PAA, fast alle 32–256 px; 4 158 DXT1, 1 212 DXT5 |
| DXT5 nach Alpha-Histogramm (Regel aus `ClassifyAlpha`) | 1 178 Blend, 33 Cutout, 1 Opaque |
| Sichtbare Module mit mindestens einer Blend-Sektion | **81 von 90** |
| Blend-Sektionen | ≈ 2 040 (≈ 38 000 Dreiecke) |
| UV außerhalb 0…1 (Stichprobe Strut A) | 0 von 8 415 Flächen |

Das Problem ist also **nicht die Polygonzahl**: 322 000 Dreiecke sind für eine RTX 3090 Ti wenig. Teuer
sind die vielen winzigen Draw-Aufrufe und dass fast jedes Modul teilweise auf dem CPU-Pfad bleibt.

## Wo die Zeit hingeht (Code-Pfad, WGPU)

1. **Zulassung zum GPU-Pfad.** `House`-Objekte sind statisch und werden in `EngineWgpu::SceneObjectCreated`
   (`EngineWgpu.cpp` ~17294) aufgenommen. Hat ein Modell eine Sektion, die der Retained-Pfad nicht
   übernimmt (`SectionRetainedRefuses` / `SectionGpuOwned`, `ShapeDraw.cpp` ~392–460: Blend ohne Tiefe),
   wird es **Partial**. Dann läuft jedes Bild das volle `Object::Draw` auf dem Haupt-Thread, und jede
   Blend-Sektion geht einzeln durch `DrawSectionTL` und den Render-Thread. Bei der Big Shell sind das
   81 Module und ≈ 2 000 Einzel-Draws pro Bild. Laut Audit vom 14.10. ist der Render-Thread schon bei
   ≈ 3 800 Einzel-Draws voll ausgelastet.
2. **GPU-Pfad.** Die übrigen ≈ 8 500 Sektionen laufen indirekt (`multi_draw_indexed_indirect_count`),
   pro Pass einmal: Vorpass, Farbpass, 4 Schattenkaskaden (2 davon im Wechsel gecacht), Meeresspiegelung.
   Das sind grob 50 000–60 000 Teil-Draws mit je ≈ 30 Dreiecken pro Bild.
3. **Culling nur pro Objekt.** Kugel pro Instanz, kein Culling pro Sektion. Ein Modul mit 150 m Radius
   ist fast immer sichtbar, auch seine Innenräume. Hi-Z gilt nur im Farbpass.
4. **Eine LOD.** Alle LOD-Hebel (Entfernung, Schattenkaskaden ab 2, Spiegelung) greifen ins Leere.
5. **Cull-Shader.** Pro Instanz läuft ein GPU-Thread etwa siebenmal seriell über alle Sektionen
   (`cull.wgsl` 288–381, `whatif_triangles` auch in Schattenansichten). Bei 300 Sektionen pro Modul und
   wenigen Instanzen ist das schlecht parallel. Wie viel das kostet, ist noch nicht gemessen.

**GL33 im Vergleich:** kein Vorpass, keine Spiegelung, kein Hi-Z. Aber jedes Bild wird der Vertexpuffer
neu gebaut und hochgeladen, wenn die Klasse `animated` erlaubt (Standard bei CfgVehicles,
`ShapeDraw.cpp:1363`), und Schatten sind eine CPU-Dreieckssuppe. Ein fairer Vergleich braucht deshalb
dieselbe Szene und dieselbe Kamera; die Zahlen sind trotzdem nur als Gesamtwert vergleichbar.

## Messung (GL33 gegen WGPU)

Mission `BigShellBench.Intro` (eigener Inhalt, im Benutzerordner des Eigentümers, nicht im Repo):
25 s Aufbau, dann drei feste Kameras je 20 s (Gesamtansicht, Deck Strut A, knapp über dem Meer).
Jede Kamera schreibt eine Zeile `[tri] triPerfStats fps=… frame=avg/p95/max … calls=…` ins Log.

Starter `Remastered\BigShell_FPS_Messung.bat`: beide Renderer nacheinander, 1600×900 im Fenster,
`POSEIDON_FPS_CAP=0`, `POSEIDON_VSYNC=0` bzw. `triSetVsync 0`, `POSEIDON_FRAME_TRACE` (eine CSV-Zeile
pro Bild, beide Renderer), unter WGPU zusätzlich `WGR_FRAME_STATS=1` (GPU-Zeit p50/p95/p99) und
`WGR_COVERAGE_TRACE=1` (Full/Partial je Modell). Ergebnis in `BigShell_FPS_Ergebnis.txt`.

Was nur WGPU kann: GPU-Zeiten je Pass (GL33 hat keine Timer-Queries). Was beide können:
`triPerfStats`, `POSEIDON_FRAME_TRACE`, `--benchmark`, `--diag perf`.

## Vorschläge, nach Nutzen sortiert (alle bildgleich, Umsetzung nach Messung und Freigabe)

| Prio | Hebel | Erwartung | Aufwand |
| --- | --- | --- | --- |
| 1 | **Addon:** Texturatlas je Modul im Konverter (UVs liegen alle in 0…1). 300 → ~5–10 Sektionen je Modul | Draw-Aufrufe ÷ 20–40, in beiden Renderern | Konverter, kein Engine-Code |
| 2 | **Addon:** Blend nur, wo wirklich durchsichtig; sonst Alpha entfernen (DXT1) oder harte Kante (Cutout) | Module werden Full, CPU-Pfad fällt weg | Konverter |
| 3 | **Addon:** Auflösungs-LODs (z. B. 1, 50, 200 m) und eine einfache Schatten-LOD | Schattenkaskaden und Spiegelung zeichnen weniger | Konverter |
| 4 | **Engine:** Große Modelle beim Registrieren räumlich in Teil-Instanzen zerlegen (Culling und Hi-Z pro Teil) | Innenräume und Teile außerhalb der Kaskade fallen weg | ≈ 300–600 Zeilen C++ |
| 5 | **Engine:** Dreieckssummen je LOD vorberechnen, `whatif` nur in der Hauptansicht | weniger serielle GPU-Schleifen | ≈ 30–60 Zeilen WGSL/Rust |
| 6 | **Engine:** Sektionen gleichen Materials beim Registrieren zusammenlegen | nur wenn Texturen mehrfach vorkommen | ≈ 100 Zeilen |
| 7 | **Engine:** Indizes vertex-cache-optimiert | wenig, Vertexshader läuft in 5–7 Pässen | ≈ 150 Zeilen |
| 8 | **Engine, nur GL33:** intakte Statics als statischer Vertexpuffer | GL33 lädt nicht mehr jedes Bild neu hoch | ≈ 5 Zeilen |

Prio 1–3 bringen am meisten, sind aber Änderungen am privaten Addon und gehören in dessen Projekt.
Prio 4 und 5 helfen allen großen Modellen (Gebäude-Addons, Reforger) und sind der Engine-Teil.

## Gelerntes, in diesem Branch umgesetzt

- `WGPU-020` (`cc6046c`): P3DM-Sicht-LODs behalten lose Punkte, die eine benannte Selektion hält
  (z. B. Lenkradachse `osaVolantZac`/`osaVolantKon`). `POSEIDON_MLOD_SELECTION_POINTS=0` = alt.
- `WGPU-021` (`fb483b9`, wirksam erst mit `08868c7`): MLOD-Adapter teilt Sektionen an den CfgModels
  `sections[]`-Selektionen, auch in der von `Model::compile` erzeugten Sektionstabelle (der Weg im Spiel),
  damit `hiddenSelections` und `setObjectTexture` auf P3DM-Modellen wirken. Keine Fläche wird umsortiert,
  jedes Stück behält Textur, Flags und RVMAT. `POSEIDON_MLOD_CFG_SECTIONS=0` = alt.

Prüfung: Windows-Spiel-Build `08868c7` grün (Workflow `wgpu-windows.yml`, Artefakt
`OpenPoseidon-Windows-MrDudeX-wgpu-08868c7`). Die neuen Unit-Tests (`test_mlod_writer.cpp` Ende,
`test_shape_adapter_cfg_sections.cpp`) sind lokal mit clang 18 syntaxgeprüft; der Loader-Test lief in einem
Harness (7 Vertices, Selektion mit genau dem Achspunkt). Die volle Testsuite baut in CI derzeit auf keinem
Branch (auch `MrDudeX-arma2` scheitert dort an fremdem Code), daher kein CI-Testergebnis.

## Gelernt, bewusst nicht hier umgesetzt

- `#lightpoint`, `setLightColor/Ambient/Brightness`, `lightAttachObject` sind weiter Stubs
  (`GameStateExt.cpp` 2091–2094, 1206–1213). Gehört in den Arma-1-Branch; Bausteine sind da
  (`LightPointOnVehicle`, `AttachedOnVehicle`, `Fireplace.cpp` als Vorlage).
- `setObjectMaterial` / `hiddenSelectionsMaterials` sind Arma-3-Stubs. Gehört zum Spiegelungsprojekt
  (Material-Overrides gehen denselben Weg wie `surfMat`).
- Reflektoren: `VehicleAI.cpp:1179` gibt `size` als Winkel weiter, die Größe bleibt 1. Eine Korrektur
  ändert die Lichtkegel-Glow-Größe aller Fahrzeuge und braucht vorher ein Bild und eine Entscheidung.
