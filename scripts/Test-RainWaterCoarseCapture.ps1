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
 # Actual failed campaign value: its cJSON decimal parses one ULP above this.
 $writer.Write([BitConverter]::Int64BitsToDouble([Convert]::ToInt64('3FC555737C000000',16)));$writer.Write([uint64]3);$writer.Write([uint64]7)
 foreach($v in @(.1,.2,.3,.4)){$writer.Write([double]$v)}
 foreach($v in 1..18){$writer.Write([single]0)}
}finally{$writer.Dispose();$file.Dispose()}
$header=Read-RainWaterCoarseCapture $fixture
$state=@{width=3;height=3;spacing=25;volume=0;rainVolume=.1;infiltrationVolume=.2;evaporationVolume=.3;outletVolume=.4;pendingSeconds=.166670260950923;generation=3;revision=7;fineActive=$false;sourceReady=$true}
$receipt=@{readonly=$true;sourceCurrent=$true;format='RWCAP001';floatEncoding='ieee754-binary32-le';sourceWitness='bit-exact-current-native-coarse-bed';path=$fixture;world='noe';worldToken='1234';sourceRevision='8';generationExact='3';revisionExact='7';timeMs=100;timeScale=0;width=3;height=3;spacing=25;originX=0;originZ=0;seaLevel=-1;pendingSeconds=.166670260950923;headerBytes=88;bytes=160;nativeBedBitsMatched=9;maximumRainMetresPerSecond=.000025;stepSeconds=.25}
if([BitConverter]::DoubleToInt64Bits($state.pendingSeconds)-[BitConverter]::DoubleToInt64Bits($header.pendingSeconds)-ne1){throw 'Observed decimal precision regression fixture is not one ULP.'}
foreach($key in @('pendingSeconds','rainVolume','infiltrationVolume','evaporationVolume','outletVolume')){
 $bits=[BitConverter]::DoubleToInt64Bits([double]$header[$key]).ToString('X16',[Globalization.CultureInfo]::InvariantCulture)
 $state[$key+'Bits']=$bits;$receipt[$key+'Bits']=$bits
}
Assert-RainWaterCoarseCapture $receipt $header $state $state $fixture 100
foreach($key in @('pendingSeconds','rainVolume','infiltrationVolume','evaporationVolume','outletVolume')){
 $bitKey=$key+'Bits';$changed=[BitConverter]::DoubleToInt64Bits([double]$header[$key])+1
 $bad=$receipt.Clone();$bad[$bitKey]=$changed.ToString('X16');Refuses {Assert-RainWaterCoarseCapture $bad $header $state $state $fixture 100}
 $badState=$state.Clone();$badState[$bitKey]=$changed.ToString('X16');Refuses {Assert-RainWaterCoarseCapture $receipt $header $badState $badState $fixture 100}
 Refuses {Assert-RainWaterCoarseCapture $receipt $header $state $badState $fixture 100}
 foreach($invalid in @($null,123,'000000000000000g','000000000000000')){
  $bad=$receipt.Clone();$bad[$bitKey]=$invalid;Refuses {Assert-RainWaterCoarseCapture $bad $header $state $state $fixture 100}
 }
}
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
