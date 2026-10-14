@echo off
rem Open Poseidon Arma-3-Test: Ablauf steht in job.txt
start "a3test" /min powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Minimized -File "%~dp0go.ps1"
