# OpenPoseidon — allgemeines Firing from Vehicles
## V6: konsolidierter Implementierungsauftrag, zur Umsetzung freigegeben

**Diese Fassung ersetzt V1–V5. Nicht kumulativ abarbeiten.**

**Geprüfte Referenz:** `koosoli/PoseidonWGPU-private-safety-mirror`, Branch `consolidated-v2`, Commit `d6f60ff097e1b7f528812e2facdce66e926f4367`, 11. September 2026. Vor Änderungen aktuellen HEAD, Arbeitsbaum und andere Agentenarbeiten prüfen. Nicht zurücksetzen.

**Urteil:** Als Architektur- und Umsetzungsplan ist diese Fassung gut so. Es gibt keinen identifizierten grundsätzlichen Blocker, der vor dem ersten generischen Durchstich einen neuen Renderer, ein neues Physiksystem oder einen vollständigen Animationsumbau erfordert. Das ist eine begründete Engineering-Einschätzung, kein Beweis für fertige Implementierung oder beliebige ungeprüfte Mods.

**Evidenz:** Erneute Quellcodeprüfung ausgewählter Integrationsstellen und Abgleich mit den bisherigen Prüfungen. Neun vorhandene synthetische mathematische Tests erneut ausgeführt; zusätzlich 13 Tests eines begrenzten abstrakten Zustandsmodells ausgeführt. Kein Engine-Patch gebaut, keine neue Runtime-/Gameplay-Abnahme. Diese 22 Modelltests sind ausdrücklich keine Poseidon-Unit-Tests. Sie begründen keinen FPS- oder Fertigstellungsanspruch.

---

## 1. Auftrag, Erfolg und Grenzen

Ein Passagier bleibt ein normaler Soldat mit seinem vorhandenen Inventar, seinen Waffen und der bestehenden Waffenlogik. Das Fahrzeug liefert Sitzbindung und zulässigen Bewegungsraum. Kein zweiter unsichtbarer Schütze, kein Umdeuten persönlicher Gewehre in montierte Fahrzeuggeschütze.

**Architekturziel:** Weitere geeignete Fahrzeug-Waffen-Kombinationen benötigen normalerweise nur wiederverwendbare Profile, keine neue C++-Sonderbehandlung der Fahrzeug-/Waffennamen.

**Nächstes verbindliches Ergebnis:** Zwei vorhandene fahrbare Fahrzeuge mit unterschiedlichem Sitzaufbau × zwei tatsächlich gültige Gewehr-Loadouts. Normales Einsteigen, Zielen, Feuern, Nachladen, Aussteigen und Wiedereinsteigen. Waffen-/Moduswechsel zusätzlich mit einer vom vorhandenen Inventar erlaubten Kombination. Danach ein vorher nicht eingepasstes drittes Beispiel ausschließlich über Profile anschließen.

Die beiden Gewehre müssen nicht gleichzeitig getragen werden: getrennte gültige Loadouts sind der richtige Kompatibilitätstest. Eine zulässige Gewehr-/Pistolen-Kombination oder vorhandene Mündungs-/Modusumschaltung prüft die Bedienung. Keine Inventarregeln oder Waffen-Slotmasken für einen grünen FFV-Test verändern.

**Produktziel:** Die geeigneten Plätze der unterstützten Originalfahrzeuge mit Default-Profilen ausliefern; weitere von der Engine bereits simulierte persönliche Waffenfamilien systematisch integrieren. Namenneutrale Unterstützung und tatsächlich abgenommene Inhaltsabdeckung getrennt ausweisen.

Geschlossene oder physisch ungeeignete Plätze dürfen korrekt abgelehnt werden. Fahrer, Piloten und montierte Geschütze behalten zunächst ihre Rollen. Kein Anspruch, ein importiertes statisches Fahrzeugmodell dadurch fahrbar zu machen oder unbekannte Skelette ohne Metadaten fehlerfrei zu animieren. Allgemeine Unterstützung ersetzt keine fehlende Arma-Config-/Fahrzeugkompatibilität.

**Nicht mehr als Voraussetzung verlangen:** Vollständige Animation-Middleware, neuer globaler Scheduler, allgemeiner Fixed-Step-Umbau, neues Netzwerkprotokoll oder vollautomatische Rekonstruktion beliebiger Modmodelle. Solche Nebenprojekte lösen nicht den nächsten FFV-Durchstich.

## 2. Festgelegte Architektur und Modulgrenzen

Vier Verantwortlichkeiten, keine Pflicht zu vier neuen Subsystemen:

| Verantwortung | Inhalt und Integrationsstelle |
|---|---|
| Sitzbindung und Profile | Bestehende Cargo-/Transportstruktur, validierter Sitz-/Proxybezug, Fähigkeiten und Bewegungsraum. |
| Waffenaktion und Steuerung | Vorhandene Inventar-/Waffen-/Aktionslogik; dünne Passagierintegration in UI, Soldier und Transport. |
| Poseauswertung | Gemeinsame reine Auswertung der Basispose und FFV-Korrekturen; getrennt von Zeitfortschritt und Aktionen. |
| Startdaten und letzte Prüfung | Aktuelle Geometrie und Trägerbewegung für ein tatsächlich anstehendes Waffenereignis; bestehender Projektilpfad bleibt erhalten. |

Die folgenden Namen sind Verantwortlichkeiten, keine Anweisung, gleichnamige Frameworks zu bauen: `SeatBinding`, `WeaponUse`, `PoseSample`, `LaunchSample`.

**Keine allgemeine Projektillogik in `EngineWgpu.cpp`, `ffi.rs` oder Rendercallbacks.** Kleine generische Korrekturen an Datenübergabe/Palette nur bei belegtem Bedarf. Keine weitere Mod-/Assetverwaltung, keine neue KI-Zielsuche neben der vorhandenen.

Ein Integrationsverantwortlicher für zentrale Soldier-/UI-/Transportstellen; andere Agenten bearbeiten klar getrennt Profile, Posemathematik und Tests. Bereits funktionierende Implementierungen behalten und erweitern. Eine gleichwertige kleinere Lösung ist zulässig, wenn sie dieselben Invarianten und Abnahmen erfüllt.

