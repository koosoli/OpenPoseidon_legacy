# Folgebericht: deine weiteren Auftraege am 9. September 2026

## Stand und ehrliches Ergebnis

Verglichen mit dem [letzten Gesamtbericht](REQUEST-AUDIT-20260909.md), Commit
`bc766e19` / Runtime `a4d09dbe`. Gepruefter Quellstand vor diesem Dokument:
`19ce224f`; zuletzt installiertes zusammenpassendes Spiel: `b51f877d`,
9. September, 21:28. Dieser Anschluss ist Dokumentation, kein neuer Spielbuild.

**Es ist deutlich mehr umgesetzt, aber nicht alles fertig.** Insbesondere die
Fusion-Karte existiert jetzt wirklich, Terrain kann mit passender Kollision
veraendert werden, Schnee wird unter geeigneten Daechern maskiert und klassische
OFP-Karten haben eine automatische Ferntextur. Die gegenteiligen Aussagen des
alten Berichts sind ueberholt. Echte Hoehlen, eine abgeschlossene BT/HTN-Studie,
der Vergleich grosser AI-Schlachten mit Vanilla und mehrere Bildfehler bleiben
offen. Ich habe diese Punkte weder erledigt noch als von dir abgelehnt verbucht.

Grundlage sind die Commits und das chronologische
[Implementierungs- und Testprotokoll](SEPTEMBER9-VISUAL-FOLLOWUP.md).
Ein bestandener gezielter Test ist keine Abnahme aller Karten, Modelle und
Grafikeinstellungen. Spaetere Eintraege im Protokoll ersetzen fruehere
"pending"-Zwischenstaende. Die folgenden Restarbeiten sind Vorschlaege, keine
Behauptung, sie bereits durchgefuehrt zu haben.

## 1. Fusion und Kartengroesse

**Alle OFP-Inseln nebeneinander: umgesetzt und im installierten Spiel geprueft.**
Everon, Malden, Kolgujev, Nogova und Desert Island liegen gemeinsam in einer
flachen **51,2 x 51,2 km** grossen Welt. Die urspruenglichen Inseln werden
verschoben, nicht skaliert oder gekruemmt. Uebernommen wurden unter anderem
407.848 Objektplatzierungen und 1.092 Bodentexturen. Zwischen den Quellgebieten
liegt mindestens 3,2 km zusaetzlicher Wasserabstand (`713d1378` und Folgefixes).

- Alle fuenf Inseln und die gemeinsame Uebersicht wurden im laufenden Spiel
  aufgenommen und angesehen; Fahrzeugpositionen und Terrainhoehen kontrolliert.
- Die Mission bietet einen Hubschrauber und Visit-Aktionen. Diese Aktionen sind
  sichtbar; automatisierte Tests versetzten Fahrzeug/Kamera zu jeder Insel.
  Ein durchgehender Flug zwischen jedem Inselpaar und jeder Aktionsklick wurden
  nicht separat abgenommen. Sichtbarkeit der Nachbarinsel haengt vom Standort ab.
- Die Originalkarten bleiben unveraendert. Die Fusion ist **kein Globus** und
  uebertraegt nicht automatisch Kampagnen, Objekt-IDs, Stadt- und Flughafenlogik.
- Originale Waldkacheln und sichtbare Unterwasser-Gebietsgrenzen bleiben.
  Vorschlag: diese Uebergaenge separat verbessern, nicht die Inseln verformen.
- Der Generator verwendet die lokal vorhandenen Originaldaten. Generierte
  Retail-PBOs werden nicht ins Git-Backup aufgenommen oder weiterverteilt.

**Mehr als 50 km: nachgewiesen, aber kein unbegrenztes Groessenversprechen.**
Eine separate **102,4 x 102,4 km** grosse, objektarme Diagnosekarte startet jetzt
als Mission, nicht nur im Editor. Drei weit auseinanderliegende Testpunkte bis
92 km wurden besucht. Speicher im wiederholten Test etwa 1,7-1,8 GiB; Fusion
etwa 2,2-2,3 GiB. Die grosse Hoehenfeld-Aufbereitung brauchte etwa 25 Sekunden.

Behoben wurden eine ueberfluessige 256-MiB-Kopie, ein zuvor verschluckter Rust-
Panic bei der Schattenmaskengroesse und unbeschraenkte automatische Verfeinerung.
Das Hoehenfeld bleibt auf 8192 Samples je Seite begrenzt; deshalb verwendet der
102,4-km-Test effektiv 12,5 m statt gewuenschter 6,25 m. Normale OFP-Karten behalten
ihre Detailvorgaben. Sichtweite 50 km und Weltgroesse sind verschiedene Grenzen.

