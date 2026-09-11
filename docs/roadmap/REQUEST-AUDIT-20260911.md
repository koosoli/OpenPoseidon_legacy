# Anforderungspruefung, 11. September 2026

## Nachtrag 12. September: munitionsabhaengige Einschlagkrater

Besitzer nimmt den pauschalen Faktor acht zurueck. Installiert 61bbb0c6:
Referenztiefe wieder 1 m statt 8 m, Referenzradius weiterhin 12 m. Beide
Dimensionen skalieren mit clamp(cbrt(indirectHit / 150), 0.25, 2).
150 entspricht der tatsaechlich geladenen klassischen LAW. Verwendet die
geladene Munitionskonfiguration, keine Namensliste; Schadenswerte sind hier
eine Gameplay-Naeherung, kein TNT-/Bodenmaterialmodell. Gewehrkugeln,
Wasser-/Objekttreffer und Mehrspielerregeln unveraendert. Grobes Terrainraster
kann kleine Radien weiterhin verbreitern. Dev-Regler nennen die LAW-Referenz;
Settings- und Master-Reset benutzen wieder die normale Basistiefe.
Build und 647 Assertions / 4 Tests bestanden. Installierter LAW-Test:
29.835 -> 28.835 m, ausgeschaltet unveraendert, Settings-Reset/Restore und
erneuter Treffer wieder 1 m, abschliessend Originalhoehe hergestellt.
Beleg: build/rocket-craters/acceptance-20260912-005648.
Shell105 mit indirectHit=110: 29.835 -> 28.9332 m (0.9018 m Tiefe),
derselbe Aus/Reset/Restore-Zyklus bestanden, sauber beendet. Beleg:
build/rocket-craters/acceptance-20260912-005750. Niedrigerer Blast als LAW
bedeutet hier einen kleineren Krater, unabhaengig vom hohen direkten Panzerschaden.
Erster Testversuch verwarf einen nicht unterstuetzten getNumber/configFile-
Diagnosebefehl; korrigierter Test berechnet die erwartete Tiefe aus der
Explosionswirkung im echten Einschlaglog. Kein neuer Skriptbefehl noetig.

## Aktueller Nachtrag 20:32 (kein Gesamtabschluss)

2026-09-12: Besitzer gibt feste Reihenfolge vor: Satmaps mit Wald/Doerfern,
danach Stratis-Godrays, danach aktualisierter Bericht. Kein Aufgabenwechsel.
Satmap-Objektbaker in 5ce3f450 gebaut (650 Assertions / 5 Tests bestanden),
in c00bf1eb nach Bildvergleich standardaktiviert und installiert (00:30).
Abschalter WGR_OFP_SATMAP_OBJECTS=0 behaelt den Boden-only-Pfad.
Statische Primary/Network-Modelle, erste zeichnbare LOD, UV/Alpha aus echten
Texturen, 64x64 CPU-Draufsichten pro Modell, Platzierung im vorhandenen Boden-
Satmapbild. Keine neue GPU-Texturverwaltung, kein eingebranntes dynamisches Licht,
keine beweglichen Einheiten. Kleine Projektionen werden flaechengewichtet.
Temporaere Caches auf je 2048 Modelle/Texturen begrenzt; vollstaendige
Deckung von Waldmodellen und Bildqualitaet sind noch zu pruefen. Zerstoerung
nach dem Bake wird noch nicht in die statische Ferntextur nachgefuehrt.
Nach Freigabe installiert 050c8ffb (quellgleich zu 5ce3f450), vier Fusion-
Laeufe mit/ohne Objektbake bei 38 km und 4 km Hoehe sauber beendet. Objektbake:
721 Modelle, 1289 Texturen, 405047 Instanzen, 667638 Samples, 2773 ausgelassen.
Zwei ON-Laeufe: 737.5 / 624.0 ms zusaetzlicher CPU-Bake beim Weltladen;
erster gesamter Satmap-Bake 1535.3 ms. Keine FPS- oder Peak-RAM-Aussage.
Native 1280x720, MSAA4, DLSS aus, gleiches frisches Grafikprofil.
Belege: build/fusion/acceptance-20260912-000824 (38 km ON),
acceptance-20260912-001116 (4 km ON), acceptance-20260912-001246 (4 km OFF).
3D-Waldkacheln ueberlagern die Draufsicht weiter: das ist kein Beweis fuer
vollstaendige Baumkronen in der Textur. 49238fe2 ergaenzt deshalb optionalen
PNG-Export WGR_OFP_SATMAP_DUMP / Test-Fusion -DumpSatmap, installiert und
650 Assertions / 5 Tests bestanden. Der Export veraendert den Renderpfad nicht.
Standardlauf OHNE Opt-in: build/fusion/acceptance-20260912-003021,
alle fuenf Inselbereiche und 40-km-Uebersicht, sauber beendet. Punktuelle
Working Sets bis 2200 MiB, kein Peak-RAM/VRAM-Nachweis. Reine Albedo-Exporte
von Fusion ON/OFF und Everon visuell kontrolliert: Objekt-/Dach-/Waldstrukturen
vorhanden. Regelmaessige OFP-Waldgeometrie bleibt; keine HLOD-Einsparung.
Grenzen und Belege: decisions/OFP-OBJECT-SATMAP-20260912.md.

