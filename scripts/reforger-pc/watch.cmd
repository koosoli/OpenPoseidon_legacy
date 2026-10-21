@echo off
rem Open Poseidon Reforger-Vergleich: Watcher starten (einmal doppelklicken, Fenster minimiert lassen)
start "rfwatch" /min powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Minimized -File "%~dp0watch.ps1"
