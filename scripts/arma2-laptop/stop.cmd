@echo off
echo stop> "%~dp0STOP"
taskkill /IM OpenPoseidon.exe /F