**Offen:** dichte 100-km-Welten, grosse Schlachten, MP, Save/Load und umfassende
Kollisionstests. Das absolute Maximum wurde nicht ermittelt. Vorschlag:
stufenweise Objekt-/Streaming-Lasttests statt weitere Limits blind hochzusetzen.
Reproduzierbar ueber `scripts/Test-Fusion.ps1`, fuer den Groessentest mit `-Large`.

## 2. Terrainmalen, Krater und Hoehlen

**Hoehenmalen: umgesetzt.** Dev Tools > **Map Editor** > Raise/Lower, Radius und
Geschwindigkeit einstellen > **Start painting**. Linke Maustaste auf dem Land
gedrueckt halten; loslassen stoppt die Bearbeitung. Bewegung mit WASD/Q/Z,
Blicksteuerung mit rechter Maustaste. Der Startknopf wurde nach einer Aufnahme
ueber die Scrollgrenze verschoben (`94d96586`, `f486ff37`).

Rendering, Terrainhoehen und Box3D-Kollision werden gemeinsam aktualisiert.
Regionale Kollision ersetzt bei Folgeaenderungen nur betroffene Teile; lokale
Himmelsverdeckung wird innerhalb des relevanten Horizontbereichs neu berechnet.
Ein erster installierter Kollisionsaufbau kostete noch rund 383 ms, eine
Wiederherstellung danach rund 1,52 ms. Das ist keine Gesamtframe-Messung.

**Radius am Cursor: umgesetzt (`b51f877d`).** "Show brush radius at cursor" ist
standardmaessig an. Terrainfolgender Kreis: Gruen = anheben, Amber = absenken,
Rot = nicht bearbeitbar. Angezeigt wird der effektive Radius einschliesslich
Mindestgroesse von 1,5 Terrainzellen. Dieselbe Treffer-/Radiusberechnung steuert
das Malen. Installiert geprueft: 29,835 -> 23,103 -> 16,048 m beim Halten,
unveraendert beim Loslassen, danach Anheben auf 22,080 m. Der Kreis ist ein
Editor-Overlay, keine gegen Hindernisse tiefengepruefte Weltgeometrie.

**Raketenkrater: umgesetzt, bewusst optional (`d8e8f1a2`, `6e71d593`).** Im Map
Editor "Rockets create terrain craters", Radius 8-100 m, Tiefe 0,1-8 m; Standard
aus, 12 m / 1 m. Explosive Raketen-Bodentreffer gehen durch den gleichen
Terrainpfad; keine Luftdetonationen, Unterwassereinschlaege oder direkte
Objekttreffer. LAW-Test aus/an/reset: 29,835 -> 29,835 -> 28,835 -> 28,835 m.
Reset stellt Einstellungen zurueck, repariert nicht bereits zerstoertes Terrain.

**Grenzen und naechste Schritte:**

- Bearbeitung ist experimentell, Singleplayer und missionslokal, fuer Raster
  bis 2049 Samples je Seite. Die grosse Fusion ist daher nicht automatisch
  mit diesem Pinsel bearbeitbar. Kein Export, Undo, Save/Load oder MP-Abgleich.
- Strassen/Gebaeude und AI-Navigation werden nicht automatisch umgebaut.
  Vorschlag: Aenderungsjournal plus geordnete Aktualisierung dieser Verbraucher,
  bevor daraus ein vollwertiger Karteneditor wird.
- **Alle Explosionen standardmaessig kleine Krater erzeugen lassen: noch nicht.**
  Deine Performancebedingung ist fuer erste Treffer und Salven nicht erfuellt;
  grobe OFP-Raster begrenzen ausserdem kleine Krater. Erst Salven/p95-Spitzen
  messen und Aktualisierungen buendeln, dann einen begrenzten Default erproben.
- **Seitliche Hoehlen/Tunnel: nicht implementiert.** Ein Hoehenfeld hat pro
  Position nur eine Hoehe, keine getrennte Decke und Bodenflaeche. Ein Shaderloch
  liesse den unsichtbaren Berg fuer Bewegung und Geschosse weiterhin massiv.
  Vorschlag: begrenzter lokaler 3D-Dichte-/SDF-Bereich mit gemeinsam erneuertem
  Mesh und Kollision. Zuerst eine begehbare Testhoehle mit beschiessbaren Waenden,
  danach Editor, Navigation, Speicherung und Netzwerk. Konkrete bestehende
  Integrationsstellen stehen in der
  [Hoehlen-/Pinsel-Untersuchung](decisions/TERRAIN-CAVES-AND-BRUSH-20260909.md).

