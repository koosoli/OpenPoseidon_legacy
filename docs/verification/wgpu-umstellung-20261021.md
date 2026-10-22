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

## Messung 1: Ausgangsstand (Spielordner, Build `59af2f9`), 21.10. 12:25

RTX 3090 Ti, 1600×900 im Fenster, 8x MSAA, Renderskala 1,25, Vsync und FPS-Grenze aus, Mission
`BigShellBench.Intro`.

| Kamera | GL33 FPS (Bildzeit Mittel / p95) | WGPU FPS (Bildzeit Mittel / p95) |
| --- | --- | --- |
| 1 Gesamtansicht | 45,4 (22,0 / 23,0 ms) | 50,8 (19,7 / 49,0 ms) |
| 2 Deck Strut A | 46,4 (21,6 / 22,8 ms) | 28,8 (34,7 / 71,2 ms) |
| 3 Meeresspiegel | 43,0 (23,3 / 36,5 ms) | 46,9 (21,3 / 29,1 ms) |

- GL33: Haupt-Thread, 13–17 ms Objektzeichnen, ~19 000 Draw-Aufrufe.
- WGPU: Haupt-Thread 2 ms; Render-Thread ~22 ms (12 ms in `finish()`+`submit`, auf dem Deck 24 ms bei
  2 340 Einzel-Draws); GPU 17–20 ms, davon Culling 3,7–4,1 ms, Objekte 1,5 ms, Schatten 0,4 ms.
- `WGR_COVERAGE_TRACE`: genau 81 Module Partial, 305 Modelle Full (wie aus den Dateien vorhergesagt).

## Messung 2: A/B mit `MrDudeX-wgpu` @ `9a43c0c` (Testordner, gleiche Spieldaten), 21.10. 17:25

„alt“ = `WGR_CULL_COOP=0 WGR_VB_WHOLE=0 WGR_SHADOW_STATE_FILTER=0`, „neu“ = Standard. Gleiches Binary.

| Kamera | WGPU alt FPS (Mittel / p95) | WGPU neu FPS (Mittel / p95) |
| --- | --- | --- |
| 1 Gesamtansicht | 54,8 (18,3 / 25,8 ms) | 53,4 (18,7 / 20,4 ms) |
| 2 Deck Strut A | 36,7 (27,3 / 43,0 ms) | **43,8 (22,8 / 42,4 ms)** |
| 3 Meeresspiegel | 52,4 (19,1 / 27,8 ms) | 51,4 (19,4 / 23,5 ms) |

| Messgröße (über den ganzen Lauf) | alt | neu |
| --- | --- | --- |
| GPU-Culling `cull_total` | 2,5–3,5 ms (Spitzen 7 ms) | **0,7–1,1 ms** |
| GPU-Bildzeit p50 (Mittel der 5-s-Fenster) | ≈ 22,6 ms | ≈ 19,5 ms |
| Render-Thread `submit` | ≈ 9,0 ms (Spitzen 17,7) | ≈ 7,9 ms (Spitzen 10,4) |

- Kein Absturz, keine Validierungsfehler im Log.
- Auf dem Deck bleibt die GPU bei ~22 ms: `colour_pass` 16,8 ms, davon 14,8 ms außerhalb der
  GPU-getriebenen Blätter, also die ~2 600 Einzel-Draws der halbtransparenten Teile (je Teil Zwilling
  mit eigener Pipeline plus Blend-Draw). Zusätzlich wartet der Haupt-Thread in `land:gnd` 8–10 ms auf
  den Render-Thread (Producer-Fenster), p95 39 ms.
- GL33 mit `POSEIDON_GL33_VB_TRUST_DIRTY=1`: nicht eindeutig. Die Draw-Aufrufe der Läufe unterscheiden
  sich (11 241 gegen 7 973 an Kamera 1), die Objektzeit pro Aufruf bleibt gleich. GL33 hängt an den
  Draw-Aufrufen, nicht am Hochladen. Der Schalter bleibt aus.

