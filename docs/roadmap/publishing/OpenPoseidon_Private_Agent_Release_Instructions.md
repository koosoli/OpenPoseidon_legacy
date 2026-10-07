# Private Agentenanweisung: Freigabe, Staging und Veröffentlichung

**Status:** v3 – zur Integration in das private Repository vorgeschlagen; noch kein aktiver Workflow.
**Geltungsbereich:** Private OpenPoseidon-Entwicklung und private Release-Vorbereitung.
**Veröffentlichung:** Diese Datei und alle Agentenanweisungen bleiben außerhalb des öffentlichen Exports und des Sponsor-Downloadrepositories.

## Grundregel

Private Entwicklungsarbeit ist nicht automatisch zur Veröffentlichung freigegeben. Nach jedem abgeschlossenen zusammengehörigen Änderungspaket den Owner fragen, ob genau dieses Paket in den Veröffentlichungs-Stagingstand übernommen werden soll. Eine erteilte Veröffentlichungsfreigabe erlaubt danach die automatische Prüfung, Synchronisierung des bereinigten Quellcodes und Bereitstellung der zugehörigen Sponsor-Binaries – ausschließlich innerhalb ihres genauen Umfangs.

Keine Veröffentlichung bei fehlender, unklarer, widerrufener oder für einen anderen Kandidaten erteilter Freigabe. Tests und Agentenanweisungen ersetzen keine Freigabe. Eine lokale Installation für den Owner, ein privater Commit und ein privater Backup-Push sind jeweils von Public-/Sponsor-Veröffentlichung zu unterscheiden.

## Bestätigtes Quellenziel und erste Freischaltung

Das zukünftige öffentliche Quellenrepository ist **koosoli/OpenPoseidon**, GitHub-Repo-ID **1406091033**, vorgesehener Branch `main`. Der Owner hat es neu angelegt. Bei der Prüfung vom 5. Oktober 2026 war es privat, eigenständig (`fork=false`) und ohne Branches. Die frühere Repo-ID 1307634938 ist NICHT das Ziel.

Dieses Ziel vom ersten Commit an wie ein öffentliches Repository behandeln: nur bereinigten Source, geprüfte öffentliche Dokumentation, zulässige Ressourcen und erforderliche Lizenzen übernehmen. Keine Agentenregeln, internen Roadmaps, Freigabe-/Ausschlussberichte, privaten Commits, Binärpakete oder privaten Actions-Logs – auch nicht auf Nebenbranches oder in Issues/PRs.

Sichtbarkeit nicht selbst ändern. Das erstmalige Umschalten auf public erfordert eine eigene ausdrückliche Owner-Entscheidung. Bis dahin ist ein freigegebener sauberer Kandidaten-Push **CLEAN_TARGET_PRIVATE**, nicht "öffentlich synchronisiert". Keine Sponsor-Binaries freigeben, bevor genau der zugehörige Quellstand tatsächlich öffentlich zugänglich ist. Ziel-ID und Sichtbarkeit vor jedem Publikationsschritt prüfen.

## 1. Zu Beginn einer Session

- Aktuelles privates Repository, tatsächlichen Arbeitsbranch und Commit prüfen. Nicht einen historischen Trunk-Namen aus alten Anweisungen ungeprüft verwenden.
- Letzten veröffentlichten Source-Commit, letzten erfolgreichen Sponsor-Build und offene Release-Kandidaten ermitteln, soweit bereits eingerichtet.
- Eigene Worktree-/Index- und Build-/Deploy-Sicherheitsregeln erhalten; fremde Änderungen nicht mitnehmen.
- Eine angefangene Freigabe gehört zu ihrem unveränderlichen Kandidaten, nicht zum jeweils neuesten HEAD. Private Weiterentwicklung darf parallel weitergehen, ohne den laufenden Release zu verändern.
- Bei fehlender Pipeline-/Zielkonfiguration nur privat vorbereiten. Keinen Zielnamen, Sponsorpreis, Organisation oder Zugangsschlüssel erfinden.

## 2. Wann die Frage gestellt wird

Einmal am Ende eines logisch abgeschlossenen Fixes, Features oder abgestimmten Änderungspakets fragen, nicht nach jedem Zwischencommit. Bei Session-Abschluss noch nicht bewertete Pakete zusammenfassen. Reine interne Roadmap-, Agenten- und Messprotokolländerungen als privat kennzeichnen; dafür nicht wiederholt eine öffentliche Freigabe verlangen.

Vor der Frage eine private Vorschau erzeugen: Änderung gegenüber dem letzten öffentlichen Stand, alle tatsächlich mitkommenden Abhängigkeiten, Build-/Teststatus, bekannte Einschränkungen, geänderte öffentliche Dokumentation, Dateiausschlüsse und vorgesehener Release-Kanal. Ein privater Draft darf vorbereitet werden, solange dadurch keine publikationsfähige Staging-Freigabe ausgelöst wird.