## 3. Showcase, Materialien und Zerstoerung

| Wunsch | Umsetzung und Nachweis | Rest / Vorschlag |
| --- | --- | --- |
| Riesige umschiessbare Blockburg | 485 dynamische Bloecke, etwa 32 m Breite, 8 m Mauern und 11 m Tuerme. Der spaeter gemeldete Durchlauf-/Explosionsfehler wurde tatsaechlich reproduziert: Probe-Koerper waren nicht in den alten Spieler-/Schadenspfad eingebunden. Kontakt-/Gleitpruefung und Explosionsimpulse ergaenzt (`3e8ecbde`). Installiert stoppt die Wand den Spieler, das Tor bleibt passierbar, Granate und LAW reissen sichtbar Breschen. | Sandbox-spezifische Bruecke, kein neuer universeller AI-/Fahrzeugcontroller und keine MP-Koerperverwaltung. Weitere schwere Koerperstapel und AI-Kontakt gesondert testen. |
| Dunkler Raum mit bewegtem Licht | Geschlossener Testraum mit bewegtem lokalem Licht; auch bei Tag aktiv. | Breite helle Dachstreifen reduziert, gezackter PCF-Saum an Wand/Dach noch offen. Nicht den gesamten Schattenfilter abschalten; verbleibende Empfaenger-/Filtergrenzen gezielt untersuchen. |
| Normal-/Glass-Beispiele funktionieren nicht | Prozedurale Normalmaps korrekt fuer NOHQ gepackt; lokale Lichter tragen jetzt spiegelnden Glanz bei. Sichtbarer Normal-/Gloss-Vergleich im Raum (`f0cb045f`, `74369a27`). | **Gloss ist nicht Glass.** Eine eigenstaendige echte Glas-Demo ist noch nicht als fertig nachgewiesen. Vorschlag: separate Glasplatte mit klarer Transparenz-/Reflexionsabnahme ergaenzen. |
| Zerstoerte Gebaeude glitchen | Reproduziert: zerstoertes Everon-Haus sah nach Fernflug/Rueckkehr wieder intakt aus. Wartende Draws erhalten isolierte Zerstoerungsgeometrie, auch fuer gestreamte statische LODs (`5e8b7a7e`, `0d264e13`). Rueckkehr zeigt jetzt weiter die Ruine, Nachbar bleibt intakt. | Kein Beweis fuer jeden Ruinentyp. Naechster Test: mehrere gleichzeitig zerstoerte Gebaeude und LOD-Wechsel. |
| Gesichter verzerren sich bei vielen NPCs | Gemischte Soldatenklassen initialisierten gemeinsam genutzte Face-Rigs erneut und verloren die Gesichts-Knochenbasis. Bereits aufgebautes Rig wird erhalten (`bb6d606c`). Gleicher 80-NPC-Test und sitzende Nahaufnahme vorher/nachher zeigen den Fix; 13 Tests / 403 Assertions. | Original-Kampagnenszene nicht erneut gespielt. NPC-Anzahl war ein Ausloeser, kein nachgewiesenes festes Limit. Kampagnen-Regression weiter beobachten. |

## 4. Schnee und Wetter

**Umgesetzt:** feinere nahe Schneegeometrie bis 0,125 m Abtastabstand im nahen
8-m-Bereich, mehr Driftvariation und geometrisch sichtbare Vertiefungen statt
nur Farbspuren. Ansammlung und bestehende Personen-/Fahrzeugspuren bleiben
missionslokal erhalten; neuer Schnee fuellt Spuren auf. Das ist weiterhin
keine vollstaendige volumetrische Schneesimulation.

**Daecher:** nahe Ansammlung nutzt eine zwischengespeicherte Gebaeudedachmaske
(`a52b82e4`). Der geschlossene Testraum bleibt im Vergleich schneefrei; auch
maximaler Schneesturm wurde innen kontrolliert. Maske etwa 43-234 Mikrosekunden
im dokumentierten stationaeren Test. Sie braucht brauchbare Gebaeudeschalen und
arbeitet im kameranahen Fenster; daraus folgt keine Garantie fuer jedes Addon.

**Schneesturm ohne Gewitter:** Dichtebudget 0,25-8x mit fester Partikelobergrenze;
Sturmpreset und hoeherer Akkumulationsrate. Schneefall erzwingt visuell mindestens
starke Bewoelkung bis geschlossenem Himmel, ohne Missionsregen oder Donner
einzuschalten (`40fa7d5e`). Nogova-Vergleich mit geschlossenem Himmel vorhanden.