## Nächster Hebel (wartet auf Entscheidung)

Aufeinanderfolgende Einzel-Draws gleicher Pipeline als ein `multi_draw_indexed_indirect`. Exakt geht
das nur für Läufe ohne Zwilling. Mit Zwilling müssten alle Zwillinge eines Laufs vor dessen Blend-Draws
gezeichnet werden; das verschiebt Farbe nur dort, wo ein späterer deckender Texel (Alpha ≥ 0,98) einen
früheren halbtransparenten überdeckt, um höchstens (1 − a)² ≤ 0,04 %. Ziel: GPU-Rest im Farbpass
(~14 ms) und Render-Thread-Zeit auf dem Deck deutlich senken.

## Messung 3: Flug über die ganze Big Shell, Build `f9a7a1a`, 21.10. 21:20

Mission `BigShellFlug.Intro` (nur auf dem PC des Eigentümers): Hinflug auf 60 m über die Ostseite
(Strut A-F-E-D, Brücke D-G, Shell 2 L-K-J, ~40 s), Drehung über Strut J (9 s), Rückflug auf
Deckhöhe 52 m über die Westseite (J-I-H, Brücken, C-B-A, ~40 s). Zeitgesteuert, für beide
Renderer gleich; triPerfStats je halbem Abschnitt (42 Fenster). FPS = zeitgewichtetes Mittel.

| Lauf | gesamt | Hinflug | Drehung | Rückflug | schlechtestes Fenster | Fenster mit p95 > 50 ms |
| --- | --- | --- | --- | --- | --- | --- |
| WGPU `WGR_DIRECT_BATCH=0` | 39,6 | 31,2 | 75,9 | 47,5 | 12,6 | 28 von 42 |
| WGPU Standard (`exact`) | 41,1 | 33,1 | 76,8 | 48,0 | 14,4 | 30 von 42 |
| WGPU `all` | 45,1 | 35,2 | 80,4 | 55,4 | 15,0 | 28 von 42 |
| GL33 | 68,5 | 64,2 | (Blick aufs Meer) | 62,1 | 37,1 | 0 von 42 |

- `all` bündelt 2 650-2 930 Draws in ~200 Läufen (170-190 mit Zwilling); auf Deckhöhe +17-40 %
  gegenüber `0`. `exact` bündelt nur ~240 Draws: fast alle Blend-Sektionen der Big Shell haben
  einen MAT-053-Zwilling.
- Die Einbrüche (12-17 fps zwischen Strut F, E und D) sind kein GPU- und kein Render-Worker-Problem:
  GPU p50 17 ms, Worker ~11 ms, der Haupt-Thread wartet nicht auf den Worker (`wait=0`), aber
  `land:gnd` braucht 40-60 ms je Bild (GL33: ~1 ms). PERF-030 zerlegt die Phase für den nächsten Lauf.

## Messung 4: PERF-030 findet die Einbrüche, PERF-031 behebt sie (21.10. 22:17 / 23:00)

PERF-030 (Build `5d9730e`, ein WGPU-Lauf mit `all`): jedes langsame `land:gnd`-Bild war der
Terrain-Draw, und darin ausschließlich der Schnee-Upload. `MaskSnowShelters` prüfte alle 0,1 s
1 515 526 Punkte unter 240 Gebäuden (die Big-Shell-Module) gegen deren Innenraum-Voxel: 75 ms je
Aufruf, obwohl auf Desert Island kein Schnee liegen kann (die alpine Schneegrenze ist standardmäßig
an, 645 m). Mittel `land:gnd` 11-17 ms, davon `snow` 10-16 ms; alle 84 langsamen Terrain-Bilder `snow`.

PERF-031 (`1b97cf4` + `b3f8c9c`): (1) ohne Schneefall und ohne Gelände an der Schneegrenze im
64-m-Fenster entfällt die Maske (kann kein Pixel ändern); (2) mit Schnee merkt sich jedes Gebäude
seine Prüfergebnisse auf dem globalen 0,5-m-Raster, ein Aufruf prüft nur Unbekanntes.

