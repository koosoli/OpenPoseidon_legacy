@echo off
rem Open Poseidon -- die Reforger-Vergleichsversion aus bin\ -- auf Everon spielen.
rem Soldat am Dorf (5150 / 5385, Blick auf ein Haus mit Tueren und Fenstern; Fluss und Teich in der Naehe).
rem Nutzt Remastered nur als Datenordner; dort wird nichts ersetzt. Eigene Grafik-Einstellungen gelten.
rem Tuer: an die Tuer gehen, Aktionsmenue (Mausrad) "Tuer oeffnen". Fenster: draufschiessen.
setlocal
set "HERE=%~dp0"
set "POSEIDON_REFORGER_WORLD=worlds/eden"
set "POSEIDON_REFORGER_OBJECTS=1"
set "POSEIDON_REFORGER_STREAM=1"
cd /d "%HERE%..\Remastered"
start "" "%HERE%bin\OpenPoseidon.exe" --render=wgpu --test-mission "%HERE%play\everon.eden" --test-world "H:\SteamLibrary\steamapps\common\Arma Reforger\addons" --reforger-world worlds/eden --log-file "%HERE%play\play.log"
