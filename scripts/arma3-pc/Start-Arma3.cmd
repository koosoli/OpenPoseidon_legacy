@echo off
rem Arma 3 mission in Open Poseidon, straight from the Arma 3 folder (nothing is copied).
rem Usage: Start-Arma3.cmd [mission]   e.g. Start-Arma3.cmd showcases\Showcase_Infantry.Stratis
rem Without a mission: Showcase_Infantry. The build is _OpenPoseidon-Arma3\bin (see bin\DEPLOYED-FROM.txt).
set M=%~1
if "%M%"=="" set M=showcases\Showcase_Infantry.Stratis
cd /d "%~dp0..\Remastered"
start "" "%~dp0bin\OpenPoseidon.exe" --arma3 "F:\SteamLibrary\steamapps\common\Arma 3" --arma3-mission "%M%" --log-file "%~dp0play.log"