**Flocken statt Kugeln / Freefly:** gefiederte sechsaermige Alphatextur, 1,1-2,7 cm
variable Groesse, Drehung und Taumeln. Bei Windstille etwa 0,36-0,94 m/s Fallen;
Wind treibt die Flocken staerker. Die Population wird um die bewegte Kamera
erneuert, vorhandene Flocken bleiben in Weltkoordinaten. Auch pausierter Freefly
behaelt sichtbaren Schnee (`ae59e548`, `e0683049`). Ein 5,8-km-Flug in vier
Sekunden und Hoehenwechsel auf 900 m wurden geprueft. CPU-Anteile dieses Tests:
etwa 0,14-0,30 ms ruhig, 1,1-1,8 ms im Maximalsturm; kein Gesamt-GPU-Benchmark.

**Noch nicht fertig:** physikalische Fuss-/Radhoehe im tiefen Schnee, Schnee auf
allen Daechern/Fahrzeugen/Strassen, Schmelzen, seitliche Materialverlagerung,
Save/Load und MP. Ferne Subpixel-Flocken koennen weiterhin punktfoermig wirken.
Vorschlag: zuerst Kontakt und Boden-/Objektuebergaenge verbessern, nicht nur
mehr Partikel oder pauschal teurere Geometrie zeichnen.

## 5. Wasser, Fernsicht, Satmap und DLSS

| Wunsch | Umsetzung und Nachweis | Rest / Vorschlag |
| --- | --- | --- |
| OFP-Satmap fuer schoenere Fernsicht | Jetzt umgesetzt: automatische 1024er Fern-Albedo mit Mips aus Original-Bodentexturen, rund 5,3 MiB; Ueberblendung 800-2400 m. Kein eingebranntes Licht oder Schnee; moderne authored Satmaps unveraendert (`8a4161a8`, `9bded011`). Everon/Nogova aufgenommen; Aufbereitung etwa 111/485 ms einmalig. | Glaettet Originalkacheln, erfindet keine Satellitendetails. Grosse Fusion liegt ausserhalb der begrenzten automatischen Bake-Voraussetzungen. Vorschlag: getrennt budgetierten Fusion-Bake testen. |
| Wasser endet / braune Ersatzkacheln | Wasserreichweite vom Land-Clipping entkoppelt, Frustumecken abgedeckt, Offmap-CDLOD-Hoehenabfragen korrigiert. Sehr hohe und 32 km entfernte Ansichten zeigen durchgehendes Wasser statt brauner Ersatzfelder. | Vorher fehlende Flaeche kostet jetzt Rendering: ein Extremtest etwa 2,8-3,9 -> 4,1-4,9 ms. Unterwasser-Quellgrenzen der Fusion bleiben ein anderes Problem. |
| Wasserglanz weiter verpixelt | Fehler in der Vier-Fetch-Bikubik-Rekonstruktion korrigiert (`fa7a7af2`), ohne mehr Texturzugriffe. | **Nicht vollstaendig geloest:** Nahaufnahmen zeigen weiter Rasterspuren. Vorschlag: getrennte Normal-/Schaum-/Upscaling-A/Bs an derselben Wasserpose, dann guenstigsten belegten Filter waehlen. |
| Wave foam intensity wirkungslos | Von Uferschaumintensitaet entkoppelt; Whitecap-Einstellungen werden gespeichert (`684b2871`). | Code-/Vertragstests vorhanden, komplette interaktive Slider-A/B-Abnahme noch offen. |
| Wolken im Wasserhorizont auf Nogova | Wolken-Raymarch endet in der Hauptansicht an der tatsaechlichen flachen Meeresflaeche statt nur am sphaerischen Planetenmodell (`abd8c50f`). Exakte gemeldete Pose, gleicher Build mit Schalter aus/an: helle Baender verschwinden und kehren im Kontrolllauf zurueck. | Kein neuer Pass/Sample; kurzer GPU-Vergleich etwa 1,009 vs. 1,031 ms ist kein allgemeiner Speedup. Andere Wolkenfehler bleiben getrennt. |
| Boden ghostet bei schnellem DLSS-Freefly | Projektionsjitter vor der Tiefenrekonstruktion fuer Bewegungsvektoren entfernt (`c2e7055f`); acht temporale Tests bestanden, Fix installiert. | **Fix vorhanden, sichtbarer Gesamtfehler nicht abschliessend abgenommen.** Kontrollierter bewegter DLSS-A/B fehlt. Danach LOD-Wechsel und Disocclusion getrennt betrachten. |
| Wolken-Grain / Nachziehen mit DLSS | Frueheren guenstigen Filter beibehalten; keine neuen unbelegten Qualitaetsversprechen durch den Horizontfix. | Wolken-History/Bewegungsdaten weiterhin offen. Vorschlag: repraesentative Wolkentiefe und Bewegung testen, statt wirkungslose Masken oder teurere Raymarch-Samples einzubauen. |