Godrays wieder offen: Stratis [1949.76,5046.07,32.37], az74.4/el-1.3, 06:03,
Besitzer-Screenshot 2026-09-11 233844 und Startkommando bestaetigen den Fall.
627317e8 enthaelt eine AUSGESCHALTETE Cutout-only-Replay-Variante
(WGR_EARLY_LATE_OPAQUE=2), jetzt auf installiertem c00bf1eb runtimegeprueft.
Siehe decisions/STRATIS-CUTOUT-RAYS-20260912.md: der erste Eindruck einer
grossflaechigen Hangabdunklung wurde durch Pixelkontrollen widerlegt. Die
Baumkronen verlieren jedoch sehr stark Kontrast; visuelle Besitzerentscheidung
angefragt, noch KEINE Standardaktivierung. Alter Late-Surface-Versuch bleibt
aus; dessen fruehere visuelle Bewertung nicht ungeprueft als Messung behandeln.
Fusion-Pose [36154.60,23965.44,38053.39], az262.5/el-60.7: frische Testprofile
mit/ohne Boden-Satmap zeigten bei 38 km Hoehe keine komplett blauen Inseln.
Das widerlegt den Besitzerbericht nicht; Profil-/Laufzeitabhaengigkeit offen.

23:35: Auf Besitzerwunsch ballistische Krater standardmaessig achtfach tief:
8 statt 1 m, Radius weiterhin 12 m (Terrainraster-Mindestabdeckung unveraendert).
Panzer-/Aufschlaggranaten mit AmmoShotShell ohne Zeitzunder verwenden jetzt
denselben Pfad wie Raketen. ShotBullet erbt von ShotShell; deshalb ausdruecklich
Munitionssimulation statt Basisklassen-Cast pruefen. Gewehrkugeln und zeit-
gezuendete Handgranaten unveraendert. Bestehende Bedingungen bleiben: explosive
Munition, Bodentreffer ohne direkt getroffenes Objekt, trockenes Land, SP.
Dev-Tab bezeichnet Schalter und Regler jetzt als ballistische Einschlagkrater;
Reset benutzt den neuen Standard. Kein Mehrspielernachweis und keine Aenderung
von Schaden, Schussphysik oder Radius. Hoehenfeld bleibt rasterabhaengig.
Installiert d6f60ff0, 23:32. Build und 643 Assertions / 4 Tests bestanden.
Installierte LAW- und Shell105-Geschosse: 29.835 -> 21.835 m; ausgeschaltet
unveraendert, nach Einstellungsreset und Terrainrestore erneut 8 m tief,
abschliessendes Terrainrestore auf 29.835 m; sauberes Beenden.
Belege: build/rocket-craters/acceptance-20260911-233301 (Shell105) und
acceptance-20260911-233405 (LAW). Bild des echten Kraters geprueft.
Andere Panzer-Munitionsvarianten und grosse Einschlagserien noch nicht vermessen.

23:26: Everon-Godrays/Baumfall nach Ansicht der aktuellen Bilder vom Besitzer
visuell akzeptiert ("ich sehe kein problem mehr"). Keine neue Shaderkorrektur
hierfuer vorgenommen. CLI-A/B auf installiertem 88542064, native 1280x720,
MSAA4, DLSS aus, feste Belichtung und festgesetzter LOD-Regelbereich:
beide 7157 sichtbare Instanzen und identische LOD-Verteilung
2231/3687/1156/63/18/2/0/0. Entfernte Waelder reagieren sichtbar auf Godrays.
Belege: build/foliage-rays/acceptance/everon-fixed-{control,rays}-88542064-*.
Der zusaetzliche Player-Freefly-Lauf zeigt eine andere Sonnenstellung und ist
deshalb kein zeitgleicher A/B-Nachweis; ebenfalls keine FPS-Aussage ableiten.
Die zwei ersten --test-world-Versuche wurden wegen Ladefehler verworfen:
klassisches Everon muss ueber die Mission geladen werden, der Override
unterstuetzt hier nur moderne WRP/native Daten. Test-FoliageRays erweitert
statt eines zweiten Testsystems. Keine pauschale Abnahme aller Lichtfehler,
keine Aktivierung der zuvor verworfenen Late-Surface-/Early-Opaque-Versuche.