Flug, WGPU `all`, gleiche Mission (zeitgewichtete fps):

| Lauf | gesamt | Hinflug | Rückflug | schlechtester Abschnitt | Abschnitte mit p95 > 50 ms | max. p95 |
| --- | --- | --- | --- | --- | --- | --- |
| ohne Schnee, `5d9730e` | 44,9 | 34,7 | 56,0 | 13,2 | 28 von 42 | 104 ms |
| ohne Schnee, `b3f8c9c` | **74,2** | 71,4 | 75,8 | **54,8** | **0** | 23 ms |
| 30 cm Schnee, `5d9730e` | 43,2 | 34,1 | 52,4 | 14,5 | 29 von 42 | 105 ms |
| 30 cm Schnee, `b3f8c9c` | **68,5** | 65,0 | 71,0 | 50,6 | 0 | 23 ms |
| 30 cm Schnee, `b3f8c9c`, `POSEIDON_SNOW_SHELTER_CACHE=0` | 40,3 | 30,9 | 50,8 | 13,2 | 29 von 42 | 143 ms |
| GL33 (Messung 3) | 68,5 | 64,2 | 62,1 | 37,1 | 0 | 28 ms |

- Mit Schnee: neue Maske 2,7 ms statt 75 ms je Aufruf, 0 neue Prüfungen, 1 515 526 aus dem Cache,
  **masked=91 280 in allen Läufen** -- dasselbe Ergebnis.
- WGPU ist damit auf dem ganzen Flug schneller als GL33 und ohne Ruckler.

## Messung 5: neuer Stand `2ff794f` im Spielordner (22.10. 08:20)