## 6. Lampen, Schatten und Picture Mode

| Wunsch | Umsetzung und Nachweis | Rest / Vorschlag |
| --- | --- | --- |
| Lampenfarbe, Temperatur, Intensitaet und Reset | Bestehende Street-lamp-Regler behalten; eigener Beleuchtungsreset und Master-Reset schliessen diese Einstellungen jetzt ein. Auch neue Pinsel-/Raketenoptionen haben Reset. | Keine Behauptung, jede beliebige Dev-Tools-Option sei weltweit audit-geprueft. Vollstaendige Settings-Inventur als Anschluss sinnvoll. |
| Keine alten Bitmap-Glows, leuchtende Fahrzeugbirnen | Lokale WGPU-Lichtquellen zeichnen analytischen Halo und HDR-Kern im vorhandenen Additiv-Batch statt Flare0-Bitmap, eine Kernlage statt acht (`ea5643e5`). Installierter Nacht-Jeep-A/B zeigt beleuchtete Linsen und glatten Halo; Hausverdeckung bleibt wirksam. | Sonnen-/sekundaere Flares und GL33 absichtlich unveraendert. Breitere Fahrzeug- und subjektive Leuchtwirkung weiter pruefen; nicht alle Alt-Flare-Arten ersetzt. |
| Licht durch Auto/Haus/Huegel | Quellverdeckung bleibt aktiv und wurde auch beim analytischen Glow kontrolliert. | Nicht alle volumetrischen Kegel und beleuchteten Flaechen damit abgenommen. Huegel, Karosserie und Raum jeweils mit getrennter Quell-/Flaechenaufnahme testen. |
| Kleine Quellen harte, grosse Quellen weiche Schatten; Reichweite | Keine vollstaendige neue quellen-/blockerabhaengige Schattenloesung hinzugefuegt. | Offen, inklusive Abschneiden erreichender Schatten. Schattenweichheit haengt auch vom Abstand Quelle-Blocker-Empfaenger ab, nicht nur Kameraabstand. Vorschlag: diese Geometrie und Reichweitenfade isoliert testen, dann begrenztes Contact-Hardening statt pauschal groesserem Atlas. |
| Freefly-Dorf wird trotz lebendem Spieler schwarz | Everon-Nachttest bis ueber 3 km und zurueck: alle 40 Lichtkandidaten bleiben ausgewaehlt, Lichtpools sichtbar. | Gemeldeten Ausfall dort **nicht reproduziert**, daher kein spekulativer Fix. Weitere Orte/Belichtung/Sichtweiten testen. |
| Zeus Torch cone wirkungslos | Expliziter Winkel wird jetzt wirklich fuer den Kegel verwendet; normale Fahrzeugvorgaben unveraendert (`f0cb045f`). Installierter Vergleich 0,12 vs. 1,1 zeigt verschiedene Lichtflaechen. | Konkreter Reglerfehler behoben. |
| Everon-Offroadstrasse glaenzt wie Glas | OnSurface-Strassendecals von automatischer Glas-Klassifizierung ausgeschlossen (`9bded011`). Exakte gemeldete Kamerapose zeigt den blauen Glasschimmer nicht mehr. | Keine globale Abschaltung von Fensterglas. |
| Picture Mode | Max blur **4** als Default, optionaler Spieler-Fokus standardmaessig aus und Klickfokus in Freefly (`684b2871`). | Implementiert; Lebenszyklustest vorhanden. Alle Fokuswechsel interaktiv noch nicht abschliessend abgenommen. |
| Kopf fehlt im Spielerschatten | Bestehenden Aussen-LOD-Fix jetzt mit klarer SoldierWB-Aufnahme bestaetigt: Kopf/Helm, Schultern, Waffe, Beine vorhanden. | Diagnostisch kurze Reichweite/PCF aus, normale Defaults unveraendert. Kein Nachweis fuer alle Einheiten oder Menschenschatten im Wasser. |
| Godrays lassen ferne Buesche/Wald-LODs kontrastreich schwarz | Konkreter Kandidat gefunden: spaete Pflanzen-Draws nach Godrays koennen als Opaque statt transparent klassifiziert sein und erhalten dann keine Oberflaechenstreuung. | **Ursache noch nicht fuer dein Bild bestaetigt, kein fertiger Fix.** Draw-Klassifizierung am betroffenen Wald erfassen und eng korrigieren. Globales Aufhellen wuerde andere bereits reparierte Materialien gefaehrden. |