## 3. Wichtigste Präzisierung: Sitzpose ist nicht Waffenaktion

Im geprüften Code hängen `WeaponsDisabled`, Optik, Handgun-Darstellung und Bewegungslimits über `MoveInfo` am aktuellen logischen Bewegungszustand. Das Default-Paket unterscheidet beispielsweise Idle/Reload mit Waffenverbot von Aim ohne dieses Verbot. Eine nur optisch neue Oberkörperpose ändert diese Semantik nicht. [R2–R4]

**Entscheidung:** Die vorhandene Bewegungs-/Aktionsmaschine bleibt Eigentümer der semantischen Waffenaktion und ihrer Ereignisse. Der FFV-Poseadapter darf für die Darstellung zusätzlich eine geeignete Sitzbasis auswerten. Nicht die verbietenden Flags der unteren Sitzanimation als Freigabequelle einer anderen oberen Waffenaktion übernehmen.

### Konkreter Aufbau

Beim Eintritt die eigentliche Cargo-Sitzaktion bestimmen. Für einen unterstützten FFV-Platz ein wiederverwendbares Aktionsprofil passend zur Waffen-/Haltungsfamilie verwenden. Beispiele für interne semantische Zustände: abgesenkt, anheben, bereit, nachladen, wechseln, werfen, absenken.

Diese Zustände nach Möglichkeit über vorhandene `MoveId`/`MotionPathItem`/`ActionContext` ausdrücken. Ein kleiner Adapter ordnet die semantische Aktion den vorhandenen oder eigenen generischen CfgMoves-Zuständen zu. Keine zweite unabhängige Reload- oder Wurfzustandsmaschine.

Für die sichtbare Pose getrennt die Sitzbasis und den betreffenden Oberkörperanteil auswerten. Gemeinsame Aktionen pro Familie, nicht eine vollständige neue Zustands-/Clipserie pro Fahrzeug-Waffen-Paar. Der originale Cargo-Clip muss dadurch nicht nachträglich schießen dürfen.

Falls dieselbe Semantik im bestehenden Code kleiner durch einen begrenzten Adapter erreichbar ist, diesen bevorzugen. Ein pauschales `return false` in `WeaponsDisabled` oder globales Abschalten der alten Restriktionen ist keine Lösung.

### Drei unterschiedliche Fragen

1. **Kann dieser Sitz persönliche Waffen unterstützen?** Abhängig von gültiger Bindung und Profil, nicht vom aktuellen Aim-Clip.
2. **Kann diese Kandidatenwaffe nach einer zulässigen Aktion benutzt werden?** Darf auch während Absenken oder mit noch nötigem Reload positiv sein.
3. **Darf dieses konkrete Waffenereignis jetzt stattfinden?** Abhängig von tatsächlicher Aktion, Munition, finaler Pose, Hindernissen und Autorität.

Die Kamera-/Bedienroute beantwortet Frage 1, die grobe KI-Auswahl Frage 2 und die Schussausführung Frage 3. Kein Bereitschaftszirkel: Die KI muss eine Waffe wählen und anheben können, bevor sie schussbereit ist. Gleichzeitig kein Kandidat, der trotz struktureller Unbrauchbarkeit ständig gewinnt.

**Produktionskontrolle und Missionsaktionen:** Eine fremde erzwungene Animation/Cutscene darf nicht jede Simulation durch FFV-Idle verdrängt werden. Bestehende Missions-/Ausstiegs-/Todesprioritäten erhalten. `SwitchMove` nicht pro Frame zum Setzen der Pose aufrufen: Es setzt unter anderem Zielwerte und Warteschlangen zurück. [R5]

## 4. Sitzbindung, Profile und Bezugssysteme

### 4.1 Validierte Sitzbindung

Die Auflösung muss ausdrücklich erfolgreich oder fehlerhaft sein. Tatsächlichen Insassen, Träger, stabilen Cargo-Platz und vorhandenen animierten Proxy prüfen. Der bisherige Fallback von `ProxyWorldTransform` auf die Fahrzeugmatrix darf im FFV-Schusspfad nicht als gefundener Sitz gelten. [R7]

Einen kleinen validierenden Aufruf neben den bestehenden Transportfunktionen ergänzen. Bestehende Formel für Proxyanimation und Modellmittelpunkt übernehmen, nicht parallel neu schätzen. Bestehende Cargo-Indizes und Missionszuweisungen nicht umnummerieren.

Nach Ausstieg/Wiedereinstieg auch im selben Sitz darf alte ausstehende Arbeit nicht plötzlich wieder gültig werden. Vorhandene Lebensdauer-/Sitzreferenzen nutzen, erforderlichenfalls eine lokale Sitzsitzungskennung ergänzen. Nicht ausschließlich auf eventuell in Singleplayer noch fehlende Netzwerk-IDs vertrauen.

Relevante Invalidierungen: Umbesetzung, Tod/Löschung, Weltwechsel, inkompatibler Modell-/Profilwechsel. Teleport oder Korrektursprung invalidiert Bewegungs-/Posehistorie; eine weiterhin richtige Belegung muss deswegen nicht neu erfunden werden.

Angehefteter, aber ungeklärter Zustand bedeutet nicht freie On-Foot-Feuererlaubnis. Er führt zu passender Cargo-Haltung und klarer Diagnose.

### 4.2 Profile

Den vorhandenen versionierten `CargoWeaponSeats`-Vertrag rückwärtskompatibel erweitern.

- **Sitz:** Proxybezug, Basispose, neutraler Körper-/Zielrahmen, anatomisch möglicher Raum, Blickbereich, Schusssektoren, gegebenenfalls Tür-/Nachbarbedingungen.
- **Waffe:** Zusätzliche Griff-/Schulter-/Hand-/Magazinanker und Platzbedarf, soweit nicht vorhanden. Die wirkliche Waffe, Mündung, Modus, Kadenz und Munition bleiben aus vorhandenen Waffendaten.
- **Skelett:** Nur notwendige Zuordnungen und Anker für die unterstützten Körper-/Armketten. Keine automatische globale Neurigging-Pipeline voraussetzen.