23:00: Installiert 88542064. M113-Tiefschneekontakt im Stand und bei Fahrt
geprueft: 0.65 m Freilegung in 1 m Schnee, breite Rinne, ausserhalb unveraendert,
erneutes Zuschneien bestanden. UAZ-Regressionslauf ebenfalls bestanden. Kein
Fahrphysik-Umbau; konservative geometrische Naeherung, Kanten noch kantig und
Grossflottenkosten noch nicht gemessen. Details im Snow-Deep-Contact-Decision.

22:54: Installiert 804ad7d4. Reforger-Vorladeversuch viermal (ABBA) und mit
zwei Dorfansichten geprueft. Keine Material-/Geometrieaenderung; einzelne
Maximalspitzen kleiner, uebrige Framezeiten gemischt. Deshalb NICHT als neue
Standardoptimierung aktiviert. Details: NATIVE-FOREST-CHECK-20260911.md.
Kettenfahrzeug-Tiefschneekontakt in Arbeit, noch nicht als abgenommen markieren.

22:24: Installiert 9c9e1607. Upstream-Cursoranpassung im installierten Spiel
bei 800x600, 1280x720, 1680x720 mit Menue/Editor/Kinddialog geprueft; Bildmasse
und quadratische Cursor stimmen. Ultrawide behaelt das vorhandene zentrale
UI-Band. Reforger-Ladespitzen weiter offen: aktuelle Fahrt auf 142b6d6a hat
P99 89.410 ms und Maximum 160.093 ms; davon im schlimmsten Frame 156.476 ms
Residency. Kein neuer FPS-Gewinn behauptet. Details im Native-Forest-Decision.
Jeep bleibt auf Besitzerwunsch geparkt; Satmaps haben weiterhin keine Doerfer
oder Baumkronen. Diese Dokumentation bedeutet nicht, dass jene Ziele fertig sind.

Installiert 142b6d6a: TrackOptimizedFour verbreitert tiefen Schnee zwischen den
vier Radkontakten, 10 Hz, 0.35 m visuelle Unterboden-Naeherung. Flacher Schnee,
Motorrad-/Panzerpfad und Fahrphysik unveraendert. Build und 10012 Pruefungen
bestanden. UAZ steht/fahrt/entfernt/Wiederauffuellen bestanden, Nahaufnahme zeigt
eine geometrische Rinne: build/snow-crawl/acceptance/crawl-20260911-214031.
Rinne noch kantig; kein vollstaendiger physikalischer Verdraengungsnachweis und
keine gemessene Performanceverbesserung. Der erste Lauf hatte eine zu ferne
Kamera und bereits fahrenden NPC. Der korrigierte Ruhetest deckte den bisherigen
isStopped-Skip auf; Schneekontakt liegt jetzt ausserhalb dieses Bewegungszweigs.

21:02: Installiert 7329e2c4, kompletter Build und 10006 Unit-Pruefungen gruen.
Zusatzfreilegung am animierten Augenpunkt ist kein nachgewiesener Fix fuer den
Untergrundblick; dieser bleibt offen. Rotor-Wiederholung auf c529af69 bestanden:
117 Updates, Mittel 0.20 ms, P95 0.40 ms pro Update (4 Hz), ein Helikopter.
Keine Aussage zu allgemeinem FPS-Gewinn. Reforger-Waldmessung und verbleibende
Lade-/GPU-Kosten: decisions/NATIVE-FOREST-CHECK-20260911.md.

20:49: Installiert 000aa2fe. Rotorabtrag am Boden ist jetzt implementiert und
mit besetzter UH60 geprueft: Motor aus unveraendert, Motor an schrittweise frei,
Umgebung bleibt stehen, Niederschlag fuellt wieder auf. Kein separater
aufgewirbelter Schneepartikel-Effekt; Mehrhelikopter-/Hangabnahme noch offen.

- Installiert bd1f889c: tiefer Schnee beim aufrechten Gehen erzeugt jetzt eine
  durchgehende Rinne statt einzelner Abdruecke. Ein-Meter-Test, Wiederauffuellen
  und neue Spur sichtbar geprueft; Rander noch kantig. Fahrzeug, Rotorwind und
  interner Untergrundblick bleiben offen.
- Genannte Default-Pakete und Fusion/Showcase/SizeProbe liegen unter Mods/@OP_*.
  Sandbox-Menue, volle Sandbox-Pruefung und Fusion-Inseln bestehen mit sauberem
  Beenden. Vorbereitete Sandbox-Mitfahrstation ist installiert; dies beweist
  keine FFV-Bedienbarkeit. Restliche lose Rendererassets separat auditieren.