Empfohlene Frage:

> Soll ich „<Änderungspaket>“ als Kandidat <ID/Kurz-Hash> für Staging und automatische Veröffentlichung freigeben? Nach erfolgreichen Prüfungen wird genau dieser bereinigte Stand in <öffentliches Ziel> veröffentlicht und als <Kanal/Plattformen> im Sponsor-Build-Repository bereitgestellt. Alternativen: nur privat behalten oder ausschließlich privat für Staging vorbereiten.

Im Bericht vollständigen Kandidaten-SHA und Exportmanifest-Hash angeben. Kein pauschales „alles synchronisieren?“ ohne sichtbar abgegrenzten Umfang.

## 3. Freigabemodi

- **PRIVATE_ONLY:** Weiterentwicklung und private Sicherung erlaubt; keine Release-Übernahme.
- **PREPARE_ONLY:** Privaten Release-Draft und Berichte vorbereiten; kein Public-Push und kein Sponsor-Release, auch bei grünen Tests.
- **PUBLISH_AFTER_GATES:** Exakten Kandidaten nach vollständigen Pflichtprüfungen automatisch veröffentlichen. Keine zweite routinemäßige Nachfrage, solange Inhalt, Ziele, Kanal und Prüfregeln unverändert bleiben.

Eine Antwort, die nur „Staging vorbereiten“ verlangt, ist nicht PUBLISH_AFTER_GATES. Ein bloßes Ja ist nur auf die unmittelbar vorgelegte eindeutige Freigabefrage bezogen, keine dauerhafte Erlaubnis für spätere Änderungen.

Die Freigabe bindet Kandidaten-Commit, Dateimanifest, öffentlichen Ausgangscommit, beide Ziel-Repositories, Zielbranch, Release-Kanal, Plattformen, öffentliche Release Notes sowie die verwendete Prüf-/Exportpolicy. Änderungen daran erfordern einen neuen Kandidaten und neue Freigabe. Ein unveränderter Wiederholungsversuch nach einem Netzwerk-/Runnerfehler benötigt keine neue inhaltliche Freigabe, muss aber noch gültig und nicht widerrufen sein.

Eine durch einen Agenten geschriebene Datei mit `approved: true`, ein PR-Titel oder ein Commit-Kommentar ist kein technischer Freigabenachweis. Der Publisher muss den authentifizierten Owner-Entscheid unabhängig prüfen. Kann die Umgebung einen Chat-Entscheid nicht zuverlässig authentifizieren, dem Owner den einmaligen GitHub-Freigabeschritt für genau diesen Kandidaten vorlegen.

## 4. Auswahl statt blindem Kopieren des privaten HEAD

Ein früher auf PRIVATE_ONLY gesetztes Feature darf nicht beim nächsten freigegebenen Fix unbemerkt öffentlich werden. Deshalb den Release-Draft aus dem letzten freigegebenen Stand plus den ausdrücklich ausgewählten Änderungen aufbauen. Eine Allowlist beantwortet, welche Dateien grundsätzlich veröffentlicht werden dürfen; sie genehmigt nicht automatisch jede Änderung innerhalb dieser Dateien.

Abhängigkeiten ebenfalls prüfen: braucht der gewünschte Fix weitere bislang private Änderungen, diese vor der Freigabe mit anzeigen. Nicht heimlich hinzufügen und nicht durch Verstümmeln des Codes umgehen. Der gesamte neue öffentliche Diff muss erklärt und genehmigt sein.

Die vollständige freigegebene Kompatibilitätsschicht bleibt enthalten. Keine automatische Entfernung nach Produktnamen oder Dateiendungen. Keine erfundenen Herkunftsbehauptungen und keine Entfernung erforderlicher Copyright-/Lizenzhinweise.

## 5. Sauberer Export ohne private Historie

- Staging und Entwickler-Worktrees nicht direkt in das öffentliche Repository mergen oder pushen.
- Ausschließlich das geprüfte Dateimanifest exportieren, einschließlich legitimer Löschungen und Umbenennungen im verwalteten öffentlichen Dateibaum.
- Erstveröffentlichung: eigenständiger Root-Commit ohne private Eltern. Folgeversionen: neue Commits auf der bereits öffentlichen Historie, nicht jedes Mal eine neue Historie.
- Keine private `.git`-Struktur, Branches, Tags, Notes, Submodule-URLs oder Commit-Nachrichten übernehmen. Kein `push --mirror`, kein `push --all`, kein pauschales Tag-Push und kein Force-Push.
- Private Agentenregeln, Handovers, Roadmaps, Audits und Rohdaten ausschließen. Erforderliche öffentliche Build-/Installationsanweisungen, Tests, Lizenzen und technische Quelldokumentation erhalten.
- Bei Änderungen durch andere Maintainer im öffentlichen Ziel stoppen, vergleichen und neu abstimmen. Nicht fremde Beiträge mit einem Snapshot überschreiben.