Gemeinsame Fähigkeiten und Familien verwenden. Eine M16-Allowlist durch 50 Einzelnamen zu ersetzen genügt nicht. Einzelprofil-Overrides sind für echte Geometrieabweichungen zulässig.

Profile vorladen, validieren und bei relevanten Änderungen neu auflösen, nicht pro Frame Dateien suchen. Existierende Modpriorität und Deaktivierungen respektieren; `access=3` nicht global aufweichen. Vererbte Sitzprofile nur bei kompatiblem realem Modell/Layout übernehmen.

Ein ungültiges Benutzerprofil darf nicht still von einem weniger restriktiven Defaultprofil übergangen werden. Fehlende Daten verständlich melden. Geeignete Originalprofile liefern die Agenten; normale Nutzer müssen sie nicht selbst erstellen.

### 4.3 Explizite Räume

Welt → Fahrzeug → befestigte Figur/Sitz → neutraler Zielrahmen → Waffenmodell/Mündung.

Für starre Orientierungen ist die Herleitung:

```text
R_WA = R_WV * R_VC * R_CA
richtung_in_A = inverse(R_WA) * richtung_in_Welt
```

Die konkrete Engine-Matrixkonvention beachten. Diese Formel ist kein blind einzukopierender Code. Punkte und Richtungen unterscheiden; Modellmittelpunkt, RTM-X/Z-Umsetzung und zulässige Skalierung genau einmal anwenden. Nicht orthogonale/singuläre Zwischenmatrizen nicht mit einer Rotationsinversion behandeln.

**Fahrzeugsektor ist nicht zusätzliche lokale Körperrotation.** Die Öffnung im Fahrzeugraum, anatomische Reichweite im neutralen Körper-/Zielraum und tatsächliche Waffenstellung jeweils im richtigen Raum prüfen. Neutralbasis nicht aus der bereits aktuellen Ziel-/Rückstoßpose zurückgewinnen.

Alle nachfolgenden Begrenzungen einschließen, insbesondere `MoveWeapons`; nicht nur den ersten Sollwert korrigieren. Gewünschte, erreichbare und erreichte Richtung getrennt diagnostizieren. Mehrere Sektoren mit nachvollziehbarer Auswahl/Hysterese und ohne Sprung durch verbotenen Raum behandeln.

## 5. Gemeinsame Pose ohne Komplettumbau

### 5.1 Neutrale Auswertung zuerst

Den FFV-Auswertungsweg anfangs mit demselben Resultat wie bisher betreiben. Sampling der unveränderten RTMs, Kopf-/Ziel-/Beintransformationen, Rückstoß und Gesichtsanteile in nachvollziehbarer Reihenfolge erhalten.

Körperpalette, Waffenproxy, Memory-Punkte, Augen, Kamera und Mündung müssen dieselben relevanten Korrekturen verwenden. Eine sichtbare Waffe nach rechts drehen, während die Mündungsabfrage aus einer alten Pose stammt, ist nicht akzeptabel.

Eine gemeinsame Auswertung bedeutet nicht, für jede Mündungsabfrage alle Meshvertices zu skinnen. Gameplayanker gezielt berechnen; Palette bei Bedarf. Vorbereitung/Preload kann bestehende Caches nutzen, die geometrische Auswertung darf jedoch keine Spielzeit, RNG-Zustände oder Waffenaktionen fortschreiben.

**Keine Rekursion:** Rohe Basisanker vor der fertigen Pose berechnen. `RecalcGunTransform` fragt heute einen Waffenmittelpunkt ab; dieser darf im neuen Pfad nicht seinerseits dieselbe unvollständige fertige Auswertung verlangen. [R8]

Gemeinsame Bank-Animationen/Skelette nicht pro Figur verändern. Ergebnisse oder Korrekturen gehören zum Insassen beziehungsweise zu einem unveränderlichen Snapshot mit begrenzter Lebensdauer.

### 5.2 Kleinstmöglicher Overlay

Geeignete Sitzpose für Becken/Beine behalten. Eine generische obere Haltung pro Waffenfamilie an den aktuellen Sitzrumpf anbinden. Es ist nicht korrekt, obere und untere alte Matrizen beliebig zusammenzukopieren.

Zunächst einen stabilen generischen Hold herstellen. Danach begrenzte Zwei-Knochen-IK für passende Arme, wo die generische Haltung nicht reicht. Waffenhaltung führt, Hände folgen. Handfläche/Griff und Handgelenk unterscheiden; bei Reload den vorhandenen Aktionsfortschritt für die Handbewegung verwenden. Unerreichbare Haltung nicht durch längere Arme lösen.

Das klassische RTM-System mischt Matrizen linear. Im neutralen Pfad unverändert lassen. Keine globale Quaternionisierung, Orthonormalisierung oder Inversion singulärer Mischmatrizen. Zusätzliche starre Korrekturen aus validierten Hilfsrahmen erzeugen und die betroffenen Ketten konsistent mitführen. Neutrale Zusatzkorrektur muss Identität sein.

Vorhandener Rückstoß wird genau einmal angewendet. Kleine kosmetische Fingerfehler sind kein Blocker für den zweiten Fahrzeugtest; falsche Mündung, entkoppelte Hände/Waffe oder grobes Karosserieclipping sind Blocker.

### 5.3 Bindung zur vorhandenen Semantik

Die Waffenfamilienzustände benötigen passende Action-Maps, Optik-/Handgun-/Manipulationsflags und Bewegungslimits. Nicht nur sichtbare RTMs erzeugen. Die aktuelle logische Waffenaktion und die separat gesampelte untere Sitzpose dürfen dabei nicht widersprüchliche Freigaben liefern.

Zeit für Waffenaktion/Reload/Wurf aus vorhandener Waffen-/Aktionssemantik nehmen, nicht versehentlich aus einem fahrzeugspezifischen Idle-Geschwindigkeitsfaktor. Körperroot nicht durch Schieß- oder Nachladeclips vom Sitz fortbewegen.

## 6. Eingabe, Kamera und NPC-Verhalten

