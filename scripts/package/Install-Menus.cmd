@echo off
rem Copies the modern menu resources from the free Cold War Assault Remastered Demo into this assets folder.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-Menus.ps1" %*
pause