- Jeep-FFV ist auf ausdrueckliche Besitzeranweisung geparkt. Keine weiteren
  langwierigen Animations-/Waffenarbeiten; historische Harness-Erfolge widerlegen
  den gemeldeten Bedienfehler nicht. Siehe PARKED.md.
- Die folgenden aelteren Abschnitte sind historische Nachweise. Satmaps mit
  Objekten, Reforger-Optik/Streaming und sonstige offene Ziele bleiben offen.

**Nicht alles fertig. Dies ist eine Zwischenpruefung, kein Abschlussbericht.**
Quellstand: `8152607f`, asset-obs-001, nach Fetch identisch mit
`safety/consolidated-v2`. Installierter Stempel: `e1738ab4`, 03:39, Deploy.ps1.
Diese Pruefung aendert keine Spielfunktion und erzeugt keine neuen Laufzeitmessungen.
Cargo.lock und fremde temporaere Verzeichnisse bleiben unangetastet.

Die alten Berichte vom 9. September sind historische Momentaufnahmen. Fuer
spaetere Implementierungen gelten die neueren Eintraege im
[chronologischen Nachweis](SEPTEMBER9-VISUAL-FOLLOWUP.md), nicht dessen inzwischen
ueberholte Zwischenstaende. Ein gezielter bestandener Test bedeutet nicht, dass
alle spaeter gemeldeten Varianten behoben sind. Restpunkte unten bleiben
autorisiert; weder Aufwand noch ein erfolgloses Experiment streichen den Wunsch.

## Zwei ausdruecklich nachgepruefte Luecken

Erneut bestaetigter Auftrag am Nachmittag: Tiefschnee-Verdraengung von
Koerpern/Fahrzeugen, Prone-Untergrundblick, Rotorwash-Abtrag und unsichtbarer
Spieler beim Freefly-/Editor-Wechsel bleiben offen. Ebenso echte Hoehlen,
Fusion-Bearbeitung, Licht/DLSS/Waldfehler und vollstaendige Materialabnahme.
Native Reforger-Optik ist gleichrangig mit Performance: Wald-/Bodenkontrast,
Materialtiefe, Baumqualitaet gegen die konvertierte Karte und zusammenhaengende
Stamm-/Kronenbewegung. Zusaetzlich die sichtbare Objekt-Ladegrenze pruefen und
entfernte Objekte in geeigneten niedrigen LODs bereitstellen. Kein Nebel oder
reduzierte Sichtweite als Ersatz; vorhandene Distanzpfade zuerst messen.

### Satmaps mit Doerfern und Waeldern: fehlen

`engine/WgpuRenderer/TerrainWgpu.cpp`, `TerrainWgpu::BuildAutomaticSatmap`,
liest die Landschaftstexturen ueber SourceName/DecodePAABuffer, verkleinert
sie auf 16x16 Texel und kombiniert sie anhand des Land-Texturrasters.
Das ist eine vorhandene automatische **Boden-Fernalbedo**, auch fuer Fusion.
Der Pfad projiziert keine Gebaeude, Baumkronen oder sonstigen Weltobjekte.
Waldmuster, die schon in Bodenbildern enthalten sind, sind kein Objekt-Bake.
Die geforderte Darstellung von Doerfern und Waeldern ist damit NICHT geliefert.

Naechster Durchstich: eine begrenzte echte OFP-Testregion aus vorhandenem
Objekt-/Materialbestand in eine unbeleuchtete Fernfarbdarstellung aufnehmen;
keine gruene Waldmaske oder erfundene Gebaeuderechtecke als Fertigstellung.
Zuerst Draufsicht und schrager Uebergang pruefen. Danach getrennt vereinfachte
Geometrie/Impostors fuer Silhouetten untersuchen: eine flache Satmap ersetzt
keine sichtbaren Baum- oder Hauskonturen. Nahgeometrie nicht voreilig ausblenden.
Speicher, Erzeugung und tatsaechlich entfallende Draw-/Materialarbeit getrennt
messen. Solange der teure Objektpfad weiterlaeuft, keine FPS-Ersparnis behaupten.

### Jeep-Passagierschiessen: SP-Durchstich geliefert, Restabnahme offen

Aktualisierung nach installiertem 65dc2a63: Das eigene Paket
`@OP_VehicleActions/AddOns/op_jeep_actions.pbo` wird normal automatisch geladen.
`content/default-packs/vehicle-actions` und `scripts/default-content` enthalten
eigene bearbeitbare Posequellen und Generatoren. Original-Jeep, Cargositz 0,
persoenliche M16: NPC und Spieler nutzen denselben vorhandenen Waffenpfad.
Keine montierte MG-Ersatzfunktion, kein angehefteter externer Soldat.