### 6.1 Tatsächliche vorhandene Route verwenden

`InGameUI::SimulateHUD` besitzt bereits einen Weg für Cargo als persönlichen Schützen sowie Waffenumschaltung über `NextWeapon`. `KeyboardPilot` besitzt weitere persönliche Bedienung; `InfantryController` liefert außerdem Steuersignale. Existenz dieser Funktionen beweist nicht, dass alle für den Sitz ausgeführt werden. Erst tatsächliche Aufrufe/Verbraucher verfolgen. [R6]

Vorhandene funktionierende Eingabepfade behalten. Fehlende persönliche Aktionen klein gemeinsam verfügbar machen. Nicht dieselbe Feuertaste zusätzlich in einer neuen FFV-Poll-Schleife auswerten und nicht den gesamten On-Foot-Pilot mit Lauf-/Bodenlogik in Cargo ausführen.

Eingabeflanken einmal verbrauchen. Gehaltenes Feuer nutzt vorhandene Kadenz. Waffen-/Moduswahl nicht per Frame zurücksetzen. Ungeeignete Auswahl muss sich wieder abwählen lassen.

Kontrollroute an stabile Sitzfähigkeit binden, nicht an momentane Schussfreigabe. HUD-, Inventar- und Aktionsziel müssen derselbe reale Soldat sein. Gleichzeitig Fahrerlenkung, Gruppenbefehle und montierte Waffen nicht unbeabsichtigt übernehmen.

Karte, Chat, Pause, Freefly, Kamerasequenz und verlorener Fokus erhalten ihre vorhandenen Sperren. Ein Klick ins Einstiegsmenü darf nicht ungeprüft als neuer Feuerschuss gelten. Vorhandene Konsumsemantik prüfen, nicht global sämtliche Eingaben löschen.

### 6.2 Blick, Visier und Kamera

Passagierkamera auch bei Absenken, Waffenwechsel, Reload und erneutem Einsteigen stabil. Freies Umsehen vom Waffenraum trennen. Außerhalb der Reichweite nicht heimlich in eine andere Richtung feuern; gewählte Absenk-/Blockierungsregel verständlich darstellen.

Kamera-/Mündungsversatz und bestehende Visiernullung konsistent behandeln, ohne ungefragt das gesamte On-Foot-Zielverhalten zu ändern. Spielerziel kann ein Blickstrahl mit endlichem Zielpunkt sein; Mündungsrichtung, Laufachse, Visierkorrektur und Streuung sind getrennte Größen. Kein Projektilteleport auf die Bildschirmmitte und keine neue Auto-Aim-Hilfe.

Notwendige Zoom-/Steuerungswerte rechtzeitig als Simulationsinput erfassen. Die autoritative Schussprüfung braucht keine aktive Renderkamera und keine GPU-Rücklesung.

### 6.3 KI

Günstige Kandidatenverwendbarkeit von exakter momentaner Schussfreigabe trennen. Bestehende Waffenwahl erweitern, keine globale neue Kandidaten-/Zielmaschine. Strukturell ungeeignet, vorübergehend nicht bereit und noch nicht exakt geprüft unterscheiden.

Abgesenkte Waffe und notwendiger Reload dürfen Kandidatenauswahl nicht blockieren. Ungeeigneter Werfer darf nicht endlos ein brauchbares Gewehr verdrängen. `_currentWeapon`-Sichtbarkeit nicht ungeprüft als exakte Mündungssicht jedes Kandidaten behandeln. Detailprüfung für relevante Kandidaten und endgültig die wirkliche Mündung.

Vorhandene Sensoren, Zielkenntnis, Feldsicht, Combat Mode, Hold Fire und Freundschutz erhalten. Die wahrnehmende Figur darf nicht erst vollständig schussbereit sein müssen, um ein Ziel zu erkennen. Kein allwissender Zusatzscan.

Befohlene Feuerkette und autonomer Cold Start getrennt abnehmen. `reveal`/`doTarget`/`doFire` sind zulässige Mittel für den ersten Test, nicht für den zweiten. Normale ausdrückliche Ausstiegsbefehle nicht blockieren; automatische Kampf-Aussteigepolitik nur gezielt an unterstützte Sitze anpassen.

## 7. Tatsächliche Waffenereignisse, Ballistik und Kollision

### 7.1 Ausführung statt globale Tick-Neuordnung

Alle tatsächlichen Auslösewege aufnehmen: manueller Schuss, NPC, Salvenfortsetzung, Script und verzögerter Animationsabschluss. Die relevante Autorität erstellt unmittelbar vor einer Ausführung die aktuellen Startdaten und prüft sie. Bestehende Geschoss-/Munitions-/Effektfunktionen führen die Aktion aus.

Keine generelle Pflicht, den vollständigen Engine-Tick umzuordnen. Wenn ein belegter FFV-Pfad mit altem Waffen-/Fahrzeugzustand arbeitet, dort minimal Absicht und Ausführung trennen beziehungsweise den erforderlichen Zustand konsistent bereitstellen. Nicht Elternfahrzeug oder Waffenzeit ein zweites Mal simulieren.

Sitz-, Pose- und Kollisionszustand müssen zur Phase des tatsächlichen Waffenereignisses passen. Rendererinterpolation ist davon getrennt. Der Renderer darf Träger und Insasse nicht aus verschiedenen Präsentationszeiten zusammensetzen.

`LaunchSample` ist nicht dauerhaft gültig. Erforderliche Invalidierung nach Bindung, Waffe/Mündung/Modus, Pose oder relevantem Hinderniszustand. Nicht nur ein Sample pro Tick: mehrere Ereignisse können dieselbe Ticknummer besitzen, aber unterschiedliche gültige Zustände.

### 7.2 Lebensdauer und vorhandene Aktionskontexte

Fehlende Clips dürfen nicht unbemerkt den Sofort-Callback von `PlayAction`/`SwitchAction` aktivieren. Vorprüfung und ausdrücklich unterstützter Fallback oder benannte Ablehnung. Queue-Endziel ist keine tatsächlich erreichte Pose. [R5]

