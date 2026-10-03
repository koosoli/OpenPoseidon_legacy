[CmdletBinding()]
param([string]$Compiler='clang++')
$ErrorActionPreference='Stop';$root=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'RainWaterCoarseCapture.ps1')
$output=Join-Path $root 'build/rain-water-coarse-capture';New-Item -ItemType Directory -Force $output|Out-Null
function Refuses([scriptblock]$Body){$failed=$false;try{&$Body|Out-Null}catch{$failed=$true};if(!$failed){throw 'Invalid coarse capture accepted.'}}
$fixture=Join-Path $output 'unit-fixture.rwcap'
$file=[IO.File]::Create($fixture);$writer=[IO.BinaryWriter]::new($file)
try{
 $writer.Write([Text.Encoding]::ASCII.GetBytes('RWCAP001'));$writer.Write([uint32]3);$writer.Write([uint32]3)
 foreach($v in @(25,0,0,-1)){$writer.Write([single]$v)}
 $writer.Write([double]0.125);$writer.Write([uint64]3);$writer.Write([uint64]7)
 foreach($v in @(0,0,0,0)){$writer.Write([double]$v)}
 foreach($v in 1..18){$writer.Write([single]0)}
}finally{$writer.Dispose();$file.Dispose()}
$header=Read-RainWaterCoarseCapture $fixture
$state=@{width=3;height=3;spacing=25;volume=0;rainVolume=0;infiltrationVolume=0;evaporationVolume=0;outletVolume=0;pendingSeconds=.125;generation=3;revision=7;fineActive=$false;sourceReady=$true}
$receipt=@{readonly=$true;sourceCurrent=$true;format='RWCAP001';floatEncoding='ieee754-binary32-le';sourceWitness='bit-exact-current-native-coarse-bed';path=$fixture;world='noe';worldToken='1234';sourceRevision='8';generationExact='3';revisionExact='7';timeMs=100;timeScale=0;width=3;height=3;spacing=25;originX=0;originZ=0;seaLevel=-1;pendingSeconds=.125;headerBytes=88;bytes=160;nativeBedBitsMatched=9;maximumRainMetresPerSecond=.000025;stepSeconds=.25}
Assert-RainWaterCoarseCapture $receipt $header $state $state $fixture 100
foreach($change in @(@{timeScale=1},@{timeMs=101},@{sourceCurrent=$false},@{sourceWitness='renderer-rounded-head'},@{generationExact='4'},@{nativeBedBitsMatched=8},@{maximumRainMetresPerSecond=.00012},@{path=($fixture+'.other')},@{readonly=1})){
 $bad=$receipt.Clone();foreach($key in $change.Keys){$bad[$key]=$change[$key]};Refuses {Assert-RainWaterCoarseCapture $bad $header $state $state $fixture 100}
}
$badState=$state.Clone();$badState.revision=8;Refuses {Assert-RainWaterCoarseCapture $receipt $header $state $badState $fixture 100}
$badState=$state.Clone();$badState.fineActive=$true;Refuses {Assert-RainWaterCoarseCapture $receipt $header $badState $badState $fixture 100}
$bytes=[IO.File]::ReadAllBytes($fixture);[IO.File]::WriteAllBytes($fixture,$bytes[0..86]);Refuses {Read-RainWaterCoarseCapture $fixture}
[IO.File]::WriteAllBytes($fixture,$bytes)
foreach($opt in @('-O0','-O2')){
 $exe=Join-Path $output 'coarse-capture-test.exe'
 & $Compiler '-std=c++20' $opt '-Wall' '-Wextra' '-Werror' ('-I'+(Join-Path $root 'engine')) (Join-Path $root 'tests/unit/engine/weather/test_rain_water_coarse_capture.cpp') '-o' $exe
 if($LASTEXITCODE-ne0){throw 'Exact coarse capture compilation failed.'}
 & $exe;if($LASTEXITCODE-ne0){throw 'Exact coarse capture native check failed.'}
}
& (Join-Path $PSScriptRoot 'Test-RainHelicopterPerformance.ps1') -SelfTest
Write-Output 'Coarse capture binary/header, frozen source receipt and rejection checks PASS; unit fixtures are synthetic, no game/profile/GPU used.'