## 7. Rauch

**Performance-Regressions weiter eingegrenzt und verbessert.** Hohle Gebaeude
wurden ueber ihre massiven Komponenten falsch als "kein Raum" erkannt; dadurch
liefen teure wiederholte Strahlpruefungen. Direkte Raumraster-Abfrage und fruehe
Ablehnung weit entfernter Gebaeude korrigieren das (`252c9cda`, `f94eb275`).

Im gleichen 30-s-Nogova-Test mit etwa 3.900 Partikeln sank die spaete Simulation
von **11-15 ms auf etwa 2,0-2,3 ms**. Ein unnoetiger 85,5-ms-Rasteraufbau eines
Nachbarhauses entfaellt. Wind, Downwash, Partikelgrenzen und Kollisionsantwort
wurden dabei nicht reduziert. 36 gezielte Tests / 28.591 Assertions bestanden.

**Ausbreitung/Verdunnung/Schatten:** wachsende Partikel verwenden dieselbe
Verduennung in Sprites, Selbst-/Bodenschatten und Volumeninjektion (`c59deee1`).
Rauchschatten haengen nicht mehr versehentlich am eingeschalteten Wolkenschatten.
Bestehende Bewegung und Lebensdauer/Despawn bleiben; keine neue unbegrenzt
wachsende Stroemungssimulation. Installierte Aufnahmen zeigen Rauch mit Schatten.

**Offen:** erster notwendiger Raumrasteraufbau kann noch stocken (etwa 30,5 ms
in der Fixture). Aussenaufnahmen ohne grosse Leckwolke beweisen nicht jede duenne
Wand oder jedes Dach. Auch ein fehlender Schatten war nicht in jeder gemeldeten
Konstellation reproduziert. Vorschlag: kalte Rasteraufbereitung budgetieren und
duenne Waende/Portale von innen und aussen vergleichen. Teardown-artige volle
Fluid-Simulation wurde wegen unbewiesener Kosten-/Qualitaetsbilanz nicht ersetzt.

## 8. AI, viele Soldaten und Vegetation

**Zwei konkrete AI-Verbesserungen sind umgesetzt:**

1. Bewegungsbefehle unterbrechen AI-Fernglasbeobachtung ueber die vorhandene
   Absenkanimation (`85c6c78c`), ohne den Spieler zu aendern. Isolierter doMove-
   Vergleich: vorher erst nach etwa 3 s Bewegung, danach 6,7 m in der ersten
   Sekunde. Das ist nicht die vollstaendige berichtete 30-s-Kampfsituation.
2. Offline-Spielerbefehle werden nicht mehr von wartender Funksprache blockiert
   (`3dcaf4f3`). Bei absichtlich 15 s blockiertem Kanal bewegte sich die Einheit
   schon im ersten Sekundenintervall; Missionsbefehle behalten ihr Timing.
   Schutz gegen doppelte Ausfuehrung vorhanden. MP-Verhalten nicht umgebaut.

**Noch nicht gemacht / keine abgeschlossene Abnahme gefunden:**