Vorhandene Kontexte und Magazine-IDs nutzen. Keine nackten Indizes über Inventaränderungen hinweg als Identität behandeln. Eine Waffen-/Mündungsentfernung und spätere Wiederanlage desselben Namens darf keine alte Aktion wieder legitimieren.

Bei Ereignisabschluss Kontext nach vorhandener Semantik einmal konsumieren; reentrante Callbacks nicht denselben noch aktiven Kontext erneut auslösen lassen. Nach Löschung, Ausstieg oder Inventaränderung nicht mit alten Referenzen fortfahren. Keine allgemeine Rückabwicklung beliebiger Scripts versprechen.

Legitime Salven-/Mehrprojektilsemantik beibehalten. Nicht „ein Tick = ein Schuss“ oder „ein Projektil = ein Magazinabzug“ fest einbauen. Eine Demonstrations-ID aus dem beigefügten Modell ist keine Aufforderung, einen unbegrenzten globalen Event-Speicher anzulegen.

Nach Abbruch muss erneutes zulässiges Schießen wieder möglich sein. Ein dauerhaft gesperrtes System besteht die Abnahme nicht allein dadurch, dass es keine falschen Schüsse erzeugt.

### 7.3 Flugmodell und Vererbung

Jede freigeschaltete Familie benutzt ihren bestehenden Weg, aber passende Startdaten. `FireMGun` ist kein Nachweis für `FireShell`, `FireMissile` oder einen verzögerten Wurf.

Die vorhandene Trägerbewegung am Welt-Freigabepunkt genau einmal hinzufügen. Für den starren Träger gilt grundsätzlich `v + omega × r`; vorhandene `AttachedPointSpeed`-Logik prüfen/verwenden. Kein zusätzlicher unkontrollierter Geschwindigkeitsanteil aus Renderinterpolation oder numerischem Differenzieren kosmetischer IK. Zusätzliche artikulierte Bewegung nur mit ausdrücklich definiertem, getesteten Datenbezug.

Vorhandene `CompensateAim`, `SimulateAgainstTarget` und Parameterquellen prüfen, bevor neue Flugmodelle entstehen. Die tatsächliche `Man::GetAimed`-Überschreibung ist relevant. Vorhersage und Flug müssen dieselbe wirkliche Anfangsgeschwindigkeit berücksichtigen; bei Luftwiderstand ist nachträgliches lineares Addieren eines Trägerwegs nicht allgemein exakt.

KI-Schätzfehler, Skill, Streuung und bewusst nicht modellierte Einflüsse erhalten. Kein Vergrößern der Trefferbox oder perfektes Stabilisieren zur Rettung eines Kurvenfixtures. Unterschiedliche numerische Integrationsschritte als Unsicherheit messen; kein globaler Fixed-Step-Umbau als Nebenaufgabe.

Vorhersagekosten begrenzen: zuerst billige Eignung/Reichweite, dann auf ausgewählte Kandidaten oder relevante Änderungen beschränkte genaue Lösung. Nicht Ziel × Waffe × Passagier × Frame vollständig integrieren.

### 7.4 Drei Kollisionsfragen

1. Passt Körper/Waffe tatsächlich in die gewünschte Stellung?
2. Ist Mündungs-/Freigabepunkt gültig, ohne Durchschieben durch Hindernisse?
3. Was trifft ein danach korrekt gestartetes Projektil?

Ein nahes Objekt vor gültiger Spielermündung darf getroffen werden. Ein unzulässig durch die Wand geführter Lauf darf dahinter kein Projektil erzeugen. NPCs behalten strengere Freund-/Sichtregeln.

Vorhandene Radius-Abfrage vor dicken Waffenhüllen prüfen; der bereits dokumentierte Vorfilterbefund erhält einen echten C++-Regressionstest. Bei Bestätigung minimal konservativ korrigieren. Punktstrahl, Null-Länge, Startüberschneidung, dünne Wand und relevante Bewegungswege testen. Ein Radiusparameter allein ist kein Beweis vollständiger kontinuierlicher Kapselkollision.

Mit kleinen Waffenhüllen und vorhandener Objekt-/Proxykollision arbeiten, nicht eine zweite Kollisionswelt einführen. Selbstkontakte des Schützen gezielt behandeln; nicht Träger und sämtliche Mitfahrer ignorieren. Geschossursprung bleibt dem Soldaten zugeordnet, nicht zur Vermeidung von Selbsttreffern pauschal auf den Träger umhängen.

Unbekannter/abgelaufener Freiraum ist keine positive Freigabe. Letzte gültige Haltung halten/absenken, neue Aktion blockieren, aber eine neue gültige Gelegenheit zeitnah prüfen. Keine dauerhafte Verhungerung durch nie bediente Prüfbudgets.

## 8. Darstellung, Save/Load, Netzwerk und Kosten

### Darstellung

Aktiven Waffenproxy passend zu Waffe/Mündung/Modus und vorhandenen Sonder-/Magazinmodellen zeigen. Cargo-Hide-Regeln dürfen eine freigeschaltete Sekundärwaffe nicht weiter pauschal unsichtbar machen. Aktive und verstaute Waffen unterscheiden.

Innen-/Außenansicht, Schatten und LOD-Wechsel prüfen. Ein gemeinsamer Waffenrahmen bedeutet keine Verdopplung des Meshes als zusätzliche Weltfigur. Freigegebene Pose muss an den vorhandenen GPU-/CPU-Skinningpfad anschließbar sein, ohne Gameplay von dessen Ausführung abhängig zu machen.

Paletten-/Snapshot-Lebensdauer korrekt besitzen. Nicht neue mutable Arrays über einen parallelen Renderpfad teilen. Bestehende Übergabeinfrastruktur nutzen und nur bei belegtem Bedarf minimal korrigieren.

### Save/Load und Fehlerfälle

Stabile bestehende Referenzen und notwendige Aktions-/Sitzinformation speichern, nicht rohe Pointer oder ganze Renderpaletten. Nach Referenzauflösung Bindung und Inventar prüfen. Gültige Aktion genau einmal fortsetzen; verbrauchte/abgebrochene Aktion nicht wiederholen.