`MrDudeX-wgpu-aktuell` @ `2ff794f` = Olis `MrDudeX-arma1` @ `9c16747d` (darauf baut
`feat/jump-reload-lean`) + 24 WGPU-Commits. Eingespielt in `Remastered` aus dem CI-Build
(GitHub Actions 39693362590, win-x64-clang-rwdi). `OpenPoseidon.exe`, `PoseidonGame.exe` und
`wgpu_renderer.dll` stammen alle aus diesem einen Build, sind per `cmp` geprüft und haben dieselbe
Dateizeit. `DEPLOYED-FROM.txt` ist neu gestempelt. Der vorige Stand (`9c16747d`) liegt in
`Remastered\_backup_vor_wgpu_20261022\`. Gemessen wurde die EXE aus `Remastered`, also genau das,
was der Besitzer spielt.

Gleiche Flug-Mission. Kennzahl hier: **Bilder ÷ Zeit über den ganzen Flug**. Das ist nicht die
abschnittsgewichtete Zahl aus Messung 4, deshalb stehen die alten Läufe in derselben Kennzahl daneben.

| Lauf | Bau | fps | schlechtester Abschnitt | max. p95 | GPU-Bild (Mittel) | 3D-Farbpass GPU |
| --- | --- | --- | --- | --- | --- | --- |
| WGPU `all`, ohne Schnee | `b3f8c9c` (alte Basis) | 75,5 | 54,8 | 22,8 ms | 12,8 ms | 7,0 ms |
| WGPU `all`, ohne Schnee | **`2ff794f`** | **77,5** | **57,0** | 19,8 ms | 14,9 ms | 8,5 ms |
| WGPU `exact` (Standard), ohne Schnee | **`2ff794f`** | 63,7 | 35,7 | 30,8 ms | 18,3 ms | 12,6 ms |
| WGPU `exact`, 30 cm Schnee | **`2ff794f`** | 58,9 | 34,3 | 31,6 ms | -- | -- |
| WGPU `all`, 30 cm Schnee | `b3f8c9c` | 69,8 | 50,6 | 22,7 ms | -- | -- |
| GL33 | `b3f8c9c` (21.10. 19:27) | 85,3 | 37,1 | 28,3 ms | -- | -- |
| GL33 | **`2ff794f`** | 86,6 | 37,8 | 28,4 ms | -- | -- |

- **Olis neue Basis macht nichts langsamer.** Gleiche Einstellung (`all`): 77,5 statt 75,5 fps.
  Die CPU-Phasen sind gleich, `land:gnd` liegt bei 0,18 ms (PERF-031 wirkt auch hier).
- **Der Unterschied zwischen 63,7 und 77,5 fps ist allein der Bündel-Modus.** `exact` (Standard
  seit WGPU-033) nimmt MAT-053-Zwillinge nicht in die Bündel auf. Auf dem Deck kostet das ~4 ms GPU
  im 3D-Farbpass (12,6 statt 8,5 ms), und `swap` wartet entsprechend länger. Dasselbe Bild zeigte
  schon Messung 3 auf der alten Basis: `exact` ≈ `0`, nur `all` spart.
- Schnee ist auch auf der neuen Basis ohne Ruckler: Maske 2,7 ms, `cached=1 515 526`, `masked=91 280`
  wie zuvor.
- GL33 ist im Mittel weiter schneller (86,6 fps). Sein schlechtester Abschnitt (37,8) liegt aber
  unter dem von WGPU `all` (57,0).
- Offen (am Ende entscheiden, Vorgabe des Besitzers): `WGR_DIRECT_BATCH=all` als Standard, mit
  Farbabweichung an überlappenden halbtransparenten Stellen. Alternative ohne Abweichung: Zwillinge
  bündeln, solange sich die Bildschirm-Rechtecke der Mitglieder eines Laufs nicht überschneiden.
  Dann ist die Reihenfolge gleichgültig und das Bild identisch (WGPU-034, Vorschlag).

## Messung 6: WGPU-034 (Zwillinge in bildschirm-getrennten Stücken) -- kein Gewinn, zurückgenommen (22.10. 09:54)

Build `6394a65` (CI 39737717502) im Testordner `_OpenPoseidon-WGPU\bin`. `Remastered` blieb
unverändert auf `2ff794f`. Drei Flüge mit demselben Build. Gestartet erst, nachdem die
Bilderserie der Spiegelungen-Sitzung (`before-2ff794f`, 09:21–09:43) fertig war.

| Lauf | fps (Bilder ÷ Zeit) | schlechtester Abschnitt | max. p95 | 3D-Farbpass GPU |
| --- | --- | --- | --- | --- |
| Standard mit WGPU-034 | 64,8 | 35,2 | 31,1 ms | 13,1 ms |
| Standard ohne WGPU-034 (`WGR_DIRECT_BATCH_TWIN_RECT=0`) | 64,0 | 34,9 | 30,9 ms | 12,2 ms |
| `all` | 76,9 | 56,8 | 21,2 ms | -- |

- Die Trace-Zeile erklärt es. `all` fasst ~2 700 Zwillings-Draws in 195 Läufe. WGPU-034 fand nur
  57 Zwillings-Stücke mit ~120 Draws und schnitt je Bild ~2 000–2 500-mal: Aufeinanderfolgende
  Zwillings-Abschnitte (in der Reihenfolge von hinten nach vorn) überlappen sich auf dem Bildschirm
  fast immer, etwa Geländer und Gitterroste, die entlang der Blickrichtung hintereinander liegen.
  Der Beweis „disjunkt, also gleiches Bild" greift deshalb kaum.
- Auch genauere Rechtecke je Abschnitt statt je Modell würden daran wenig ändern: Die Überlappung
  steckt in der Szene selbst.
- Zurückgenommen per Revert-Commit (zusätzlicher Commit, nichts umgeschrieben). Der Code auf dem
  Branch verhält sich wieder wie `2ff794f`.
- Damit bleibt `WGR_DIRECT_BATCH=all` der einzige Weg zu ~77 fps auf dem Deck. Die Entscheidung
  darüber liegt beim Besitzer (am Ende).