| Anfrage | Ehrlicher Stand und Grund | Vorschlag |
| --- | --- | --- |
| Gleichzeitige Funkstimmen | Nicht eingefuehrt. Befehlsausfuehrung und Audio-Ueberlappung sind getrennte Aufgaben; Verstaendlichkeit/Kompatibilitaet noch nicht untersucht. | Optionalen begrenzten Sprachmischer getrennt testen, nicht Missionsreihenfolge veraendern. |
| Flankieren, Unterdruecken, Fire-and-manoeuvre, Combined Arms | Kein neuer umfassender Taktikplaner geliefert. Konkrete Regessions wurden vorgezogen; das ersetzt diesen Auftrag nicht. | Ein kleines zuschaltbares Gruppenszenario auf bestehende Aktionen aufbauen, Erfolgs-/Verlust-/Befehlsmetriken gegen Original messen. |
| Codebasierte BT-/HTN-Architekturbewertung, zunaechst ohne Implementierung | Die von dir verlangte abgeschlossene Untersuchung liegt fuer diesen Folgeauftrag noch nicht vor. Keine Entscheidung fuer einen Architekturtausch getroffen. | Erst FSM, Gruppe, Wahrnehmung, Planung, Bewegung und Befehle im tatsaechlichen Code kartieren; dann Kompatibilitaet, Save/MP/Determinismus und CPU bewerten. Bestehendes Verhalten als A/B-Baseline, keine pauschale BT/HTN-Einfuehrung. |
| 80+/hunderte Soldaten schneller, Vanilla-Vergleich, Multithreading/Steam-Hinweis | Kein belastbarer gleicher 80/160/320-Soldaten-Vergleich und keine daraus abgeleitete AI-Parallelisierung abgeschlossen. Der Gesichtstest mit 80 NPCs ist **kein Performancebeleg**. | Gleiche Szene mit Simulation, Wahrnehmung/Pfadsuche, Animation/Skinning, Schatten und Draw-Vorbereitung getrennt messen. Erst den gemessenen Engpass optimieren; Threads nur fuer isolierte Aufgaben mit geordneter Ergebnisuebernahme. Den verlinkten Hinweis dabei gegen unseren Code pruefen. |
| Vegetations-Billboards, bessere prozedurale/windbewegte Baeume, weniger Waldkacheln aus der Luft | Kein neuer Classic-Billboardpfad oder prozeduraler Waldersatz im Folgeauftrag umgesetzt. Vorhandene Rendererarbeit ist nicht diese neue Verbesserung. Fusion zeigt weiterhin Original-Waldkacheln. | Erst eine Baumart als mehransichtiger Fern-Impostor mit korrekter Beleuchtung/Schatten und Ueberblendung testen. Luft-/Bodenvergleich ist Pflicht; danach Wald-Verteilung/Varianten separat. Nicht alle Pflanzen auf ein flaches Max-LOD zwingen. |

Diese Aufgaben sind **offen, nicht unmoeglich und nicht von dir abbestellt**.
Die bisherige Priorisierung konkreter Start-, Render- und Bedienfehler erklaert
den Rueckstand, ist aber keine Erledigung. Insbesondere behaupte ich keinen
nachgewiesenen FPS-Gewinn fuer grosse Schlachten oder eine fertige AI-Studie.

## 9. Startfehler, Windows, Speicherplatz und Backup

**RTX 3090 Ti / schwarzes Bild:** Im eingesandten Log war DLSS mangels passender
DLL **nicht aktiv**. Belegt war eine ungueltige 8x-MSAA-Konfiguration fuer
Rgba16Float ohne aktivierte adapterabhaengige Formatfaehigkeiten. Diese werden
jetzt korrekt angefordert; Samplezahl bekommt einen sicheren Fallback
(`3d662192`). Installierter 8x-MSAA-Vergleich ohne DLSS: vorher schwarz, danach
Everon gerendert, kein entsprechender Validierungsfehler. **Ruecktest auf genau
dem Rechner des Melders fehlt.** Falls es dort bleibt: neues vollstaendiges Log
mit aktuellem Build; vorlaeufig geschlossenes Spiel auf `msaaSamples=4` in
`%APPDATA%/CWR/graphics.cfg` einstellen. Nicht pauschal DLSS die Schuld geben.

**git.exe-Absturz und ganz Windows haengt:** Ursache **nicht geklaert**. Die
Nullpointer-Meldung allein beweist weder einen Enginefehler noch defekten RAM.
Die Arbeitslast koennte beigetragen haben; das wurde nicht ausgeschlossen.
Arbeit wurde pausiert, Buildparallelitaet von zehn auf zwei Jobs reduziert und
Spieltests serialisiert. Vorschlag bei Wiederholung: zeitlich passende Windows-
Ereignisse, Fehlerdump und Speicher-Commit erfassen; kein weiterer Lasttest bis
der Rechner stabil ist. Keine Hardware-/Treiberdiagnose als gesichert ausgeben.

**C: aufraeumen:** sechs alte temporaere Ordner in den Archivbereich
`D:\CWR-Archive\2026-09-09-temp-cleanup` uebertragen, Datei-Hashes vor Entfernung
der C:-Duplikate verglichen. Freier Platz etwa **15,4 -> 23,7 GiB**. Aktuelle
Worktree-Belege, SDK-/Hardlink-Bestaende und fremde `Cargo.lock` ausgenommen.
Nicht pauschal alle `.tmp`-Ordner geloescht; sie enthalten teils Testnachweise.

**Backup:** Implementierungsstand bis `19ce224f` liegt auf beiden GitHub-Refs
`safety/asset-obs-001` und `safety/consolidated-v2`. Die fruehere benannte Sicherung
`codex/backup-asset-obs-20260909-bc766e19` bleibt ein historischer Snapshot, kein
automatisch mitwanderndes Backup. Generierte Retail-Assets, lokale Testbilder
und installierte Binaerdateien sind nicht automatisch Teil eines Git-Backups.

## 10. Aeltere offene Punkte nicht vergessen

