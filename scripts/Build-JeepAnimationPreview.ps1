param([string]$Python='python')
$ErrorActionPreference='Stop'
& "$PSScriptRoot/Build-VehicleActions.ps1" -Python $Python -Preview