Fehlendes oder inkompatibles Paket stellt passende ursprüngliche Cargo-Haltung her. Kein stehender Soldat durch das Dach, keine Phantomschüsse, keine Magazinauffüllung. Alte Wasser-/Lauf-/Leiterzustände beim Einsteigen nicht blind auf Cargo übertragen; konkrete Übergänge testen statt unbelegte Altbugs zu behaupten.

### Multiplayer

Autoritätsvertrag früh festhalten, separat freischalten. Tatsächliche bestehende Locality-/Netzwerklogik verwenden, nicht pauschal eine bereits vorhandene Serverautorität unterstellen. Remoteprofil/-pose muss darstellbar sein, ohne dort nochmals autoritative Aktionscallbacks/Projektile auszuführen.

Träger und Passagier mit unterschiedlichen Eigentümern, Besitzerwechsel, Late Join während Reload, veraltete Nachrichten und Paket-/Protokollkonflikte testen. Keine neue verteilte Exactly-Once-Garantie oder parallelen Netzwerkstack einführen. Bis zur Abnahme klarer MP-Fallback, nicht bloß die heutige Sperre löschen.

### Kosten

Wiederverwendbare Profile vorladen. Gameplayanker unabhängig von Sichtbarkeit aktuell halten; kosmetische IK-Detaillierung darf abgestuft sein, darf aber die gameplayrelevante Mündung nicht verändern.

Gleiche Szene mit 1/8/32/64 aktiven Passagieren abnehmen, jeweils Pose-, Kandidaten-, Kollisions-, Vorhersagekosten und Allokationen messen. Vorher-/Nachher unter gleichen Einstellungen, ohne Videoaufnahme als Performancegrundlage. FFV ist zusätzliche Funktionalität, kein versprochener FPS-Gewinn.

## 9. Umsetzung in vier abnehmbaren Lieferungen

### Lieferung 1 — ein gemeinsamer Gewehrpfad

Normalen Einstieg und installierten Kontrollstand sichern. Zwei gültige Gewehr-Loadouts auswählen. Namensbindung durch passende Familienfähigkeit ersetzen; stabile Sitz-/Bedienroute und vorhandene Waffenaktion anbinden. Aktuelle Mündungs-/Feuerwege schließen, neutrale gemeinsame Pose zunächst unverändert.

**Abnahme:** Beide Loadouts funktionieren nacheinander im selben geeigneten Sitz; nachladen und aus-/wiedereinsteigen über den normalen Weg. Die vorhandene Inventory-Policy bleibt unverändert. Ein installierbarer Zwischenstand wird geliefert; Grenzen offen benennen.

### Lieferung 2 — eigentlicher Generalisierungsnachweis

Zweites geeignetes Fahrzeug mit anderem Proxy-/Sitzaufbau und anderer Sitzorientierung. Kleinsten notwendigen generischen Pose-/Zieladapter umsetzen, keine Sonderkorrektur für den neuen Fahrzeugnamen. Zulässigen Waffen-/Moduswechsel zusätzlich testen. In mindestens einem Fahrzeug mehrere unabhängige Insassen.

**Abnahme:** Volle 2×2-Matrix. Danach ein zurückgehaltenes drittes geeignetes Beispiel nur über Profile. Ein fachlich allgemeiner Bugfix ist erlaubt, aber dann den Hold-out nicht als unverändert bestandenen Daten-only-Test ausgeben. Nach Fix gesamtes Set wiederholen und neues zurückgehaltenes Beispiel wählen.

### Lieferung 3 — verlässliche KI und Bewegung

Autonomer Start ohne künstliche Zielkenntnis; befohlene Feuerkette separat. Stand, Beschleunigen/Bremsen, tatsächliche Drehung zum Schusszeitpunkt und Unebenheiten. Genauigkeit nicht mit technischem Schussnachweis vermischen. Save/Load, Abbruch, fehlende Pakete und Mehrinsassenbetrieb vollständig abnehmen.

**Abnahme:** Funktioniert nicht nur nach Diagnoseeingriffen oder bei einer kurzen zufälligen Zielgelegenheit. Unterstützte Konfigurationen erhalten nachvollziehbare Fortschritts-/Reaktionsgrenzen bei stabil gültigen Bedingungen.

### Lieferung 4 — Produktabdeckung und MP

Weitere bereits unterstützte Waffenfamilien: MG/MP, Pistole, Unterlaufwerfer, Schulterwerfer, Wurfwaffen. Deren verschiedene Freigabe-, Mündungs- und Nachladewege nicht als Gewehrsimulation vereinheitlichen. Andere Ausrüstung explizit abgrenzen. Geeignete Originalplätze inventarisieren und Default-Profile vervollständigen. Kosten, frische Release-Installation und separate Multiplayer-Abnahme.

**Abnahme:** Eine veröffentlichte Matrix zeigt, was technisch angeschlossen, normal spielbar, nicht geeignet oder noch offen ist. Zwei Gewehre bleiben nicht das Ersatzabschlusskriterium für alle versprochenen Familien.

## 10. Tests, Diagnosen und Stoppregel

### 10.1 Reale Bedienung

Außerhalb des Fahrzeugs beginnen, echtes Aktionsmenü, relative Maus und normale Tasten. Keine Kameraübernahme, `crew_cursor`, erzwungene Pose oder Zielmanipulation als Ersatz. Diagnose-/Scriptfixtures behalten, aber anders beschriften.

Gültige Loadouts, keine erzwungenen zwei Primärwaffen. Auto-/Einzelfeuer, Loslassen, zulässiger Modus-/Waffenwechsel, Reload, Absenken/Anheben, Ausstieg, Wiedereinstieg. Sondereingaben in Pause/Karte/Chat separat.

### 10.2 Unabhängige Oracles

Neutralpose gegen erhaltenen gültigen Altpfad oder Referenzdaten, nicht zwei Abfragen desselben neuen Helpers. Startdaten gegen das tatsächlich erzeugte Engineprojektil vor erster Integration; danach dessen echten Verlauf beobachten.

