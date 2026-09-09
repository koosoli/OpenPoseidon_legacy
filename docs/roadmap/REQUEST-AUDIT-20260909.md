# Bericht zu allen Auftraegen vom 8./9. September 2026

> Historischer Stand: Runtime a4d09dbe. Der
> [Folgebericht mit Umsetzung, Nachweisen und offenen Arbeiten](REQUEST-AUDIT-FOLLOWUP-20260909.md)
> aktualisiert die seit diesem Bericht angefragten Punkte. Insbesondere Satmap,
> Terrainbearbeitung, Schnee-Dachmaskierung und Fusion sind inzwischen umgesetzt;
> die damaligen Aussagen unten sind keine aktuelle Statusbeschreibung.

## Ergebnis und Verbindlichkeit

**Nicht alles ist vollstaendig erledigt.** Dieser Bericht korrigiert insbesondere
zu weit gehende Abschlussaussagen zu Rauch, Wolken, Leichen und Rotoren. Eine
Codeaenderung, ein bestandener Test und ein visuell bestaetigter Fehlerabschluss
sind drei verschiedene Dinge. Offene Fehler werden hier nicht als erledigt oder
als vom Benutzer abgelehnt umetikettiert.

Arbeitsort war ausschliesslich `.claude/worktrees/asset-obs`, Branch
`asset-obs-001`. Die fremde `Cargo.lock` blieb unangetastet. Der urspruengliche
Handover wurde zuerst gelesen und mit den spaeteren Entscheidungsprotokollen,
dem aktuellen Code und erneuten installierten Tests abgeglichen. Stand dieses
Berichts: Runtime `a4d09dbe`; weitere Aenderungen dieses Berichts sind Dokumentation.

Die kleinen, belegten Anschlusskorrekturen wurden vor diesem Bericht nachgeholt:
Rauch-Gebaeudecache und ein beim Test aufgedecktes Renderthread-Rennen. Groessere
noch offene Probleme samt technischem Grund stehen unten. Das ist keine Behauptung,
dass ihre vollstaendige Behebung unmoeglich waere oder weitere Arbeit unnoetig sei.

## Sichtweite, Nogova und Beleuchtung

