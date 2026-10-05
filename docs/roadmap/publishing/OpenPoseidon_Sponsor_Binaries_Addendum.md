# Ergänzung: Sponsor-Zugang zu offiziellen OpenPoseidon-Binaries

**Stand:** 5. Oktober 2026  
**Status:** Ergänzung zum privaten Veröffentlichungsplan. Noch keine Repositories, Organisationen, Sponsoring-Stufen, Einladungen oder Workflows erstellt oder verändert.

## 1. Ziel

Der geprüfte OpenPoseidon-Quellcode wird einschließlich der vollständigen freigegebenen Kompatibilitätsschicht öffentlich und selbst baubar veröffentlicht. Fertig kompilierte offizielle Pakete werden zusätzlich über ein separates privates GitHub-Repository angeboten.

Gewünschte Berechtigung: aktive monatliche GitHub-Sponsorship ab 10 EUR oder mehr. Einmalzahlungen, niedrigere Stufen und beendete Abos sollen keinen fortdauernden Zugang zu neuen offiziellen Downloads erhalten. Bereits erhaltene Software und die GPL-Rechte daran werden durch eine Kündigung nicht widerrufen.

**Währungsentscheidung noch offen:** GitHub definiert Sponsoring-Stufen in USD. 10 USD sind nicht automatisch 10 EUR. Vor Einrichtung einen vom Owner bestätigten USD-Preis und die öffentliche Formulierung festlegen; keine stillschweigende Umrechnung oder täglich schwankende Zugangsgrenze einführen.

## 2. Getrennte Repositories und Grenzen

- Privates Entwicklungsrepository: vollständige Entwicklung einschließlich interner Historie und Arbeitsunterlagen. Kein Sponsorzugang.
- Private Release-Vorbereitung / sauberer Exportkandidat: wie im Hauptplan; keine Veröffentlichung ohne Freigabe.
- Öffentliches Quellenrepository: geprüfter Export mit eigener öffentlicher Historie, Build-Anleitung, Kompatibilitätscode und Lizenznachweisen.
- Privates Binärrepository: vorgeschlagener Name `OpenPoseidon-Builds`, im Besitz einer vom Owner kontrollierten GitHub-Organisation. Konkreten Namespace prüfen und freigeben lassen, nicht als bereits vorhanden behaupten.

Das Quellenrepository darf unter einem persönlichen Konto bleiben. Für GitHubs eingebaute Sponsor-Repository-Kopplung muss das verknüpfte private Binärrepository einer Organisation gehören und der gesponserte Maintainer dort Adminzugriff haben. Das persönliche Sponsoring-Profil kann hierfür weiter verwendet werden.

Im Binärrepository keine Kopie der privaten Entwicklung, Roadmaps, Agentenanweisungen, privaten Commits oder unbereinigten Build-Artefakte ablegen. Ein privates Sponsor-Repository ist für seine berechtigten Kunden sichtbar und keine interne Geheimablage.

## 3. Zugangsverwaltung

Vorrangig GitHubs eingebaute Funktion „Grant sponsors access to a private repository“ verwenden. Das Binärrepository jeder berechtigten monatlichen Stufe zuordnen, auch sämtlichen höheren Stufen. Nicht darauf vertrauen, dass die Formulierung „ab 10 Euro“ in einer Beschreibung allein Rechte erteilt.

GitHub dokumentiert automatische Einladungen an neue Sponsoren und Entfernung des Zugangs bei Kündigung. Vor Freigabe kontrollieren:

- Einladung, Annahme und tatsächlich nur benötigter Lese-/Download-Zugang;
- Monatsabo unterhalb/auf/oberhalb der gewählten Schwelle;
- Wechsel auf eine niedrigere oder höhere Stufe und Kündigung;
- bestehende Sponsoren sowie auslaufende Altstufen;
- freie individuelle Beträge: nicht ungeprüft als einer berechtigten Stufe zugeordnet behandeln;
- keine automatische Freigabe durch Einmalzahlung;
- manuelle Einladungen: GitHub überschreibt diese laut Dokumentation nicht durch den Sponsor-Sync; solche Sonderrechte separat dokumentieren und verwalten;
- Organisations-Sponsoren: die eingebaute Funktion lädt persönliche Konten ein, keine Organisationskonten. Ein mögliches Firmenangebot separat klären.

Keine pauschale Mitgliedschaft mit Zugriff auf alle privaten Projekt-Repositories vergeben. Keine Schreibrechte oder Release-Verwaltung für Download-Nutzer vorsehen.

Für eine reine Download-Organisation zunächst GitHub Free prüfen: Auf kostenpflichtigen Organisationsplänen können externe Kollaboratoren in privaten Repositories jeweils einen bezahlten Sitz verbrauchen. Keine Plan-Upgrades ohne Kostenprüfung.

## 4. Pakete als Releases, nicht als Git-Historie

Im Repository selbst nur eine kurze README, Installations-/Supporthinweise, Release-Metadaten und notwendige Lizenzinformationen halten. EXE/DLL/ZIP-Dateien als **GitHub Release Assets** bereitstellen, nicht jede Version als Binärdatei in Git committen.

Jedes freigegebene Paket enthält bzw. benennt:

- Version, Plattform und genaue öffentliche Source-Commit-ID;
- zusammengehörige Engine-/Renderer-Binaries und rechtmäßig verteilbare Laufzeitabhängigkeiten;
- SHA-256-Prüfsummen;
- Lizenztexte und Drittanbieter-Hinweise;
- klare Build-/Source-Zuordnung, beispielsweise eine `SOURCE.txt`;
- unmittelbar beim Download einen Link zum vollständigen passenden öffentlichen Source-Stand und dessen Downloadmöglichkeit.

