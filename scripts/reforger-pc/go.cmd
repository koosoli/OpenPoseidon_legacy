@echo off
rem Einen Lauf mit job.txt starten
start "rftest" /min powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Minimized -File "%~dp0go.ps1"