Installiert bestanden: echte Treffer/Munition, Hindernis/Feuerverbot,
automatische Sitzanimation, Nachladen, Speichern auch mitten im Nachladen,
Spielersteuerung, Ausstieg/Sitzwechsel/Tod/Fahrzeugzerstoerung und gerade Fahrt.
Frisches Release-Verzeichnis 125134 besteht zusaetzlich automatisches Laden,
beide SP-Pfade, Abschalten, fehlendes/inkompatibles Paket und Benutzer-Mod-Vorrang.
Originalinstallation und deren PBOs wurden nicht veraendert.

Aktueller Fahrtest a22ce887 / 155706: schnellere sitzspezifische Nachfuehrung
reduziert seitlichen Fehler auf etwa ein Grad, aber keine Schuesse; damit keine
neue Fahrabnahme. Historische gerade Fahrt oben ersetzt diesen aktuellen Test
nicht. Zwei stehende Jeeps und gespeicherter Originalpose-Fallback nach
Paketabschaltung wurden inzwischen erfolgreich geprueft (141650/133911).

Offen: Kurven/Unebenheiten/mehrere fahrende Jeeps, feinere Animationsabnahme,
kontrollierte Zusatzkosten und echter Multiplayer-Fallbacktest. MP ist im Code
weiter auf Originalverhalten begrenzt, nicht als FFV freigegeben. Laden alter
Spielstaende nach Abschalten des Animationspakets wurde im isolierten Fall
133911 geprueft; weitere Missionsvarianten bleiben ungetestet.
Siehe [chronologische Implementierung und Tests](decisions/JEEP-FFV-20260911.md).

## Uebrige Anforderungen und offene Abnahme

Die folgenden Zeilen verdichten bestehende Code-/Testprotokolle. Anders als die
beiden obigen Pfadpruefungen sind sie keine erneute Laufzeitabnahme von HEAD.
"Teilweise" bedeutet explizit NICHT, dass der ganze Auftrag fertig ist.