| Dein Wunsch | Umsetzung und Pruefung | Grenze / Status |
| --- | --- | --- |
| Kuestenlinien beim weiten Herausfliegen | Hoehere Tiefenpraezision mit `Depth32FloatStencil8`, Faehigkeitspruefung und 24-Bit-Fallback. Die auffaelligen Streifen wurden in den Hoehenaufnahmen auf der RTX 3070 beseitigt. | Umgesetzt; eine andere duenne Wasser-/Meeresbodennaht ist damit nicht automatisch behoben. |
| Viel hoehere Sichtweite, Maximum als Standard, korrekte Game Settings | Gemeinsames Maximum und Werksstandard **50 km**, moderne Game Settings und klassisches Video-Menue angeglichen. Sichtbare Objekte folgen der Sichtweite; AI-Simulation und Schattenreichweite bleiben separat begrenzt. Vorhandene gespeicherte Einstellungen werden nicht bei jedem Start ueberschrieben. Das Besitzerprofil wurde damals unter Testsperre auf 50 km eingestellt. | Umgesetzt. 50 km ist nicht die Zusicherung, jedes Detail gleichzeitig in hoechstem LOD zu zeichnen; komplette Kartenuebersicht bleibt kameraabhaengig. |
| Kein Entfernungsnebel trotz ausgeschaltetem Fog | Fog 0 schaltet den entfernungsabhaengigen Nebel aus. Godrays/atmosphaerische Lichtstreuung sind ein separater Effekt und koennen weiter einen Schleier erzeugen. | Umgesetzt, aber Godrays an bedeutet nicht physikalisch voellig klare Luft. |
| Schwarzes Schild und dunkle Strasse bei den angegebenen Koordinaten auf **Nogova** | DAVLE-Schild/Strassenmaterial untersucht; ungueltige Nullnormale vor dem Normalmapping durch gueltige Normale ersetzt. Transparente/ueberblendete Oberflaechen verwenden ihre eigene Tiefe fuer die Lichtstreuung. | Konkrete Material-/Tiefenfehler korrigiert und aufgenommen. Nicht als Abnahme saemtlicher Buesche/Baeume bei jedem Sonnenstand zu verstehen. |
| Monitor am gehaltenen Black Ops Laser Designator | Falsche Proxy-/Displayzuordnung korrigiert; Gruenphosphor- und geglaetteter Scanline-Effekt auf der Displayflaeche. | Umgesetzt. Kein zweiter unabhaengiger, gezoomter Sensor-Renderpass. Gemeint und bearbeitet war die Geraeteflaeche, nicht nur der Blick durch die Optik. |
| Strassenlampen in Dev Tools einstellen | Im Zeus-Tab unter **Street lamps and light shadows**: RGB, Farbtemperatur 1000-12000 K, Intensitaet 0-8 sowie Pool-/Spill-/Glow-Parameter; bestehende Lampen werden nach Aenderung aktualisiert. | Code und Parameterpruefungen vorhanden. Vollstaendige interaktive Abnahme jeder Sliderkombination ist nicht dokumentiert. Nicht im Weather-Tab suchen. |
| Lampenlicht durch Wagen, Huegel und Haeuser | Verdeckung der sichtbaren Lichtquelle/Glare gegen Objekte und Terrain; Traegerobjekt bei eigenem Fahrzeugscheinwerfer gesondert behandelt. Wagen-/Gebaeudefixture zeigt verdeckte Quelle deutlich unterdrueckt. | Teilweise: Nachweis fuer den Quell-Glare, nicht fuer jeden volumetrischen Kegel und jede beleuchtete Flaeche. Eine separate Huegelaufnahme fehlt. |
| Kopf fehlt im Spielerschatten | Innen-/Pilot-/Gunner-/Cargo-LOD wird fuer passende Schattenwerfer durch die aeussere visuelle Geometrie ersetzt. | Implementiert; bisherige weiche Schattenaufnahme ist fuer den konkreten Kopf keine eindeutige visuelle Abnahme. |
| Menschenschatten im Wasser sehen falsch aus | Von Koerperauftrieb und Kopf-LOD getrennt betrachtet. | Offen: keine eindeutige isolierte Reproduktion. Eine pauschale Abschaltung von Wasserschatten waere ein visueller Rueckschritt, kein Fix. |

Nachweise: [Nogova](decisions/NOGOVA-VISUAL-20260908.md),
[Lichtverdeckung](decisions/LOCAL-LIGHT-GUARDS-20260908.md),
[Wasser/Wolken/Schatten](decisions/WATER-CLOUD-FILTERING-20260908.md).

## Hubschrauber, Rauch und Wolken

