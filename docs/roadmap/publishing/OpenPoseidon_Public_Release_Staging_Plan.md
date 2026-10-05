# OpenPoseidon – privater Staging- und Veröffentlichungsplan

**Stand:** 5. Oktober 2026  
**Status:** Vorschlag für die ausführenden Agenten. Noch kein Export, kein angelegter Branch, kein neues GitHub-Repository und keine Veröffentlichung.  
**Ziel:** Ein nachvollziehbar geprüfter öffentlicher Quellcode-Stand einschließlich der vollständigen vorhandenen Kompatibilitätsschicht, ohne private Entwicklungshistorie und ohne interne Arbeitsunterlagen.

## 1. Verbindliche Ausgangspunkte

- Quellrepository: `koosoli/PoseidonWGPU-private-safety-mirror`.
- Vom Owner benannter Quellbranch: `codex/kolgujev-fog-corpse-fixes-20261004`.
- Bei der lesenden Prüfung aufgelöster HEAD: `9f108ec7eb753e875fc308bea64b4ba99e5186f3`.
- Zugehöriger Git-Tree: `d6ab95b0a8ed3dc139c7aed049de324f74215278`.
- Beabsichtigte öffentliche Projektadresse: `koosoli/OpenPoseidon`.
- Diese Adresse bezeichnet derzeit ein vorhandenes öffentliches Fork-Repository. GitHub meldet einen Child-Fork. Ein neuer eigenständiger Repository-Start ist deshalb eine mögliche, aber noch separat freizugebende Alternative.
- Die Kompatibilitätsschicht soll vollständig mitgegeben werden. Ältere Pläne, die entsprechende Implementierungen aus dem öffentlichen Export ausschließen, sind für diesen Auftrag überholt.
- Interne Agenten-Anweisungen, private Roadmaps, Arbeitsprotokolle, Übergabeprompts und private Entwicklungscommits sollen nicht veröffentlicht werden.
- Erforderliche Lizenztexte, Copyright-Hinweise, Drittanbieter-Nachweise und vollständige zum veröffentlichten Produkt gehörende Quellen bleiben erhalten.

Vor Beginn den Branch erneut auflösen. Hat sich der HEAD geändert, die Differenz melden und einen neuen Kandidaten ausdrücklich festlegen. Nicht stillschweigend dem beweglichen HEAD folgen. Der neueste Commit ist nicht automatisch der geeignete Release-Kandidat.

## 2. Berechtigungsgrenze

Zunächst nur privat vorbereiten und prüfen. Insbesondere noch nicht:

- auf das öffentliche Repository pushen oder dort einen PR mit privaten Entwicklungscommits öffnen;
- ein Repository öffentlich schalten, umbenennen, löschen, archivieren oder aus einem Fork-Netz lösen;
- fremde Forks verändern;
- Branches oder Historie mit Force-Push überschreiben;
- das private Entwicklungsrepository zum öffentlichen Repository machen;
- experimentelle Funktionen aktivieren, Kompatibilitätscode entfernen oder weitreichende Refactorings als angebliches Release-Cleanup durchführen.

Die spätere Veröffentlichung braucht eine ausdrückliche Freigabe für **Zielrepository, Zielbranch, exakten Export-Commit und gegebenenfalls Release-Artefakte**.

## 3. Zwei getrennte Staging-Ebenen

### A. Private Vorbereitung mit erhaltener Historie

Vorgeschlagener Branch im privaten Repository: `release/public-prep-20261005`.

Er basiert auf dem bewusst festgelegten Quellcommit. Dort dürfen README, öffentliche Dokumentation, Exportwerkzeug, Prüfungen und notwendige Packaging-Anpassungen bearbeitet werden. Der ursprüngliche Entwicklungsbranch bleibt unverändert. Interne Nachweise und die private Historie bleiben erhalten.

**Dieser Branch selbst ist kein öffentlich pushbarer Branch.** Er enthält private Vorfahren, auch nachdem interne Dateien in einem späteren Commit gelöscht wurden.

### B. Eigenständiger Veröffentlichungskandidat ohne private Historie

Aus Ebene A werden ausschließlich ausdrücklich freigegebene Dateien in ein leeres, außerhalb des privaten Worktrees liegendes Verzeichnis exportiert. Keine `.git`-Datei, kein `.git`-Verzeichnis, keine Worktree-Verweise, Reflogs, Git-Notes, alten Tags oder Git-Objekte kopieren.