Reine Geometrie: andere neutrale Waffenrichtung, seitlicher/rückwärtiger Sitz, Roll/Pitch/Yaw, Modellmittelpunkt, LOD-Wechsel und Verschiebung des Weltursprungs. Flugbahnsymmetrien nur unter passender Gravitation/Wind verwenden; nicht beliebige Raumrotation als unveränderte Ballistik voraussetzen.

Gezielte Mutationen müssen Tests rot machen: falscher Proxyoffset, falscher Modus, falscher Bezug, ignorierte Invalidierung, doppelte Aktionsausführung. Ein System, das immer blockiert, darf die positiven Fälle nicht bestehen.

### 10.3 Lebensdauer und Fortschritt

Fehlender Clip/Anker, leere Waffe mit und ohne Reserve, ungeeigneter Kandidat mit brauchbarer Alternative, Wiedereinsteigen in denselben Index, Waffenentfernung/Ersetzung, Türänderung zwischen Vorbereitung und Schuss, Nachbarinsasse, Ausstieg während Salve, Tod im Callback, Save/Load mitten im Reload.

Unter stabil erfüllten Voraussetzungen muss nach vorhandener gültiger Übergangs-/Reload-/Kadenzzeit tatsächlich eine Aktion möglich werden. Kein unbegrenztes Blockieren wegen permanenter Neuinitialisierung, Queue-Neustart oder nie abgearbeiteter Freiraumprüfung. Fristen aus realen Profil-/Waffenzeiten ableiten und vor dem Lauf festlegen, nicht auf einen einzelnen gelungenen Lauf hintrimmen.

### 10.4 Wirkliche Bewegung und saubere Fehlerklassen

Am Schussereignis Trägergeschwindigkeit und tatsächliche Winkelbewegung mit passendem Zeitbezug protokollieren. Konstanter 45-Grad-Kurs ist keine Kurve. Heading-Wrap richtig behandeln; Teleports/ungültiges dt nicht als extreme Lenkbewegung zählen.

Unterscheiden: `PASS`, `FAIL`, `PRECONDITION_FAILED/INCONCLUSIVE`. Wenn Fahrer nie fährt oder nie ein ausreichendes Zielzeitfenster entsteht, kein Runtime-Erfolg. Wenn Bedingungen erfüllt sind und trotzdem keine gültige Aktion erfolgt, nicht beliebig wieder als Fixtureproblem abtun.

### 10.5 Diagnose ohne neue Eingriffe

Begrenzte Diagnose für ausgewählten Insassen: stabile Sitzbindung, Profil, aktuelle und Kandidatenwaffe/Mündung/Modus, logischer Zustand und wirkliche Phase, Soll-/Grenz-/Istrichtung, Mündung/Startbewegung, Zustandsrevision sowie frühester relevanter Ablehnungsgrund.

Beispiele: `seat-unresolved`, `unsupported-use`, `missing-action`, `raising`, `reloading`, `pose-unreachable`, `carrier-obstruction`, `input-suspended`, `authority-denied`, `stale-binding`.

Kein unbeschränktes Frame-Logging, kein Diagnosecode, der selbst Zielwissen, Zielrichtung oder Schüsse verändert. Reproduktion → Hypothese → minimaler Patch → gleicher Test plus Regressionen.

### 10.6 Stoppregel für Planänderungen

**Diese Architektur jetzt implementieren.** Eine neue grundsätzliche Planung ist nur bei einem reproduzierbaren Widerspruch, einem nachgewiesenen Kostenproblem oder einer tatsächlich anderen Anforderung nötig. Ein weiterer denkbarer Randfall wird zunächst einem vorhandenen Vertrag/Test zugeordnet, nicht automatisch zu einem neuen Subsystem.

Die Grenze zwischen Profilfehler, Enginefehler und noch nicht zugesagter Inhaltsabdeckung bleibt sichtbar. Keine erneute allgemeine Freigabe durch den Nutzer für jeden vorgesehenen kleinen Implementierungsschritt verlangen. Gefährliche Änderungen an fremden Arbeiten oder laufendem Spiel selbstverständlich nicht eigenmächtig durchführen.

## 11. Lieferung und Definition of Done

Eigene Profile und Animationen über den vorhandenen Build nach `@OP_VehicleActions`, standardmäßig geladen. Originalarchive unverändert lokal lesen, keine fremden Spielassets im Release mitkopieren. Eigene Quellen/Generatoren und Herkunft nachvollziehbar halten. Nutzer benötigen kein Blender/Python und keine Diagnoseparameter.

Default, Disable, fehlendes/inkompatibles Paket, Benutzer-Modpriorität und frische Installation testen. Normale Spielerbedienung kurz dokumentieren.

Jeder Statusbericht nennt tatsächlich getesteten Binary-Commit, lokale Abweichungen, Paket-/Profilhash, Inputmethode, Szene, erfüllte Voraussetzungen und offene Grenzen. Verteilte EXE/DLL und Inhaltspaket müssen zur ausgewiesenen Abnahme passen. Fehlgeschlagener Shutdown/Watchdog/fehlende Welt darf keinen grünen Integrationsnachweis ergeben.

**Architektur fertig:** Die volle 2×2-Matrix und ein wirkliches Daten-only-Hold-out bestehen mit gemeinsamer Waffenlogik, konsistenten Räumen und normaler Bedienung. Keine Fahrzeug-/Waffennamen-Sonderfälle im generischen Pfad.

**Produktabdeckung fertig:** Die zugesagten geeigneten Plätze und Waffenfamilien bestehen ihre konkreten Runtime-, Lebensdauer-, Kosten- und Releasefälle; MP besitzt eine eigene bestandene Freigabe.

**Sofortiger Arbeitsbeginn:** Lieferung 1, dann Lieferung 2. Die Spezifikation ist ausreichend stabil. Weitere theoretische Perfektion ist nicht Voraussetzung für den nächsten kleinen, abgenommenen Patch.

---

## Prüfvermerk und Quellen

### Was in dieser letzten Runde bestätigt beziehungsweise präzisiert wurde