| Dein Wunsch | Umsetzung und Pruefung | Grenze / Status |
| --- | --- | --- |
| Andere Helikopterrotoren drehen je nach Blickwinkel mit | Pro-Instanz-Kopien der animierten CPU-Geometrie verhindern das Ueberschreiben gemeinsam genutzter Rotordaten. | Ursache korrigiert. Gestoppter Rotor kontrolliert; bisherige Aufnahmen zeigen die laufenden Fluggeraete nicht hinreichend fuer eine umfassende Abnahme. |
| Rotorblaetter verschwinden in Entfernung | LOD-Fallback behaelt rotierende Blaetter, wenn ein entfernter LOD keine Blur-Geometrie hat. | Teilweise: Stock-UH60 hat Blur-Geometrie in allen relevanten LODs. Dieser Fix allein erklaert seinen gemeldeten Fall nicht; Fernsicht bleibt offen. |
| Smoke-Performance-Regressions, Wind und Downwash erhalten | Dichteaggregation pro belegter Zelle, guenstige Kollisions-Vorpruefung, geteilte Rendergrenzen und einmalige Tiefenberechnung pro Partikel vor dem Sortieren. Wind/Downwash bleiben aktiv. Im dokumentierten Aussenvergleich bei ueber 3800 Partikeln sank die Rauchsimulation von 10,775 auf 7,860 ms, etwa **27 %**. | Reale Verbesserung dieses Tests, keine globale Entwarnung. Innenraumsimulation zeigte weiterhin Spitzen von 12-50+ ms. Downwash-Koexistenz getestet, kein sauber isolierter Downwash-A/B-Nachweis. |
| Rauch soll nicht durch Waende und Daecher clippen | Kontakt-/Raumgrenzen werden in Sprites, Rauchschatten und Volumeninjektion verwendet. Quellraum statt Zielraum bestimmen; integrierte Position statt vorauseilender Position. Beim Berichtsaudit zusaetzlich den 12-m-Gebaeudecache gegen den wirklichen Abfragepunkt im Raumraster abgesichert (`8f2e8a65`). | Teilweise. Sieben Strahlen sind keine exakte Kugel-/Festkoerperkollision; duenne Waende koennen im Volumenfeld weiter lecken. Der neue installierte Volumentest zeigt Aussenrauch und beweist **keine** vollstaendige Wanddichtigkeit. |
| Rauch optisch eher wie Teardown, ohne hohen Mehrverbrauch | Bestehende Partikel-/Volumenpfade verbessert, keine hoeheren Standardbudgets oder neue Fluid-Simulation eingeschaltet. | Kein Teardown-Nachbau. Eine raumaufloesende Stroemungs-/Hindernissimulation braucht eine andere Kosten- und Kollisionsgrundlage; blind hoehere Voxelaufloesung wuerde deiner Performancevorgabe widersprechen. |
| Wolken/Godrays ziehen mit DLSS bei Kamerabewegung nach | Engine-seitige Wolken-History-Maske erweitert. Die anschliessende SDK-Pruefung zeigte jedoch, dass diese Transparenzmaske im aktuellen DLSS-Pfad nicht wirksam konsumiert wird. | **Nicht behoben.** Erforderlich sind passende Wolken-Bewegungsvektoren und repraesentative Tiefe. Nicht auf einen alten DLSS-Preset umgestellt und keinen wirkungslosen Maskenpatch als Erfolg verbucht. |
| Wolken weniger Grain bei geringen Kosten | Tiefengewichteter 3x3-Filter statt vier Taps; fuenf zusaetzliche Samples, kein neuer Raymarch-/History-Pass. | Teilverbesserung. Grain tritt auch ohne DLSS auf. Zusaetzlicher gegenlaeufiger Raymarch-Sampleversuch wurde nach Messung verworfen: rund **0,43 ms** Mehrkosten ohne ueberzeugenden Bildgewinn. |

**Waehrend dieser Nachpruefung zusaetzlich behoben:** Ein Nogova-Start stuerzte
noch vor der Raucherzeugung in `EngineWgpu::DrainInstanceOps` ab. Der Producer
vergroesserte dieselbe Slot-Vector-Speicherstruktur, auf die der Consumer parallel
zugriff. `a4d09dbe` trennt Producer-Handlevergabe und Consumer-Speicherbesitz.
Ungueltige/entfernte Zuordnungen werden zudem abgefangen. Keine zusaetzliche
Hotpath-Sperre und keine abgesenkte Grafikqualitaet. Ein Source-Ownership-Test
schuetzt diese Trennung; er ist ausdruecklich kein ThreadSanitizer-Nachweis.

Nachweise: [Rotor/Rauch/Boot](decisions/ROTOR-SMOKE-WATER-20260908.md),
[Rauchkontakt und Audit](decisions/SMOKE-CONTACT-20260908.md),
[Wolkengrain-Experiment](decisions/CLOUD-GRAIN-EXPERIMENT-20260909.md).

## Wasser, Boote und Kartenrand