## 6. Pflichtprüfungen und Binaries

Aus exakt dem bereinigten, unveränderlichen öffentlichen Kandidaten bauen, nicht aus dem privaten Arbeitsverzeichnis oder dessen `dist/`. Vollständige erforderliche Quellen, Abhängigkeiten, Shader-Imports und verteilbare eigene Ressourcen müssen ohne interne Dokumente verfügbar sein.

Prüfen: Dateiauswahl und History, Secrets/Privatpfade/unerlaubte Daten, Lizenzen, Konfliktmarker, README/Buildanleitung, frischer Build, relevante CPU-/GPU-Tests und vereinbarte Runtime-/Regressionstests. Ein nicht ausgeübter Pfad oder ein fehlender GPU-Adapter ist kein Pass. Fehlende Pflichtumgebung blockiert den Release, statt Tests stillschweigend abzuschalten.

EXE, Renderer-DLL, Default-Content und Laufzeitbibliotheken als zusammengehöriges Paket prüfen. Hashes und öffentliche Source-ID einbetten; keine lokalen Spielarchive, privaten Logs, Agentenregeln, Secrets oder ungeprüften Symbole mitsenden. Eventuell verpflichtende mitzuliefernde Dateien separat berücksichtigen.

Öffentlichen Source-Commit verfügbar machen und vom Ziel erneut überprüfen, bevor Sponsor-Binaries als Release sichtbar werden. Binaries als Release-Anhänge, nicht als Git-Historie. Jeder Binärrelease hat eine exakte Source-ID, Versions-/Kanalangabe, Prüfsummen und nötige Lizenztexte.

Code-, Shader-, Build- oder mitgelieferte Runtime-Ressourcenänderung: erfolgreicher freigegebener Release erzeugt einen neuen passenden Build. Reine öffentliche Textdokumentation: im Freigabebericht SOURCE_ONLY vorschlagen; nicht denselben alten Build mit einer neuen vermeintlichen Source-Herkunft etikettieren.

## 7. Sicherheit, Fehler und Abschluss

Die Publikation übernimmt eine getrennte deterministische Pipeline, nicht eine freie Folge agentengenerierter Push-Kommandos. Normale Entwicklungsagenten erhalten keine Publikationsschlüssel. Buildcode darf keine Public-/Sponsor-Schreibrechte besitzen. Änderungen an Exportpolicy, Freigabemechanik und Workflows gesondert vom Owner prüfen lassen.

Bei einem roten Gate nichts veröffentlichen. Tests nicht lockern, Dateien nicht stillschweigend entfernen und fehlende Funktionen nicht als vollständig getestet beschreiben. Ein Fix verändert den Kandidaten und muss erneut freigegeben werden.

Wenn Source-Push erfolgreich war, Binärupload aber scheitert: als Teilveröffentlichung melden, bisherigen erfolgreichen Download nicht ersetzen und nur denselben verifizierten Kandidaten erneut hochladen. Keine öffentliche Historie zur Vertuschung zurückschreiben.

Je Paket privat dokumentieren: ausgewählte und zurückgehaltene Änderungen, jede ausgelassene Datei mit Grund, geänderte öffentliche Texte, Tests/Skips/Fehler, Source-/Build-Zuordnung und tatsächlicher Veröffentlichungsstatus. Öffentliche Release Notes aus dem bereinigten Diff formulieren, nicht private Commit-Nachrichten übernehmen.

Abschlussmeldung unterscheidet ausdrücklich:
`nur privat`, `wartet auf Freigabe`, `Staging geprüft`, `sauberer Kandidat im noch privaten Ziel`, `öffentlich synchronisiert`, `Binaries veröffentlicht` oder `blockiert/teilweise veröffentlicht`.

Ein vorbereiteter Workflow ist nicht aktiv. Ein Push ist kein Build-Pass. Ein Build-Pass ist keine erfolgreiche Veröffentlichung.

## Autorisierter privater Transfertest vom 2026-10-07

Oliver hat ausdrücklich den bereinigten Transfer nach `koosoli/OpenPoseidon`
zum Testen autorisiert. Dort liegt jetzt ein Kandidat auf einer unabhängigen
`main`-Historie. Diese konkrete Autorisierung erlaubt den privaten Transfertest
bei ehrlich dokumentierten offenen Prüfungen; sie erlaubt keine öffentliche
Freigabe oder Binärveröffentlichung. Keine Review-Gates künstlich auf bestanden
setzen. Details und Pins stehen in `SOURCE-CANDIDATE-TRANSFER-20261007.md`.

Bei weiteren Exporten alle bereinigten Quelldatei-Ersatztexte aus dem neuen Pin
erneuern und ihre Diffs prüfen; alte vollständige Ersatzdateien dürfen neuere
Engine-Änderungen nicht zurücksetzen. Die private Historie niemals in den
Release-Branch mergen oder pushen. Das Ziel bleibt privat.