Erst im sauberen Exportverzeichnis ein eigenständiges Git-Repository initialisieren. Darin einen Root-Commit erstellen, beispielsweise mit dem sachlichen Titel `Initial public source snapshot`. Dieser Commit erhält **keinen privaten Elterncommit**. Ein solcher Snapshot ist kein Anspruch, sämtlichen enthaltenen Code selbst geschrieben zu haben; die tatsächliche Herkunft bleibt dokumentiert.

Ein mögliches unabhängiges GitHub-Staging-Repository ist `OpenPoseidon-release-staging`, zunächst **privat** und über „New repository“, nicht über „Fork“ angelegt. Repository-Erstellung erst nach entsprechender Freigabe. Der gesamte Kandidat muss bereits vor einem späteren Sichtbarkeitswechsel geprüft sein; auch seine frühere Staging-Historie darf dann keine privaten Dateien enthalten.

Zukunftsregel: Weitere private Fortschritte ebenfalls als geprüfte Datei-Updates exportieren. Niemals später die private Entwicklungshistorie in die neue öffentliche Geschichte mergen.

## 4. Umgang mit dem bestehenden öffentlichen Fork

Zwei getrennte Ziele unterscheiden:

1. **Nur private Entwicklungscommits nicht veröffentlichen:** Ein neuer geprüfter Snapshot-Commit auf Basis des vorhandenen öffentlichen Branches reicht. Die alte öffentliche Historie bleibt bestehen, ohne dass private Elterncommits importiert werden. Der Fork-Status verhindert diesen Workflow nicht.
2. **Keine alte Historie und kein Fork-Netz:** Ein neues eigenständiges Repository aus Ebene B verwenden.

Empfehlung für einen vollständigen Neustart: Das vorhandene öffentliche Repository behalten und später als Altstand mit einem Verweis auf den Nachfolger archivieren, nicht löschen. Zunächst einen anderen Namen für das neue Repository verwenden. So bleiben alte Issues, Forks und Links nachvollziehbar.

Soll die neue unabhängige Veröffentlichung unbedingt wieder `koosoli/OpenPoseidon` heißen, ist eine gesonderte Migration nötig: altes Repository etwa nach `OpenPoseidon-legacy` umbenennen, neues Repository an der alten Adresse erstellen und die Nutzer informieren. **Achtung:** Die Wiederbelegung des alten Namens beendet dessen automatische Weiterleitung zum umbenannten Repository. Alte Clone-/Push-Adressen können dann auf das neue Projekt zeigen. Das ist keine nebenbei auszuführende Umbenennung.

Stars, Issues, PRs und Child-Forks gehen nicht automatisch auf ein neu erstelltes Repository über. Bereits veröffentlichte Inhalte verschwinden durch einen Neustart nicht aus alten Forks, lokalen Klonen oder archivierten Ansichten. Bei einem tatsächlich offengelegten Geheimnis ist zunächst dessen Sperrung/Rotation erforderlich.

## 5. Allowlist statt Gitignore als Veröffentlichungsgrenze

Eine normale `.gitignore` für temporäre Build-Dateien bleibt sinnvoll. Sie ist aber **nicht** die Export-Policy und bereinigt keine bereits versionierte Historie.

Ein privates Exportwerkzeug mit zwei ausdrücklich getrennten Betriebsarten erstellen:

- **Inventory/Dry-run:** erfasst und klassifiziert alle versionierten Einträge des festgelegten Git-Trees; verändert kein öffentliches Repository.
- **Export:** schreibt ausschließlich die geprüfte Allowlist in ein neues leeres Zielverzeichnis und erzeugt einen unabhängigen Prüfbericht.

Keine pauschale Freigabe allein wegen einer Endung wie `.cpp`, `.h`, `.rs`, `.wgsl` oder `.md`. Auch Quellcodedateien können eingebettete fremde Daten, private Pfade oder Anweisungen enthalten. Ebenso ist nicht jede Binärdatei unzulässig: eigene Icons oder rechtmäßig verteilbare Ressourcen können benötigt werden.

Verzeichnisregeln dürfen eine Kandidatenliste erzeugen. Die finale Freigabe wird jedoch zu einem **expliziten Dateimanifest mit erwarteten Inhaltshashes** aufgelöst. Neue Pfade oder geänderte Inhalte sind nicht automatisch genehmigt. Unklassifizierte Dateien blockieren den finalen Export.

### Vorgeschlagene Manifest-Felder

`source_path`, `source_blob`, `source_sha256`, `git_mode`, `classification`, `destination_path`, `export_sha256`, `rule_id`, `reason`, `license_review`, `build_dependency`, `review_state`.