| Dein Wunsch | Umsetzung und Pruefung | Grenze / Status |
| --- | --- | --- |
| Boote werden von Wellen zu stark gebremst | Zusaetzlichen wellenbedingten Tiefgangswiderstand kleiner Gleitboote auf 0,25 m begrenzt; Auftrieb und vertikale Daempfung bleiben. Im Test nach 19,1 s: 11,952 statt 4,373 m/s bei Wellenamplitude 3. | Umgesetzt und Fahrtest vorhanden. Keine pauschale Abschwaechung aller Bootphysik. |
| Hochwellenschaum wirkt gekachelt | Nichtperiodische, windorientierte Modulation fuer Wellenkamm-Schaum; Uferschaum nicht ersetzt. | Umgesetzt; gestalterische Qualitaet nicht als endgueltig abgenommen ausgeben. |
| Verpixelter Wasserglanz | GGX-Sonnenreflex mit Normalvarianz-Antialiasing; keine zusaetzlichen Texturzugriffe oder FFT-Paesse. | Umgesetzt, Filter-/Bildpruefungen vorhanden. |
| Leichen sollen oben schwimmen, nicht fliegen oder unter Wellen verschwinden | Tote Infanterie bekommt kontinuierliche Wasserunterstuetzung, 0,12 m Eintauchtiefe und begrenzten Aufstieg; eingefrorene Wasserabfragen und lebende Spielerauftriebslogik fuer Tote korrigiert. | **Nur teilweise.** CPU-Sechswellenmodell und GPU-FFT-Oberflaeche stimmen nicht exakt ueberein. Dein spaeterer Hover-/Unterwasserbericht ist deshalb nicht abgeschlossen. Ein konstanter Hoehenoffset kaschiert nur eine Wellenphase; synchrones GPU-Auslesen koennte den Frame blockieren. Gemeinsame bzw. zeitlich kompensierte Wasserabfrage ist der notwendige Anschluss. |
| Kampagnenfiguren ertrinken beim Ein-/Ausladen von Booten | Zuerst getrennt untersucht, danach deinen Tester-Widerruf uebernommen. | **Zurueckgenommen**, keine unnoetige Boarding-Aenderung. Bootladen/-entladen wird hier nicht als behobener Bug verkauft. |
| Schussringe machen danach einen flachen Wasserbereich | Ripple-Neigung wird zur vorhandenen Wellennormalen addiert, statt diese bis 95 % durch eine flache Ripple-Normale zu ersetzen. Keine zusaetzlichen Samples/Paesse. | Umgesetzt; analytische Tests und installierter Interaktionsvergleich. Eine exakt aufgezeichnete Einzelschussfolge fehlt. |
| Physics-Tab-Objekte sollen schwimmen/sinken und beim Einschlag/Rollen splashen | Auftrieb nach Masse **und verdraengtem Volumen**, Wasserwiderstand/Daempfung, trockene Bereiche ausgeschlossen. Energie-/groessenabhaengige Spritzer, Ringe und Schaum mit Ereignislimit/Hysterese. Gleiche Kugel: 100 kg schwimmt, 1000 kg sinkt; installierte Eintritte getestet. | Umgesetzt als Naeherung. Masse allein entscheidet physikalisch nicht. Keine Hohlkoerper-Flutung, keine vollstaendige Form-/Kenter-Simulation; dieselbe CPU-/FFT-Abweichung bleibt. |
| Sichtbarer Kartenrand beim Herausfliegen | Karte enthaelt endliches Land; zusaetzlich endete vorher das Wasser-Rendergebiet. Guenstige prozedurale Wasser-CDLOD-Fortsetzung ausserhalb des Kerngebiets, bei 60 km Entfernung und negativer Ecke geprueft. | Umgesetzt fuer den Wasserabbruch. Erfindet keine neue Insel/Geographie. Gemessener Zusatz im nahen Kuestentest rund 0,15 ms, nicht kostenlos. |
| Meer wie Waterline-Video mit kombinierbaren Spektren | Bestand geprueft: bereits vier FFT-Kaskaden, JONSWAP/TMA, Gerstner-Fallback, Schaumadvektion und Interaktionen. | Bewusst kein beliebiger Vier-System-Mischer mit Phillips/PM/Custom/Cache. Vier Kaskaden sind nicht vier austauschbare Ozeansysteme. Zusatzsimulation, Cachebandbreite und Energie-/Wetteruebergaenge sind ohne gemessenen Qualitaetsgewinn nicht gerechtfertigt. Deine Bedingung war geringe Mehrkosten, nicht Funktionsparitaet um jeden Preis. |
| Automatische Satmap fuer Fernsicht, falls sinnvoll | Authored-Satmap-Fernblend existiert bereits fuer moderne Karten. Klassische Karten verwenden gefilterte Materialtexturen. Nogova-Terrainfarbe kostete im Test nur rund 0,349 ms. | Automatischer Classic-Baker begruendet nicht hinzugefuegt: begrenztes Einsparpotenzial, etwa 85 MiB bei 4096 bzw. 341 MiB bei 8192 unkomprimiert inklusive Mips, neue Blend-/Farbfehler moeglich. Die behauptete universelle automatische Arma-Erzeugung wurde nicht als erwiesene Tatsache uebernommen. |
| Seltsame Schatten beim Schuss in Desert-Boden | Normale Einschlagstaubpartikel erzeugen keine dichten Rauchschattenblobs mehr; rauchige Explosionen bleiben berechtigt. | Umgesetzt und echter Desert-Geschosseinschlag vorher/nachher geprueft. Krater-/Staubdarstellung bleibt erhalten. |

