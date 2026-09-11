# Anforderungspruefung, 11. September 2026

## Aktueller Nachtrag 20:32 (kein Gesamtabschluss)

Naechster Durchstich: TrackOptimizedFour verbreitert tiefen Schnee zwischen den
vier Radkontakten, 10 Hz, 0.35 m visuelle Unterboden-Naeherung. Flacher Schnee,
Motorrad-/Panzerpfad und Fahrphysik unveraendert. Build und 10012 Pruefungen
bestanden; installierter UAZ-Fahr-/Wiederauffuelltest noch ausstehend. Nicht als
vollstaendige physikalische Fahrzeugverdraengung abgenommen.

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