Zulässige Klassifikationen:

- `INCLUDE_UNCHANGED`: unverändert übernehmen;
- `INCLUDE_REWRITTEN`: bewusst redaktionell/technisch angepasst übernehmen, mit dokumentiertem Diff;
- `EXCLUDE_INTERNAL`: interne Arbeitsunterlage;
- `EXCLUDE_GENERATED`: regenerierbares Build-/Messartefakt;
- `EXCLUDE_EXTERNAL_DATA`: nicht zur Verteilung freigegebene externe Spieldaten;
- `REVIEW_REQUIRED`: noch unklar, blockiert eine vollständige Freigabe;
- `BLOCKED_LICENSE` oder `BLOCKED_SECRET`: ausdrücklicher Veröffentlichungsblocker.

Der Export muss Symlinks, Submodule, LFS-Zeiger, ausführbare Dateimodi, Pfade außerhalb des Ziels und Groß-/Kleinschreibungs-Kollisionen unter Windows gesondert behandeln. Symlinks nicht blind dereferenzieren. Keine private Submodule-URL oder nicht verfügbare LFS-Datei als vermeintlich vollständige Quelle veröffentlichen.

## 6. Was grundsätzlich mitkommen soll

Die folgenden Bereiche sind **zu prüfende Kandidaten**, keine bereits freigegebene vollständige Allowlist:

- vollständiger Engine- und Renderer-Code, Anwendungen und unterstützte Server-Werkzeuge;
- die gesamte vorhandene Kompatibilitätsschicht einschließlich Readern, Konvertern, Materialübersetzung, Datenauflösung, Build-Verknüpfung und benötigten eigenen Ressourcen;
- CMake/Cargo/vcpkg-Konfiguration, festgelegte Abhängigkeitsversionen, notwendige Patches, Build- und Installationswerkzeuge;
- verteilbarer Drittcode einschließlich seiner vollständigen Lizenz- und Notice-Dateien;
- sinnvolle öffentliche Unit-/GPU-/Regressionstests und selbst erzeugte, rechtmäßig verteilbare Testdaten;
- kurze öffentliche Build-, Nutzungs-, Kompatibilitäts- und Beitragsdokumentation;
- geprüfte `.github`-Workflows und Issue-Vorlagen ohne privaten Infrastrukturbezug;
- geprüfte eigene Icons, Logos, Bilder und sonstige notwendige Ressourcen mit klarer Lizenz.

Keine Engine-Funktion nur deshalb entfernen, weil ihre Entstehung intern diskutiert wurde. Keine unbeabsichtigten Änderungen an Default-Flags, Physik, Rendering oder unterstützten Datenformaten durch das Packaging. Ausnahmen müssen separat erklärt und freigegeben werden.

„Kompatibilitätscode vollständig enthalten“ bedeutet ausdrücklich **nicht** „alle Spiele vollständig spielbar“ und ebenfalls nicht „deren Originalinhalte werden verteilt“. Der öffentliche Supportstatus muss getestete Funktionen, experimentelle Funktionen und bekannte Grenzen auseinanderhalten.

## 7. Was privat bleiben soll

Beim geprüften Root-Tree tatsächlich vorhanden und zunächst auszuschließen:

- `.agents/`;
- `CLAUDE.md`;
- `HANDOVER-PARITY-NEXT-AI.md` und `HANDOVER-PROMPT.md`;
- `.tmp-movecam/` und `.tmp-shadow/`;
- interne Roadmaps, Status-Ledger, Arbeitsentscheidungen, Forschungsprotokolle und rohe Verifikations-/Capture-Daten unter den jeweiligen `docs/`-Bereichen.

Zusätzlich rekursiv suchen und bei Vorkommen klassifizieren: `.agent/`, `.claude/`, `AGENTS.md`, `CLAUDE.local.md`, Agentenlogs, private Editor-/MCP-Einstellungen, Zugangsdaten, lokale Spielpfade, private Korrespondenz, Debug-Dumps und lokale SDKs.

`experiments/`, `dev-missions/`, `screenshots/`, `content/`, `tools/` und `scripts/` nicht pauschal freigeben oder löschen. Hier können sowohl notwendige, verteilbare Bestandteile als auch private Experimente oder fremde Originaldaten liegen. Insbesondere Test-P3Ds, Texturen, Archive, extrahierte Tabellen und in Source eingebettete Daten auf tatsächliche Herkunft prüfen.

