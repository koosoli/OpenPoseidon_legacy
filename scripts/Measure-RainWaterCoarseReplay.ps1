[CmdletBinding()]
param(
 [Parameter(Mandatory)][string]$CampaignDirectory,
 [string]$Compiler='clang++',
 [ValidateRange(2,8)][int]$Rounds=4,
 [switch]$SkipBenchmark
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'RainWaterCoarseCapture.ps1')
$campaign=(Resolve-Path -LiteralPath $CampaignDirectory).Path
$resultPath=Join-Path $campaign 'result.json'
$resultHash=(Get-FileHash -LiteralPath $resultPath).Hash
$result=Get-Content -LiteralPath $resultPath -Raw|ConvertFrom-Json
function Require([bool]$ok,[string]$message){if(!$ok){throw $message}}
function Sha-Text([string]$text){
 [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($text)))
}
function Canonical([string]$text){$text.Replace("`r`n","`n").TrimEnd([char[]]"`n")+"`n"}
function Float-Bits($value){
 Require ($null-ne$value-and[double]::IsFinite([double]$value)-and[double]$value-ge0-and[double]$value-le1) 'Missing/nonfinite endpoint forcing.'
 [BitConverter]::SingleToUInt32Bits([single]$value).ToString('X8',[Globalization.CultureInfo]::InvariantCulture)
}
Require ($result.status-ceq'actual-occupied-dry-rain-flight-measured') 'Campaign did not pass occupied flight capture.'
Require ($result.sourceHead-cmatch'^[0-9a-f]{40}$'-and$result.installed.stamp-match[regex]::Escape($result.sourceHead.Substring(0,8))) 'Captured installed/source commit identity missing.'
Require ($result.installed.exe-cmatch'^[0-9A-F]{64}$'-and$result.installed.dll-cmatch'^[0-9A-F]{64}$') 'Captured matched-pair hashes missing.'
$games=@(Get-Process -Name OpenPoseidon -ErrorAction SilentlyContinue)
if(!$SkipBenchmark){Require ($games.Count-eq0) 'A game is running: hold offline performance timing; use -SkipBenchmark for source/correctness checks.'}
$runName='run-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmssfff')
$output=Join-Path $root ('build/rain-water-coarse-replay/'+[IO.Path]::GetFileName($campaign)+'/'+$runName)
New-Item -ItemType Directory -Force -Path $output|Out-Null
$weather=Join-Path $root 'engine/Poseidon/World/Weather'
$sourceHashes=[ordered]@{}
foreach($name in @('RainWaterField.hpp','RainWaterFine.hpp','RainWaterGeometry.hpp','RainWaterForcing.hpp','RainWaterCost.hpp')){
 $path='engine/Poseidon/World/Weather/'+$name
 $pinned=(git -C $root show ($result.sourceHead+':'+$path)) -join "`n"
 Require ($LASTEXITCODE-eq0) ('Captured source unavailable: '+$name)
 $pinned=Canonical $pinned
 $actual=Canonical ([IO.File]::ReadAllText((Join-Path $root $path)))
 Require ($actual-ceq$pinned) ('Actual solver dependency differs from captured installed source: '+$name)
 $sourceHashes[$name]=Sha-Text $actual
 if($name-ceq'RainWaterField.hpp'){$solver=$actual}
}
Require ((Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'RainWaterCoarseCapture.ps1')).Hash-ceq$result.helperHashes.coarseCapture) 'Exact receipt helper differs from captured campaign.'
Require (([regex]::Matches($solver,'(?m)^private:$')).Count-eq1) 'Exact test-only friend insertion seam changed.'
$generatedHashes=[ordered]@{}
foreach($label in @('A','B')){
 $generated=[regex]::Replace($solver,'\bRainWaterField\b','RainWaterFieldReplay'+$label)
 $generated=[regex]::Replace($generated,'\bGRainWater\b','GRainWaterReplay'+$label)
 $generated=$generated.Replace("private:`n","private:`n    friend struct RainWaterReplayBridge;`n")
 $file=Join-Path $output ('RainWaterReplay'+$label+'.hpp')
 [IO.File]::WriteAllText($file,$generated,[Text.UTF8Encoding]::new($false))
 $generatedHashes[$label]=Sha-Text $generated
}
# Reverse the ONLY permitted identifier/friend edits. Fail closed if a future
# prototype silently adds an algorithm, forcing, layout or admission change.
foreach($label in @('A','B')){
 $roundtrip=[IO.File]::ReadAllText((Join-Path $output ('RainWaterReplay'+$label+'.hpp')))
 $roundtrip=$roundtrip.Replace("    friend struct RainWaterReplayBridge;`n",'')
 $roundtrip=[regex]::Replace($roundtrip,'\bRainWaterFieldReplay'+$label+'\b','RainWaterField')
 $roundtrip=[regex]::Replace($roundtrip,'\bGRainWaterReplay'+$label+'\b','GRainWater')
 Require ($roundtrip-ceq$solver) 'Generated baseline contains unapproved production changes.'
}
$arms=@()
foreach($weatherName in @('Dry','Rain')){
 $matches=@($result.arms|Where-Object{$_.control-ceq'Default'-and$_.weatherName-ceq$weatherName})
 Require ($matches.Count-eq1-and$matches[0].status-ceq'actual-occupied-flight-captured') 'Actual default arm absent/ambiguous.'
 $arm=$matches[0];$capture=$arm.coarseCapture
 $path=Join-Path $campaign ('Default-'+$weatherName+'/coarse-runoff.rwcap')
 $hash=(Get-FileHash -LiteralPath $path).Hash
 Require ($hash-ceq$capture.sha256) 'Capture SHA256 differs from completed campaign.'
 $header=Read-RainWaterCoarseCapture $path
 Assert-RainWaterCoarseCapture $capture.receipt $header $capture.before $capture.after $path $capture.receipt.timeMs
 Require ($capture.receipt.terrainRange-eq2048-and$capture.receipt.terrainSpacing-eq6.25-and$capture.receipt.sourceStride-eq4-and$header.width-eq513-and$header.height-eq513) 'Exact original native source/grid receipt differs.'
 Require ($capture.receipt.particleSnowflakes-is[bool]-and$capture.receipt.particleSnowflakes-eq$false) 'Endpoint is not actual liquid rain.'
 $forcing=@{rainBits=(Float-Bits $capture.receipt.liquidRain);solarBits=(Float-Bits $capture.receipt.solarDrying);windBits=(Float-Bits $capture.receipt.windInput)}
 $arms+=@{name=$weatherName;path=$path;sha256=$hash;header=$header;receipt=$capture.receipt;forcing=$forcing;proofs=@();timings=@()}
}
$receipt=[ordered]@{
 status='prepared';scope='Exact captured coarse state plus held endpoint float forcing, not historical flight replay or game FPS.'
 campaign=$campaign;campaignResultSha256=$resultHash;capturedSourceHead=$result.sourceHead;installed=$result.installed
 replayScriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash
 replayCppSha256=(Get-FileHash -LiteralPath (Join-Path $root 'tests/unit/engine/weather/rain_water_coarse_replay.cpp')).Hash
 sourceSha256=$sourceHashes;generatedSha256=$generatedHashes
 reconstruction='Configure-sized arrays, exact bed/depth/pending/four budgets/generation/revision. Uncaptured flows/scratch start zero; full bitwise checks begin after first genuine Step.'
 diagnosticEnabled=$false;rounds=$Rounds;timedStepsPerBlock=16;order='ABBA';arms=$arms
 initialGameProcesses=@($games|ForEach-Object{@{pid=$_.Id;started=$_.StartTime.ToUniversalTime().ToString('o')}})
 timingScope='Exploratory offline solver timings only. Absence of OpenPoseidon does not establish a generally quiet host; no optimisation acceptance or game FPS.'
}
$saved=$env:POSEIDON_RAIN_WATER_COST
try{
 $env:POSEIDON_RAIN_WATER_COST='0'
 $compilerVersion=(& $Compiler '--version') -join "`n"
 Require ($LASTEXITCODE-eq0) 'Compiler unavailable.'
 $receipt.compiler=$compilerVersion
 foreach($opt in @('-O0','-O2')){
  $exe=Join-Path $output ('replay'+$opt+'.exe')
  & $Compiler '-std=c++20' $opt '-Wall' '-Wextra' '-Werror' ('-I'+$output) ('-I'+$weather) (Join-Path $root 'tests/unit/engine/weather/rain_water_coarse_replay.cpp') '-o' $exe
  Require ($LASTEXITCODE-eq0) 'Exact replay compilation failed.'
  foreach($arm in $arms){
   $lines=@(& $exe $arm.path $arm.forcing.rainBits $arm.forcing.solarBits $arm.forcing.windBits 'proof' $Rounds)
   Require ($LASTEXITCODE-eq0-and$lines.Count-eq1) 'Exact baseline/control replay failed.'
   $proof=$lines[0]|ConvertFrom-Json
   Require ($proof.kind-ceq'bitwise-proof'-and$proof.advancesCompared-eq14-and$proof.firstGenuineStep-eq$true-and$proof.flowHistoryCaptured-eq$false) 'Native proof scope incomplete.'
   $arm.proofs+=@{optimization=$opt;exeSha256=(Get-FileHash -LiteralPath $exe).Hash;result=$proof}
  }
 }
 foreach($arm in $arms){
  Require ($arm.proofs[0].result.outputFnv64-ceq$arm.proofs[1].result.outputFnv64) 'O0/O2 exact replay endpoint differs.'
 }
 if(!$SkipBenchmark){
  foreach($arm in $arms){
   $lines=@(& $exe $arm.path $arm.forcing.rainBits $arm.forcing.solarBits $arm.forcing.windBits 'bench' $Rounds)
   Require ($LASTEXITCODE-eq0-and$lines.Count-eq$Rounds*4) 'Bounded ABBA replay failed.'
   foreach($line in $lines){$arm.timings+=($line|ConvertFrom-Json)}
   Require (@(Get-Process -Name OpenPoseidon -ErrorAction SilentlyContinue).Count-eq0) 'A game appeared during offline timing; performance acceptance is refused.'
   Require (@($arm.timings.outputFnv64|Select-Object -Unique).Count-eq1) 'Identical restored baseline/control timed outputs differ.'
   foreach($label in @('A','B')){
    $rows=@($arm.timings|Where-Object{$_.label-ceq$label})
    $wall=@($rows.wallMs|Sort-Object);$cpu=@($rows.threadCpuMs|Where-Object{$null-ne$_}|Sort-Object)
    $summary=@{label=$label;blocks=$rows.Count;medianWallMsPerStep=($wall[$wall.Count/2-1]+$wall[$wall.Count/2])/32.0;threadCpuSamples=$cpu.Count}
    if($cpu.Count-eq$rows.Count){$summary.medianThreadCpuMsPerStep=($cpu[$cpu.Count/2-1]+$cpu[$cpu.Count/2])/32.0}
    if(!$arm.ContainsKey('summary')){$arm.summary=@()};$arm.summary+=,$summary
   }
  }
 }
 foreach($arm in $arms){Require ((Get-FileHash -LiteralPath $arm.path).Hash-ceq$arm.sha256) 'Read-only capture changed during offline replay.'}
 Require ((Get-FileHash -LiteralPath $resultPath).Hash-ceq$resultHash) 'Completed campaign result changed during replay.'
 $receipt.status=$(if($SkipBenchmark){'actual-state-baseline-bitwise-pass'}else{'actual-state-baseline-bitwise-and-abba-pass'})
 $receipt|ConvertTo-Json -Depth 20|Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding UTF8
 Write-Output ($receipt.status+'; '+(Join-Path $output 'result.json'))
}finally{$env:POSEIDON_RAIN_WATER_COST=$saved}
