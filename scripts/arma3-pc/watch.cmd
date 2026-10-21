@echo off
rem Startet den Arma-3-Testwaechter (laeuft minimiert, wartet auf request.txt).
start "a3watch" /min powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Minimized -File "%~dp0watch.ps1"