Öffentliche Tests mit externen Originaldaten müssen den Nutzer einen eigenen Datenpfad angeben lassen. Geheimen Privatbestand nicht zur stillen Build-Voraussetzung machen. Gezielte Skips klar als Skips ausweisen; keine Tests deaktivieren, nur um den Export grün erscheinen zu lassen.

## 8. PROVENANCE.md und Lizenznachweise

Die aktuell gelesene `PROVENANCE.md` behandelt nur Arma-3-Kompatibilität und enthält detaillierte Aussagen über den Entwicklungsprozess. Sie ist **nicht automatisch geeignet**, den gesamten heutigen Release zu beschreiben.

Empfehlung: Das bestehende Dokument im privaten Archiv erhalten. Für den öffentlichen Export nicht unverändert übernehmen. Eine eigenständige Datei mit diesem Namen ist nach den hier geprüften GPL-/BI-Texten nicht als Pflicht vorgeschrieben; die inhaltlichen Pflichten bleiben davon unberührt.

Öffentlich bleiben insbesondere:

- die vollständige `LICENSE` einschließlich der BI-Zusatzbedingungen;
- unverfälschte Herkunft und Kennzeichnung als modifizierte Community-Version;
- erforderliche Copyright- und Drittanbieter-Lizenzhinweise;
- gegebenenfalls erforderliche Änderungs-/Datumsangaben;
- die vollständigen Quellen, Build-/Installationsskripte und sonstigen Voraussetzungen, die für veröffentlichte Binärdateien unter den jeweils geltenden Lizenzen benötigt werden.

Die README braucht keine lange Rechtfertigung und keine Spekulation über Motive, Tools oder rechtliche Sicherheit. Sie soll knapp Herkunft, Lizenz, Unabhängigkeit und Datenvoraussetzungen erklären. Keine ungeprüften Absolutaussagen wie „vollständig clean-room“, „niemals reverse engineered“ oder „vollständig rechtlich freigegeben“ hinzufügen.

**Konkreter Fund:** `CREDITS.md` enthält nicht nur Danksagungen, sondern auch Port-Zuordnungen und den tatsächlichen Tidewater-MIT-Lizenztext. Diese Inhalte nicht zusammen mit internen Roadmap-Verweisen entfernen. Drittanbieter-Hinweise gegebenenfalls in eine gepflegte `THIRD_PARTY_NOTICES.md` umziehen und Links entsprechend aktualisieren.

**Weiterer Prüfpunkt:** Bei der Malprave-AI-Herkunft beschreibt die aktuelle Credits-Datei eine bislang nicht unabhängig nachgeprüfte vorgelagerte Quellenkette. Das ist kein Beweis einer Rechtsverletzung, aber ein offener Herkunftsprüfpunkt. Vor der Freigabe intern klären, nicht durch Löschen des Hinweises als erledigt behandeln. Bei unauflösbaren Rechten Veröffentlichung der betroffenen Teile blockieren und dem Owner konkrete Optionen vorlegen.

Die bisherige `THIRD_PARTY_NOTICES.md` enthält außerdem eine veraltete pauschale Aussage, Rust-Komponenten seien nur eigenständige Tools und nicht in die C++-Engine eingebunden. Für den heutigen Renderer und tatsächlich gelieferte Abhängigkeiten muss diese Bestandsaufnahme neu stimmen. SDKs, dynamische Bibliotheken und deren Verteilungsbedingungen separat prüfen; ein erfolgreiches Linking ist kein Lizenznachweis.

## 9. README neu ausrichten

Eine kompakte, nutzerorientierte README unter dem einheitlichen öffentlichen Projektnamen **OpenPoseidon** schreiben. Alte private Repository-Links und abgelöste Namen nicht blind übernehmen.

Vorgeschlagener Aufbau:

1. Was das Projekt ist; Ursprung in Poseidon/CWR-CE und unabhängiger Community-Fork.
2. Tatsächlich verfügbare Hauptfunktionen, keine internen Prozentwerte oder Roadmap-Versprechen.
3. Kompatibilität: was für welche Datenfamilie implementiert, getestet, experimentell oder unvollständig ist.
4. Build-Voraussetzungen einschließlich C++- und Rust-Toolchain; verifizierte Plattformen von nicht aktuell getesteten trennen.
5. Ein vollständiger frischer Build und Start mit eigenem lokalem Spielbestand.
6. Wichtige Einschränkungen und bekannte Probleme des tatsächlich exportierten Stands.
7. Lizenz, Herkunft, Credits, externe Spieldaten, Beiträge und bestehende freiwillige Unterstützungslinks.