| Bereich | Belegter Stand | Noch zu erledigen / Abnahme |
| --- | --- | --- |
| Fusion aller OFP-Inseln | Flache 51,2-km-Welt mit Everon, Malden, Kolgujev, Nogova, Desert und installierten Besuchen vorhanden. | Interinsel-Fernsicht, dunkle entfernte Inseln, Quellgebietsgrenzen und fehlende ferne Waelder nicht geschlossen. |
| Groessere Welten | Objektarme 102,4-km-Testmission laeuft; kein pauschales 50-km-Startlimit mehr. | Dichte 100-km-Welten, Kollision, Save/Load und MP; absolutes Maximum nicht ermittelt. |
| Sichtweite / Meergrenze | 50-km-Einstellungen und Offmap-Wasser-/CDLOD-Korrekturen vorhanden. | Vollstaendige interinsulare Ansicht und alle spaeteren Kachel-/Kuestenfehler pruefen; keine Nebelkaschierung. |
| Hoehenmalen | Map-Editor-Tab, kontinuierliches Raise/Lower, Cursor-Radius, Rad/Alt-Anbindung, Aenderungsjournal und Wiederherstellung vorhanden. | Effektives Minimum weiter rastergebunden; kleine Radien sind nicht automatisch feine Geometrie. Fusion-Raster ueber 2049 weiterhin gesperrt. |
| Krater | Regionale Kollisionsaenderung, Raketenkrater inzwischen standardmaessig an auf unterstuetzten SP-Rastern; Terrain-Wiederherstellung getestet. | Erster Aufbau/Salven-Spitzen, alle Explosionsarten, Save/Load/MP und grosse Raster nicht vollstaendig abgenommen. |
| Seitliche Hoehlen | Integrationsanalyse vorhanden. | Begehbarer 3D-Hohlraum mit Decke, Kollision, Projektilen und Editor fehlt. Kein Hoehenfeld-Loch als Hoehle verkaufen. |
| Blockburg | 485 Bloecke, Spieler-Kontakt und Granat-/LAW-Breschen installiert geprueft. | Keine allgemeine dynamische Objekt-/AI-/MP-Loesung. |
| Glas / Licht-Demoraum | Eigenstaendiges beidseitiges zerstoerbares Glas und Durchgang nach Zerstoerung getestet; bewegtes Raumlicht und Normal-/Gloss-Demo vorhanden. | Hinterseitige Glaswirkung und verbleibender Dach-/Wandschatten-Saum. |
| Alle OFP-Kleinteile physikalisch / Ragdolls | Einzelne Box3D-Probes vorhanden. | Universelle Tisch-/Stuhl-/Objektanbindung und Soldaten-Ragdolls fehlen. Pub-Objekte durch Waende weiterhin offen. |
| Schneefall / Sturm / Daecher | Flocken, Kameranachfuehrung, Sturm, Bewoelkung ohne Donner, Dachmaske und Bodenansammlung getestet. Tester bestaetigt Innenraumschutz. | Nicht jedes Gebaeude-/Addon-Dach garantiert; keine vollstaendige volumetrische Schneesimulation. |
| Schneespuren | Geometrische Nahvertiefungen, Kriechen, Wiederauffuellen und Explosionsmulden vorhanden. Volle aufgefuellte Chunks werden nun begrenzt freigegeben; neue Spuren ohne Reset nachgewiesen. | Tiefschnee-Koerper-/Radkontakt, ungeeignete kleine Spuren, Prone-Untergrundblick und Rotorwash-Abtrag offen. |
| Schnee auf Strassen | Distanzabhaengige kuenstliche Freilegung korrigiert. | Gesalzene Strassen mit weniger physischem Schnee und durchgaengige Uebergaenge nicht fertig. |
| Spieler nach Terrainmalen unsichtbar | Zwei grosse Nogova-Gruben plus Huegel: Spieler danach sichtbar. | Gemeldete Variante nicht reproduziert; Freefly-Uebergang und Bearbeitung unter dem Spieler noch pruefen. |
| Rauch | Raumabfrage-/Kollisionskosten reduziert; Wind/Downwash erhalten; Expansion, Verduennung und Bodenschatten angebunden. | Duenne Daechern/Portale, kalte Aufbereitung, alle Schattenvarianten und gewuenschte Gesamtoptik nicht geschlossen. Keine Teardown-Fluid-Simulation geliefert. |
| Meer / Brecherschaum | Bestehende Wellen und mehrere Filter-/Schaumkorrekturen vorhanden; Wave-Foam-Regler entkoppelt. | Besserer brechender Schaum, geringe Wiederholung, verbleibendes Glanzraster und Kuestenvibration offen. Vollstaendige Slider-Abnahme fehlt. |
| Wasserinteraktion | Einzelne Ripple-/Splash-/Auftriebsverbesserungen dokumentiert. | Vollstaendige Matrix fuer Schuesse/Raketen/rollende Koerper, Wellen nach Ripples, Boots-Wellendrag, Leichen auf dynamischen Wellen und Menschenschatten im Wasser bleibt offen. |
| Helikopterrotoren | Fruehere Rotorarbeiten sind kein Nachweis aller Distanz-/Winkelvarianten. | Fremde Rotoranimation und entfernte drehende Blaetter gezielt erneut kontrollieren; nicht ohne Nachweis geschlossen. |
| Lampen / Reset | Temperatur/Farbe/Staerke, gezielte Resets und Master-Reset-Ergaenzungen vorhanden; Torch-Winkel installiert wirksam. | Vollstaendige Dev-Settings-Inventur; nicht jede Einstellung pauschal als resetbar bestaetigt. |
| Moderne Lampenglow / Fahrzeugbirnen | Analytischer Halo/HDR-Kern statt lokaler Flare-Bitmap; Nacht-Jeep-A/B vorhanden. | Weitere Fahrzeuge, volumetrische Verdeckung und verbliebene Alt-Flare-Arten nicht alle abgenommen. |
| Licht durch Hindernisse / Schattenweichheit | Quellverdeckung und lokale Schattenreichweite verbessert (bis 180 m Deckel). | Durchgaengige Quelle-Blocker-Empfaenger-Weichheit und alle Haus-/Huegel-/Auto-Kegel fehlen als Abnahme. |
| Freefly-Dorf-/Fahrzeuglicht verschwindet | Nacht-Rueckkehrtests behalten 40/42 Kandidaten und sichtbare Lichtpools. | Intermittierende Spieler/Truck-Variante nicht reproduziert, daher nicht als behoben markieren. |
| Godrays auf entfernten Pflanzen | Mehrere bestehende Pfade und abgelehnte Experimente dokumentiert. | Dunkle Fern-LODs auf OFP/Arma-3 weiterhin offen; globale Aufhellung ist kein Ersatz. |
| DLSS / Wolken | Jitterkorrektur und Nogova-Wasserhorizont-A/B vorhanden. | Bewegtes Boden-Ghosting, Wolken-History/Nachziehen und Grain nicht vollstaendig geloest. |
| Picture Mode | Max blur 4, Spielerfokus optional aus, Klickfokus angebunden. | Vollstaendige interaktive Fokus-/Lebenszyklusabnahme. |
| Strassenglanz / Texturqualitaet | OnSurface von falscher Glasheuristik ausgeschlossen; konkrete Everon-Pose ohne blauen Glanz. | Gemeldeten Informations-/Farbverlust mit Originaltextur, Filterung, Mips und Tonemapping getrennt pruefen; mehr Kontrast ist kein Beweis gegen Degradation. |
| Eigene photorealistische Materialien | Eigenes Default-Paket 0.1.4, drei exakte Albedos, Road/Terrain-Normals und Sand-Detailnormals; Quellen/Generatoren und statische Tests vorhanden. | Rauheitsanbindung, fotorealistische Gesamtqualitaet, Bewegung/Wetter/DLSS, frisches Release und Benutzer-Mod-Prioritaet noch nicht voll abgenommen. |
| Black Ops Designator | Gewuenscht ist die Displayflaeche am Geraet, nicht die Optik. | Erneuten eindeutigen Material-/Monitor-Abnahmenachweis liefern; nicht aufgrund anderer Materialarbeiten schliessen. |
| Kopf-/Gesichtsfehler | Kopf im Spielerschatten sichtbar; gemeinsam reinitialisiertes Face-Rig korrigiert, 80-NPC- und Sitzaufnahme getestet. | Originalkampagne und alle Wasser-/LOD-Varianten nicht erneut abgenommen. |
| Zerstoerte Haeuser | Isolierte Zerstoerungsgeometrie auch beim Streaming; Ruine nach Fernflug erhalten. | Mehrere Ruinentypen und gleichzeitige Zerstoerung/LOD-Wechsel weiter pruefen. |
| Bodentreffer-Schatten Desert/Stratis | Echte Schusstests unterscheiden Staub/Decals von Schattenblobs; keine solchen Blobs im gueltigen Test. | Dunkler 6-Uhr-Fall ist keine ausreichende visuelle Abnahme; gemeldete Variante nicht pauschal widerlegt. |
| RTX 3090 Ti schwarzes Menue | Log zeigte DLSS inaktiv; ungueltiges 8x-MSAA-Feature behandelt, installierter lokaler 8x-Test rendert und beendet sauber. | Rueckmeldung auf konkreter Tester-GPU fehlt; kein pauschaler DLSS-Schuldnachweis. |
| AI / viele Soldaten | BT/HTN-Codebewertung vorhanden; Fernglas senkt bei Bewegung, offline UI-Move-Befehl wartet nicht auf Sprache; Palette-/Renderarbeiten vorhanden. | Gesamte Radio-Ueberlappung, koordinierte Taktik und grosse aktive Schlachten nicht fertig. Kein freigegebener pauschaler AI-Neubau. |
| Reforger native Optik | NTC-Normalkanaele korrigiert, zusaetzliches Blattflattern begrenzt. | Stamm/Krone-Synchronitaet in Bewegung, Kontrast/Normalwirkung von Wald/Boden und Innenmaterialien gegen konvertierte Karte offen. |
| OFP-Wald von oben | Originale Platzierungen erhalten. | Organische statt Rasterwirkung, bessere Fernkronen/Impostors und nachgewiesener Qualitaetsgewinn fehlen. |
| Arma 1/2 direkt laden | Code-/Formatplan und externe Datenpfade vorhanden. | Vollstaendige unkonvertierte Mission mit benoetigten Script-/Config-/Animationsfunktionen nicht geliefert. Keine geleakten Quellen verwenden. |
| Upstream | September-11-Pruefung: main weiterhin 3541affa; mehrere Schutzkorrekturen bereits vorhanden. | Cursor-Geometrie/Hotspot-Kandidat noch zu portieren; Sortieransatz erst fork-spezifisch messen. Kein blinder Komplettmerge. |
| Backups / Hostabsturz / Speicher | Eigene Commits auf beiden safety-Refs; temporaere Altbelege zuvor kontrolliert archiviert. | Windows-/git-Absturzursache nicht bewiesen. Fremdes Cargo.lock ist ausdruecklich nicht Teil eigener Sicherung. |