- **Leichen auf FFT-Wellen:** bisherige CPU-Wasserunterstuetzung bleibt, aber
  Hover-/Untertauchen gegen die GPU-Oberflaeche nicht zuverlaessig geloest.
  Gemeinsame bzw. zeitkompensierte Wasserabfrage erproben, kein konstanter Offset.
- **Fernrotoren und Menschenschatten im Wasser:** kein neuer abschliessender
  Vergleich. Gestoppter/laufender Rotor mit identischer Kamera und Wasserschatten
  getrennt vom Auftrieb testen. Die Kopfaufnahme schliesst diese Punkte nicht.
- **Bootladen/-entladen:** Meldung von dir nach Tester-Korrektur widerrufen.
  Kein Bugfix erforderlich und keiner dafuer als Leistung verbucht.
- **Upstream / native Reforger-Optimierung:** Ergebnisse des alten Berichts
  bleiben gueltig. Keine neue vollstaendige Upstream-Pruefung oder allgemeine
  Reforger-FPS-Steigerung seither behauptet. GL33-Portierungen nur bei Nutzen
  fuer unseren Pfad, keine blind uebernommenen konflikttraechtigen Serien.
- **Ozean-Vier-System-Mischer:** weiterhin nicht eingefuehrt, weil keine
  gemessene Verbesserung bei deinen Kostenvorgaben belegt ist. Vorhandene vier
  FFT-Kaskaden sind nicht vier frei austauschbare Simulationssysteme.
- **HRTF/Mousewheel und weitere manuelle Handover-Abnahmen:** weiterhin offen,
  nicht durch Rendering-Tests ersetzt.

## Startbefehle in PowerShell

Fusion, bereits lokal installiert:

```powershell
Set-Location 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
.\OpenPoseidon.exe --render wgpu --window --test-mission 'Missions\FusionTour.FusionOFP'
```

Showcase mit Burg und Lichtraum, alternativ starten:

```powershell
.\OpenPoseidon.exe --render wgpu --window --test-mission 'Missions\ShowcaseLab.Intro'
```

Separate installierte 102,4-km-Diagnosekarte, keine bevoelkerte Fusion:

```powershell
.\OpenPoseidon.exe --render wgpu --window --test-mission 'Missions\SizeProbe.FusionSizeProbe'
```

`cd /d` gehoert zu cmd.exe, nicht PowerShell. In PowerShell ist das `.\` vor
dem lokalen Programm erforderlich. Nur eine Spielinstanz gleichzeitig starten.

## Empfohlene Reihenfolge der Restarbeiten

1. Kontrollierten Viele-Soldaten-Vergleich und die ausdruecklich angeforderte
   Code-Architekturbewertung liefern; erst danach groessere AI-Umbauten.
2. Wolken-/Terrain-DLSS in Bewegung und die konkrete ferne Waldklassifizierung
   abnehmen, Wasser-Rasterglanz und FFT-Koerperkontakt getrennt behandeln.
3. Lichtreichweite/Contact-Hardening, Raum-PCF-Saum, echtes Glas und fehlende
   interaktive Settings-/Fokusabnahmen schliessen.
4. Terrain-Ersttreffer und Salven budgetieren, dann Default-Krater entscheiden;
   Hoehlen als begrenzten gemeinsamen Render-/Kollisionsprototyp beginnen.
5. Schnee-Kontakt und Speicherung sowie Fusion-Uebergaenge/Vegetationsdarstellung
   erweitern, ohne bestehende Karten oder Kampagnen ungefragt zu veraendern.

## Verifikationsgrenzen dieses Berichts

Die genannten Messungen stammen aus dokumentierten gezielten Tests, nicht aus
einem heute erneut gestarteten Gesamtbenchmark. Testzahlen werden nicht zu einer
scheinbaren Gesamt-CI-Summe addiert. Beim Map-Editor-Lauf blieb ein bestehender
breiter Panel-Source-Guard mit sechs alten Assertions rot; neue Pinseltests
bestanden. Die letzte Pinselabnahme endete mit Exit 0, enthielt aber vier schon
zuvor vorhandene LockCache-Shutdownwarnungen. **Nicht die gesamte Suite gruen.**

Fremde `Cargo.lock` bleibt unveraendert, erwarteter SHA256:
`9A4E74D791B26A4303E064867DA67B862C1439D0635DF3E19ED5A97AADF31A2E`.
Fuer diese reine Dokumentationsaenderung ist kein Spielbuild oder Deployment
erforderlich. Lokale Rohaufnahmen sind im Testprotokoll benannt, aber nicht alle
in einem frischen Git-Clone vorhanden.