**Bereits sichtbare Überarbeitungspunkte:** Die derzeitige README heißt noch `PoseidonWGPU`, enthält doppelte Windows-Installationsabsätze und macht pauschale APL-SA-Aussagen über Spielinhalte. Solche Aussagen nicht auf Arma/DayZ/Reforger und andere Datenbestände verallgemeinern. Für externe Spieldaten gelten deren jeweilige Bedingungen; die Engine-Lizenz ist keine pauschale Lizenz für alle kompatiblen Inhalte.

Build-Anleitungen bei Bedarf als kuratierte `BUILDING.md` und Kompatibilitätsdetails als `COMPATIBILITY.md` im Root veröffentlichen. So kann der interne `docs/`-Baum privat bleiben, ohne Nutzer ohne Anleitung zurückzulassen. Links aus README, Credits, Tests und Buildsystem anschließend vollständig prüfen.

## 10. Bestehendes Packaging nicht ungeprüft übernehmen

`scripts/package_preview0.ps1` ruft derzeit private Ledger-Validatoren und den Capability-Matrix-Generator auf. `docs/release/preview0/RELEASING.md` baut auf Nachweisen unter `docs/roadmap/evidence/` auf.

Damit ist „docs entfernen, alle scripts behalten“ **kein bewiesener funktionierender Export**.

Die brauchbaren Sicherheitskontrollen – festgelegter Commit, sauberer Baum, Inhaltsprüfung von EXE/DLL und Paketen, echte WGPU-Auswahl – erhalten. Interne Ledger-Abhängigkeiten durch eine schmale öffentliche Release-Konfiguration und reproduzierbare Build-/Testkontrollen ersetzen. Dies ist ein überprüfter Packaging-Umbau, kein Entfernen von Tests zur Schönung des Ergebnisses.

Externe Abhängigkeiten, Shader-Imports, Codegeneratoren und ggf. Default-Content-Pakete müssen im bereinigten Baum vollständig auflösbar sein. Neue Binärpakete ausschließlich aus dem **öffentlichen Kandidaten-Commit** bauen, nicht alte private `dist/`-Dateien kopieren. Bei identisch klingenden EXE/DLL/PBO-Namen reicht Zeitstempelgleichheit nicht; Inhalte und Buildherkunft prüfen.

## 11. Private Berichte – vor Veröffentlichung vorzulegen

Folgende Ergebnisse erzeugen, aber nicht in den öffentlichen Kandidaten aufnehmen:

### PUBLIC-RELEASE-AUDIT.md

Lesbare Zusammenfassung des Umfangs, der Exportentscheidung, der README-/Lizenzänderungen, der Build-/Runtime-Ergebnisse und sämtlicher offener Blocker. Keine erfundenen Passes oder geschätzten Dateimengen.

### public-export-inventory.json / .csv

Eine Zeile pro versioniertem Eintrag mit den Manifest-Feldern aus Abschnitt 5. Alle Quellpfade müssen zugeordnet sein. Exportierte neue Dateien und umgeschriebene Dateien ebenfalls erfassen.

### public-export-exclusions.csv

Für jede ausgeschlossene Datei: genauer Pfad, Kategorie, konkrete Begründung, Build-/Lizenzrelevanz und gegebenenfalls Ersatzdatei. Nicht nur Sammeltext „docs removed“. Gruppen dürfen in der Zusammenfassung verdichtet werden; der maschinenlesbare Bericht bleibt vollständig.

### public-export-changes.diff

Alle Änderungen an übernommenen Dateien: Dokumentation, Packaging, Pfadbereinigung und eventuell ausdrücklich freigegebene Codeänderungen. Separat zeigen, welche Runtime-Dateien unverändert übernommen wurden.

### public-release-build-report.md

Frischer Configure-/Buildlauf, Tests mit Pass/Fail/Skip, getestete Plattformen, benötigte externe Originaldaten, Smoke-Tests und bekannte Unterschiede. Nicht vorhandene Windows-/GPU-/Spieldatenzugänge ehrlich als fehlende Abnahme melden.

Die private Zuordnung lautet: **privater Quell-SHA → Prep-SHA → Allowlist-Hash → Export-Tree → öffentlicher Kandidaten-SHA → Binärhashes**. Das erhält Rückverfolgbarkeit ohne Veröffentlichung interner Entwicklungskommentare.

