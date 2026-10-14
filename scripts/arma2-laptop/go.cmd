@echo off
rem Open Poseidon Arma-2-Test (Laptop): Ablauf steht in job.txt
start "a2test" /min powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Minimized -File "%~dp0go.ps1"