Nachweise: [Leichen](decisions/INFANTRY-WATER-20260908.md),
[Wasserringe](decisions/WATER-RIPPLE-20260908.md),
[Physics-Wasser](decisions/PHYSICS-PROBE-WATER-20260908.md),
[Ozeanfortsetzung](decisions/OCEAN-CONTINUATION-20260909.md),
[Satmap-Abwaegung](decisions/TERRAIN-SATMAP-ASSESSMENT-20260909.md),
[Desert-Einschlag](decisions/DESERT-IMPACT-SHADOW-20260908.md).

## Schnee, Sandbox, Upstream und Performance

**Experimenteller Schnee:** Im Weather-Tab zuschaltbar, standardmaessig aus.
Schneefall, Tiefe/Rate/Maximum, 10 cm hinzufuegen und Leeren. Schneedecke waechst;
Personen/Fahrzeuge hinterlassen persistente Spuren, Granaten Vertiefungen. Neue
Schneemenge fuellt Spuren wieder auf, auch wenn die globale Maximalhoehe erreicht
ist. Fahrt 12 km weg und zurueck sowie erneutes Zuschneien im installierten Spiel
geprueft. Sparse CPU-Speicherung verwirft belegte Zellen nicht heimlich; feine
GPU-Darstellung folgt der Kamera in einem 64-m-Fenster. Terrain-Pass im kurzen
Test 0,326 auf 0,357 ms, kein allgemeines Hardwareversprechen.

**Wichtige Schneegrenzen:** Experimenteller Terrain-Effekt, nicht Schnee auf
Daechern, Strassen, Fahrzeugen und Vegetation. Ansammlung wird unter Daechern
noch nicht ausgeschlossen. Keine angepasste Kollision/Fuss-/Radhoehe; tiefer
Schnee kann Geometrie verschlucken. Keine Savegame-/Netzwerkpersistenz, kein
Schmelzen oder seitliches Umverteilen. Speicher hat eine feste Kapazitaet.
Persistente Spuren bedeuten hier waehrend der laufenden Mission, nicht unbegrenzt
ueber Spielneustarts. [Details](decisions/EXPERIMENTAL-SNOW-20260909.md).

**Zerstoerbares Terrain:** Wie erbeten Machbarkeit untersucht, nicht implementiert.
Eine sichtbare Hoehenmulde allein waere falsch: Hoehen-/Normalencaches, CDLOD,
Kollision, Strassen, AI-Navigation, Schatten, Savegames und Netzwerk muessen
denselben Zustand sehen. Der bestehende HeightChange-Pfad erledigt das nicht.
Hoehlen brauchen zudem etwas anderes als ein reines Hoehenfeld. Daher kein
visueller Scheinkrater als fertige Terrainzerstoerung ausgegeben.