Keine fremden Originalspielarchive, Modelle, Texturen, Audio oder nicht freigegebenen SDK-Binaries beilegen. Enthaltene eigene oder lizenzierte Ressourcen bleiben Bestandteil der Rechte-/Allowlist-Prüfung.

## 5. Verbindliche Source-/Binary-Reihenfolge

1. Privaten Quellstand festlegen und gemäß Allowlist prüfen/exportieren.
2. Bereinigten Veröffentlichungskandidaten ohne private Elterncommits erstellen.
3. Genau diesen Kandidaten in einer frischen Umgebung bauen und testen.
4. Vollständigen zugehörigen Source-Stand öffentlich verfügbar machen.
5. Erst danach die zugehörigen geprüften Binärpakete im privaten Sponsor-Repository veröffentlichen.

Kein neuer Sponsor-Build aus einem internen privaten Commit, dessen benötigte Änderungen im veröffentlichten Source fehlen. Die öffentliche Hauptbranch allein ist kein ausreichender Source-Verweis; der passende unveränderliche Release-Stand muss verfügbar sein.

Automatisch erzeugte ZIP/TAR-Links eines reinen Binärrepositories enthalten dessen Metadaten, nicht automatisch den Engine-Quellcode. Diese Links nicht als GPL-Source-Angebot ausgeben.

Die Build-/Upload-Automatisierung erhält nur die nötigen Rechte. Keine privaten Upload-Tokens an ungeprüfte Fork-PRs oder fremden Buildcode geben. Offizielle Pakete nicht versehentlich gleichzeitig als öffentliche CI-Artefakte veröffentlichen. Der öffentliche Selbstbau bleibt ausdrücklich möglich und gewollt.

## 6. Lizenz- und Produktversprechen

GPL-Software darf gegen Entgelt verteilt werden. Der Vorteil ist der Zugang zu **offiziellen vorgebauten Paketen und zukünftigen offiziellen Downloads**, nicht eine exklusive Nutzungslizenz.

Empfänger behalten ihre GPL-Rechte, einschließlich Weitergabe unter den geltenden Bedingungen. Kein Weitergabeverbot, NDA für die GPL-Binaries, Abo-Zwang zur weiteren Nutzung bereits erhaltener Versionen oder Behauptung, nur zahlende Sponsoren dürften die Engine benutzen.

Bei jedem Binärdownload vollständigen passenden Corresponding Source ohne zusätzliche Gebühr bereitstellen. GPLv3 Abschnitt 6(d) erlaubt hierfür auch einen anderen Server, wenn direkt bei den Binaries klare Hinweise stehen und der Anbieter die Verfügbarkeit sicherstellt. Der geplante öffentliche Source-Release ist hierfür eine geeignete technische Grundlage; weitere enthaltene Lizenzen und die BI-Zusatzbedingungen bleiben gesondert einzuhalten.

Vorgeschlagene sachliche Beschreibung:

> OpenPoseidon bleibt quelloffen und kann kostenlos selbst gebaut werden. Unterstützer der berechtigten monatlichen Sponsoring-Stufe erhalten Zugang zu unseren offiziellen vorkompilierten Paketen. Das Sponsoring unterstützt Entwicklung, Tests und die Bereitstellung dieser Builds. Die Rechte aus der Softwarelizenz bleiben unverändert.

Den konkreten Preis erst nach der USD-/EUR-Entscheidung einsetzen. Keine unbestätigten Zusagen zu garantierten Release-Intervallen, Support-SLAs oder Exklusivrechten hinzufügen.

## 7. Private Abnahme und Freigabe

Zum privaten Releasebericht ergänzen:

- endgültiges Source- und Binär-Zielrepository;
- USD-Stufe und zugehörige öffentliche Preisformulierung;
- Liste der berechtigten wiederkehrenden Stufen;
- Ergebnis der Zugangs-, Stufenwechsel- und Kündigungstests;
- verbleibende manuelle Sonderrechte;
- Kostenprüfung der Organisation;
- Source-/Binary-Mapping, Paketdateien, Hashes und Lizenzen;
- Nachweis, dass der Sponsorzugang keine interne Entwicklung offenlegt;
- Freigabe des Owners für die konkrete Einrichtung und den ersten Release.

Die Entscheidung für dieses Modell ist keine Erlaubnis, jetzt schon Repositories öffentlich zu schalten, Sponsoring-Stufen zu publizieren oder Zahlungs-/Zugangsänderungen durchzuführen.

## Offizielle Referenzen

- GitHub Sponsors: Managing your sponsorship tiers — `https://docs.github.com/en/sponsors/receiving-sponsorships-through-github-sponsors/managing-your-sponsorship-tiers`
- GitHub Sponsors: Setting up for your personal account — `https://docs.github.com/en/sponsors/receiving-sponsorships-through-github-sponsors/setting-up-github-sponsors-for-your-personal-account`
- GitHub: About releases — `https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases`
- GitHub: Adding outside collaborators — `https://docs.github.com/en/organizations/managing-user-access-to-your-organizations-repositories/managing-outside-collaborators/adding-outside-collaborators-to-repositories-in-your-organization`
- GNU GPLv3, insbesondere Abschnitte 1, 6(d) und 10 — `https://www.gnu.org/licenses/gpl-3.0.html`
- GNU: Selling Free Software — `https://www.gnu.org/philosophy/selling.html`