## Drei Modernisierungsschritte

[Die Codebewertung](decisions/MODERNISATION-THREE-TRACKS-20260910.md) existiert;
**die Umsetzung aller drei Schritte ist nicht abgeschlossen**.

1. Fernansicht: Terrain-CDLOD, automatische Boden-Satmap, Instancing und
   Objekt-Streaming vorhanden. Objektvollstaendige Ferntexturen/HLOD fehlen.
   Keine pauschal verlustfreie LOD- oder FPS-Aussage. Nah- und Fernmaterialarbeit
   muessen im Experiment tatsaechlich getrennt werden.
2. CPU: Worker, Renderthread und Overlap existieren schon. Vier kontrollierte
   Szenarien (Mission, viele sichtbare Soldaten, Kampf, schneller Flug) sind noch
   nicht gemeinsam auf dem aktuellen Stand vermessen. Keine parallele
   AIUnit::Think-Weltmutation und kein zweiter Scheduler als Abkuerzung.
3. Laden: bestehende Pipeline erweitert, Lebensdauer-/Generationspruefungen und
   Messharness vorhanden. Native Texturidentitaet spart im gleichen Build-A/B
   rund 0,56 GiB Prozessspitze und 248,5 MiB getrackte Objekttexturen. Kein
   stabiler FPS-/Tailgewinn. DDS-Worker-Experiment bleibt mangels Nutzen aus;
   es kostete etwa 0,6 GiB mehr RAM. Einzigartige Materialaufbereitung und
   Aufenthalts-/Upload-Spitzen bleiben zu reduzieren, nicht eine konkurrierende
   Streaming-Verwaltung daneben bauen.