**Showcase-Sandbox:** Installierte Einzelmission **Showcase Lab - Physics and
Light** auf Desert: 42 Box3D-Koerper, Stapel/Dominos, Werfen/Ablegen/Reset/Leeren,
warme/kalte/bewegte Lichter, Tag/Nacht, Flat-/Normal-/Gloss-Vergleichsflaechen,
Jeep/Ural/UH60. Eigene Testassets, kein Kopieren fremder Videoassets. Mission aus
dem installierten Menue gestartet und Aktionen/Physikkontakte getestet. Es ist
eine Showcase-Mission auf bestehender Karte, **keine neue WRP-Karte** und keine
1:1-Rekonstruktion des Videos; dessen Einzelbilder waren nicht inspizierbar.
[Details](decisions/SHOWCASE-LAB-20260909.md).

**Upstream:** Selektive Pruefung gegen `upstream/main` **3541affa**.
Scene-Preloader-Reset aus `a61130ca` an unseren Teardown angepasst; echter
In-Process-Remount mit 20 belegten Slots vorher/nachher bestanden. Mehrere andere
Fixes waren bereits vorhanden oder trafen unsere abweichenden Pfade nicht.
GL33-Ressourcen-/State-Fixes nicht blind in wgpu portiert. Sortierung,
Light-Instancing, Conform und Cursor-Skalierung brauchen eigene Vergleichstests:
kein konflikt-/regressionsfreier Bulk-Merge versprochen. Das war keine lueckenlose
Pruefung aller historischen Commits. [Commitliste und Gruende](decisions/UPSTREAM-REVIEW-20260908.md).

**Native Reforger-Performance ohne Qualitaetsreduktion:** Wirklich die nativen
Archive verwendet, nicht die konvertierte Karte. Erst einen 50-km-Guard behoben,
der Terrain **und Objekte** uebersprang; leere Szenen sind keine schnellen
Benchmarks. Danach nutzlose Vorbereitung konvertierter Loose-EMAT-Texturen fuer
native Archivmodelle entfernt. Zwei Vergleiche: etwa **7 Sekunden Hintergrund-
Texturarbeit auf 0,93 ms**, **351-447 MB belegter CPU-Vorbereitungsspeicher auf
0**, weiterhin 496 uebernommene Modelle / 22.360 registrierte Instanzen. Das sind
keine sieben Sekunden weniger pro Frame; GPU-Residency blieb rund 1835 MB.
Village- und Coast-Pruefungen vorhanden. Keine Sichtweite, Grasmenge, Aufloesung
oder Schattenqualitaet reduziert.

Eine Solid-Shadow-Shader-Abkuerzung nach mehrfacher Messung wieder entfernt:
Gewinn im Bereich der Streuung. GPU-FPS-Gewinn nicht behauptet, weil der normale
LOD-Regler zwischen Laeufen unterschiedlich viel Geometrie zeichnet. Teure ferne
Schatten und echte Upload-/Dekompressionsarbeit bleiben Optimierungsfelder.
[Messungen und Fallen](decisions/NATIVE-REFORGER-PERF-20260909.md).

## Zusaetzliche Punkte aus dem urspruenglichen Handover

| Handover-Punkt | Aktueller Befund |
| --- | --- |
| Scheinwerferbirne wirkt nicht leuchtend | Selbstverdeckung, falsche Softparticle-Behandlung und Marker verbessert; kompakte acht additive Lagen statt vergroesserter Nebelscheibe. Aktuelle Aufnahme weiterhin zu dunkel/undeutlich fuer eine Abnahme als leuchtende Linse. Nicht abgeschlossen. Testfahrzeug meldet ausserdem leeren Treibstoff; diese Fixture darf keine starke Helligkeitsaussage tragen. |
| Wasser-Speckles in Hoehe | Tiefenpraezision und Reflexfilter umgesetzt, siehe oben. |
| LGT-025-Basis-Guard | Kamera-/Schattenursprung-Abweichung ueber einen Meter bzw. ungueltige Werte werden diagnostiziert. Umgesetzt. |
| Lokales Schattenbudget erhoehen | Nicht pauschal erhoeht: 24 Views entsprechen vier Punktlichtern. Vorhandener Atlasplatz macht die verbleibenden rund 1,25 ms pixelabhaengige Abfragearbeit nicht kostenlos. Eigener belastbarer Budgetvergleich waere Voraussetzung. |
| Farblose/schattenlose Innenraeume | Aktuell nicht eindeutig reproduziert. Die bereits gemessene Two-Sided-Normal-Aenderung machte das Bild dunkler und wurde verworfen; nicht erneut eingebaut. Zwei-Kamera-Abnahme bleibt offen. |
| Fernimpostors auf klassischen Inseln | Pfad erhaelt dort weiterhin nicht die modernen Placements. Kein neuer Classic-Impostorpfad: ohne Material-/Silhouetten-/Schattenabnahme drohen sichtbare Auslassungen. Offen, kein behaupteter Performancegewinn. |
| HRTF und Mousewheel | Bereits im Handover geparkte manuelle Audio-/Bedienabnahmen unveraendert offen. Keine erdachten Hoer- oder Eingabetests. |