- Der aktuelle Default-Inhalt definiert weiterhin genau den Jeep/M16-Einstiegsfall, nicht bereits eine allgemeine Produktabdeckung.
- Die logische Waffenaktion ist über `MoveInfo` mit Darstellung und Verboten verknüpft. Daher benötigt die Verbindung von Sitzbasis und oberer Waffenhaltung eine ausdrückliche semantische Anbindung, nicht nur neue Matrizen.
- Die UI hat bereits Teile des persönlichen Cargo-/Waffenwechselpfads. Nicht daneben eine konkurrierende Eingabeschleife implementieren.
- `SwitchMove` und fehlende Actions besitzen konkrete Seiteneffekte. Ein Modell-/Posewechsel ist nicht gleich ein harmloser Wechsel einer sichtbaren Textur.
- Positive Fortschrittseigenschaften wurden der Abnahme gleichberechtigt zu den Verboten hinzugefügt: keine falschen Aktionen UND funktionierende erlaubte Aktionen.
- Keine neue Designgrundlage gegenüber V5 erforderlich. V6 konkretisiert den Einbau und friert Umfang und Abnahme ein.

### Projektquellen

Referenzbasis (für alle folgenden Pfade, sofern nicht anders angegeben):

```text
https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/tree/d6f60ff097e1b7f528812e2facdce66e926f4367
```

R1. Branch und Vergleich von `88b9bbd8f2d9347c5d475f1ad4f1c264bacb07f0` auf die Referenz. FFV-Kernpfade aus V5 wurden durch dieses Delta nicht als neue FFV-Implementierung ersetzt.

R2. `content/default-packs/vehicle-actions/config.cpp`: `OPJeepActions`, `OPJeepIdle/Aim/Reload` und `CfgOPVehicleActions::Jeep`; Waffenverbot im logischen Zustand und enges Defaultprofil.

R3. `engine/Poseidon/World/Entities/Infantry/SoldierOld.hpp`, insbesondere 840–1035: `MOVE_INFO_TEST`, `MOVE_INFO_VALUE`, Waffen-/Animationsabfragen.

R4. `engine/Poseidon/World/Entities/Infantry/SoldierOldAI.cpp`, 25–110: `GetConfiguredCargoWeaponSeat`, `GetActiveCargoWeaponSeat`, `CargoWeaponLineClear`. Weitere zuvor geprüfte Stellen: `FireWeapon`, `FireInRange`, `CalculateAimWeapon`, `GetAimed`.

R5. `engine/Poseidon/World/Entities/Infantry/SoldierOldMove.cpp`, 300–590: Aktionsabschluss, `SwitchMove`, `GetCurrentMove`, `PlayAction`, `SwitchAction`, `SwitchVehicleAction`.

R6. `engine/Poseidon/UI/InGame/InGameUIMenuSim.cpp`, `SimulateHUD`, Cargo-Umschaltung und `NextWeapon`; `engine/Poseidon/Input/InfantryController.cpp`, `Poll`; `SoldierOldActions.cpp`, `KeyboardPilot` aus den vorausgehenden Codeprüfungen.

R7. `engine/Poseidon/World/Entities/Vehicles/TransportCore.cpp`: `GetProxy`, `ProxyWorldTransform`, `DrawCrewMember`, `DrawProxies`, `Simulate`; in vorangegangenen Prüfungen gelesen, weiterhin als Integrationsstellen relevant.

R8. `SoldierOldActions.cpp`: `RecalcGunTransform`, `MoveWeapons`; `SoldierOld.cpp`/`SoldierOldSimProxy.cpp` und `World/Simulation/Animation/RtAnimation.hpp/.cpp`: Roh-/fertige Pose und Palette; Befunde der vorausgehenden Prüfungen.

R9. `World/Entities/Weapons/Ballistics.hpp`, `Shots.cpp`, `AI/VehicleAICombat.cpp`, `World/Simulation/Collisions.cpp`, `World/Detection/TargetFire.cpp`: vorhandene Vorhersage-/Start-/Kollisions- und Kandidatenpfade. Nicht jede Funktion dieser Dateien wurde in dieser letzten Runde erneut vollständig gelesen.

R10. `docs/roadmap/decisions/JEEP-FFV-20260911.md`, 420–620: dokumentierte Grenzen von Fahrtests, Scriptboarding gegenüber Aktionsmenü und aktueller Paketabnahme.

### Öffentliche Primärquellen

```text
https://dev.arma3.com/post/oprep-firing-from-vehicles
https://guillaumeblanc.github.io/ozz-animation/documentation/ik/
```

Bohemias Dokument erläutert persönliche Waffen aus geeigneten Plätzen sowie nötige Raum-/Kompatibilitätsabwägungen. Es ist kein offener Drop-in-Code. ozz beschreibt begrenzte IK und notwendige Ketten-/Koordinatenbedingungen, keine automatische Lösung für das klassische Poseidon-RTM-Format. Keine Pflicht, ozz als Runtime-Abhängigkeit einzubauen.

### Ausgeführte Modellprüfungen

1. V4 erneut: vier synthetische mathematische Fälle, erfolgreich.
2. V5 erneut: fünf synthetische mathematische Fälle, erfolgreich.
3. `contract_model.py`: 13 begrenzte abstrakte Zustands-/Ereignistests, erfolgreich. Geprüft: Kandidat vs. bereit, Reload mit Reserve, fehlender Clip, Remote-Fähigkeit ohne Autorität, Wiedereintritt, veraltete Tür-/Modusdaten, seiteneffektfreies Sampling, mehrere verschiedene Ereignisse im selben Tick, Duplikat, Callback-Ausstieg und positive Fortschrittsbedingung.

**Evidenzgrenze:** Diese Modelle enthalten keine Engine-Simulation, Assets, Grafik, echte Waffenaktionen oder Netzwerkschicht. Sie sind ausführbare Spezifikationsbeispiele. Die geforderten C++-/Spieltests sind erst durch die Agenten zu implementieren und auszuführen. Ein grünes Modell ist keine Runtime-Abnahme; das Ergebnis lautet „Plan zur Umsetzung geeignet“, nicht „FFV funktioniert bereits universell“.