Zahlen stammen aus den dokumentierten Tests, nicht aus diesem Audit. Siehe
[native Texturidentitaet](decisions/NATIVE-TEXTURE-IDENTITY-20260911.md).
Prioritaet nach bisherigem Beleg: unnoetige Lade-/Materialarbeit verringern,
Fernrepraesentation als begrenzten Qualitaetstest entwickeln, weitere CPU-
Parallelisierung erst nach dem fehlenden Szenenprofil. Jeep bleibt ein separates
autorisiertes Gameplay-Paket und wird nicht durch Streaming-Arbeit ersetzt.

## Bewusst nicht als fertige oder gestrichene Arbeit verbuchen

- Boot-Ein-/Aussteigen: vom Besitzer nach Tester-Korrektur zurueckgenommen.
- Flache Fusion statt Globus: ausdruecklich erlaubte Variante, keine fehlende
  Globus-Simulation als heimlich erledigt ausgeben.
- Ein fehlgeschlagener Filter/Worker/Shader-Versuch lehnt nur den Ansatz ab,
  nicht das Ziel besserer Wolken, Waelder, Wasser oder Streaming.
- Groessere Umbauten ohne Abnahme nicht default aktivieren: insbesondere FFV-
  Multiplayer, Hoehlenkollision und neue AI-Planer. Das ist eine Integrations-
  grenze, keine Begruendung, diese Ziele dauerhaft zu streichen.
- Kein Wunsch wird hier allein wegen seiner Groesse als schlechte Idee erklaert.
  Insbesondere Satmaps mit Objekten, Jeep-FFV und echte Hoehlen bleiben offen.

## Erneut bestaetigte offene Abnahmeziele (Abend)

Diese Punkte bleiben gleichrangig neben Performance; keine Streichung durch
einen erfolgreichen Build, Einzeltest oder Speichergewinn:

- Tiefschnee: Beim Gehen muessen Beine/Koerper eine zusammenhaengende Rinne
  verdraengen, nicht einzelne Fussabdruecke hinterlassen. Entsprechende
  Fahrzeug-/Rumpfverdraengung fehlt ebenfalls. Bestehende Kriechspuren nicht
  als fehlendes Feature behandeln. Der separate Untergrundblick beim Kriechen
  blieb im Ein-Meter-Vergleich trotz verbesserter Freilegung offen.
- Rotorwind: bodennahen Schnee abtragen; noch nicht fertig integriert.
- Unsichtbarer Spieler nach Terrainbearbeitung: bislang nicht reproduziert,
  insbesondere Freefly-/Editor-Wechsel noch gesondert pruefen. Nicht als fix
  verbuchen, nur weil die bisherige Nogova-Sequenz keinen Fehler zeigte.
- Satmaps: Boden vorhanden; Doerfer und Baumkronen weiterhin fehlende Abnahme.
- Jeep: Aussage "nur Paket-Infrastruktur" ist inzwischen veraltet. SP-Sitz,
  eigene Animationen und echte Schuesse existieren teilweise; Bedienung und
  normale Einstiegsabnahme bleiben nach Besitzerfeedback offen. Installiert
  35e3a0d4: separierte Kamera, normaler relativer Maustest bestanden. Keine
  allgemeine Waffen-/Fahrzeugfreigabe oder vollstaendige FFV-Abnahme behaupten.
- Mods-Ordner und Sandbox-Jeepstation sind im Quelltext vorbereitet, aber noch
  nicht gebaut/installiert/runtime-geprueft, solange die Besitzersitzung laeuft.
- Reforger-Streaming: Speicherverbesserungen schliessen Nachladehaenger nicht.
  Entfernte Objekte brauchen einen budgetierten niedrigen LOD-Pfad, sodass die
  sichtbare Ladegrenze verschwindet, nicht mehr Nebel oder weniger Sichtweite.
- Reforger-Wald/Boden: Kontrast und Materialtiefe; Baumqualitaet gegen die
  konvertierte Karte vergleichen; Stamm/Krone zusammenhaengend im Wind bewegen.
- Echte Hoehlen, Fusion-Terrainbearbeitung, Licht-/DLSS-/Waldfehler und die
  vollstaendige Materialabnahme bleiben offen. Jeweils tatsaechlichen Code und
  reproduzierbare Tests pruefen, bevor ein Teilziel geschlossen wird.