## Was bewusst nicht als Abschluss zaehlt

Die begruendeten Nichtuebernahmen sind der automatische Classic-Satmap-Baker,
beliebiges Ozeanspektren-Layering, teurere erfolglose Wolkensamples, pauschal mehr
Lokalschatten, blinde GL33-Ports und der nicht ueberzeugende Shadow-Shader-Versuch.
Terrainzerstoerung war eine Machbarkeitsfrage; Bootboarding wurde von dir widerrufen.

Davon zu unterscheiden sind **echte offene Fehler**: FFT-genaues Leichenschwimmen,
Wolkenbewegungsdaten fuer DLSS, Rauch an duennen Waenden und Innenraumspitzen,
Fernrotoren sowie eindeutige Kopf-/Wasserschatten- und Lichtkegelabnahme. Die
genannten technischen Risiken erklaeren, weshalb kein schneller kaschierender
Patch eingebaut wurde. Sie sind keine Benutzerfreigabe, diese Fehler dauerhaft
fallen zu lassen. Fuer einen ehrlichen vollstaendigen Abschluss fehlen diese
Arbeiten; dieser Bericht stellt das ausdruecklich fest.

## Audit-Verifikation

- `PoseidonGame` und `PoseidonTests` erfolgreich gebaut.
- 98 gezielte Testfaelle, 29.353 Assertions bestanden: Instance-Ownership,
  Gebaeuderaeume/Rauch/Wind/Airflow, Leichen-Wasserunterstuetzung,
  Schattenmathematik, Sichtweite und Settings. Kein kompletter Gesamt-CI-Lauf.
- Build `a4d09dbe` als zusammenpassendes EXE/DLL-Paar installiert und zuvor
  nach `safety/asset-obs-001` sowie `safety/consolidated-v2` gepusht.
- Nogova, jeweils 30 Sekunden, Wind 18 und achtfache Rauchdichte: Sprite- und
  echter Volumenmodus nach dem Ownership-Fix ohne protokollierten Crash beendet.
  Volumenaufnahme inspiziert, sichtbarer Aussenrauch bleibt. Spaete einzelne
  Simulationssamples etwa 9,8-13,9 ms sind kein p95 und kein kontrollierter
  allgemeiner Performancebeweis.
- Zwei native Reforger-Village-Wiederholungen bestanden: je 22.360 registrierte
  Instanzen, Terrain vorhanden, Streaming abgearbeitet, regulaerer Shutdown.
  Gemeldete Framewerte 23,314 und 22,377 ms; dies ist eine Stabilitaetskontrolle,
  kein neuer kontrollierter Speedup-Nachweis. Zweite Aufnahme visuell inspiziert.
  Insgesamt vier installierte Laeufe nach dem Ownership-Fix ohne erneuten Crash;
  das ersetzt keinen Langzeit-/ThreadSanitizer-Test.
- Fremde `Cargo.lock`: erwarteter SHA256
  `9A4E74D791B26A4303E064867DA67B862C1439D0635DF3E19ED5A97AADF31A2E`.

Lokale Rohbelege liegen in `.tmp-bulb/audit-*.log`,
`.tmp-nogova/audit-*.{log,png}` und
`build/native-reforger-perf/Village-audit-instance/`. Die Entscheidungsdokumente
enthalten zusaetzlich die vorherigen Tests und Vergleichswerte. Diese lokalen
Artefakte sind keine Zusage, dass sie in einem frischen Clone vorhanden sind.