## 12. Abnahmekriterien

Freigabe erst, wenn:

- jede versionierte Quelldatei eine dokumentierte Entscheidung hat und alle Blocker aufgelöst sind;
- der Export genau der freigegebenen Allowlist entspricht;
- keine privaten Commit-Vorfahren, Tags, Notes oder fremden Git-Objekte im neuen Repository liegen;
- Lizenzen, Copyrights, Änderungen, Credits und benötigte Buildquellen vollständig erhalten sind;
- die ganze beabsichtigte Kompatibilitätsschicht im Export vorhanden und baubar ist;
- Secrets-/Privatpfad-/Konfliktmarker-/eingebettete-Daten-Prüfung durchgelaufen ist;
- der bereinigte Kandidat in einem frischen Checkout ohne private Roadmaps oder lokale Caches konfiguriert und gebaut wurde;
- öffentliche Tests und gezielte Smoke-Tests die entfernten Dokument-/Packaging-Abhängigkeiten abdecken;
- die README nur belegte Eigenschaften und klar bezeichnete Grenzen verspricht;
- Release-Binaries, falls vorgesehen, zum finalen öffentlichen Source-Stand passen;
- der Owner den exakten Bericht und Zielstand ausdrücklich freigegeben hat.

Bei jeder Änderung am Kandidaten prüfen, welche bisherigen Nachweise ungültig werden. Insbesondere README-Featureversprechen und lizenzrelevante Änderungen nicht automatisch aus einem früheren Build ableiten.

## 13. Spätere Veröffentlichung

Erst nach Freigabe das konkrete öffentliche Ziel verifizieren. Bei neuem eigenständigem Repository muss `fork=false` sein. Nur den explizit freigegebenen Kandidatenbranch und ausdrücklich freigegebene Release-Tags übertragen. **Kein `push --mirror`, kein pauschales `--all`, kein automatisches Mitnehmen aller Tags.**

Beim bestehenden öffentlichen Repository ohne Neustart muss der neue Release-Commit ausschließlich auf freigegebener öffentlicher Historie aufbauen. Beim neuen Repository startet die Historie mit dem geprüften Root-Commit. Diese Alternativen nicht mischen.

Anschließend einen neuen Clone vom Ziel erstellen, Commit-/Tree-/Dateihashes prüfen und den öffentlichen Buildweg erneut testen. Der erste öffentliche Release ist kein Anlass, den privaten Prep-Branch später nachträglich zu mergen.

## 14. Grenzen dieses vorbereitenden Reviews

Gelesen wurden der benannte Branch-HEAD, der Root-Tree, Teile der Unterverzeichnisse sowie README, PROVENANCE, LICENSE, CREDITS, Teile der Third-Party-Notices, CMake und Release-Anleitung/-Skript. Das ist eine konkrete Ausgangsprüfung, **kein vollständiger Datei-für-Datei-Code-, Secrets- oder Lizenz-Audit**.

Der endgültige Ausschlussbericht kann erst nach vollständiger Inventur und realem Export entstehen. Dieses Dokument nennt bewusst keine erfundenen ausgeschlossenen Dateizahlen und behauptet keine bereits erfolgte Bereinigung.

## Quellen für die Ausführung

Repository-Belege beziehen sich auf den oben genannten unveränderlichen Quell-SHA:
`README.md`, `PROVENANCE.md`, `LICENSE`, `CREDITS.md`, `THIRD_PARTY_NOTICES.md`, `CMakeLists.txt`, `scripts/package_preview0.ps1`, `docs/release/preview0/RELEASING.md`.

Offizielle Referenzen, am 5. Oktober 2026 eingesehen:

- GitHub: Detaching a fork — `https://docs.github.com/en/pull-requests/how-tos/work-with-forks/detaching-a-fork`
- GitHub: Renaming a repository — `https://docs.github.com/en/repositories/creating-and-managing-repositories/renaming-a-repository`
- GitHub: Removing sensitive data — `https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/removing-sensitive-data-from-a-repository`
- Git: gitignore — `https://git-scm.com/docs/gitignore/`
- GNU: GPLv3, besonders Abschnitte 1, 4, 5, 6 und 7 — `https://www.gnu.org/licenses/gpl`
- GNU: GPL FAQ, Corresponding Source / Distribution — `https://www.gnu.org/licenses/gpl-faq.html`

Die individuelle Rechteprüfung der gesamten Kompatibilitäts- und Drittcodekette wird durch diesen technischen Veröffentlichungsplan nicht ersetzt.
