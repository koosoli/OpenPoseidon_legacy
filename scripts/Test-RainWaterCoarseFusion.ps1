[CmdletBinding()]
param([Parameter(Mandatory)][string]$BaselineReceipt,[string]$Compiler='clang++',[switch]$Production,
 [switch]$Measure,[ValidateRange(2,8)][int]$Rounds=4)
$ErrorActionPreference='Stop';$root=Split-Path -Parent $PSScriptRoot
function Require([bool]$ok,[string]$message){if(!$ok){throw $message}}
function Hash-Text([string]$v){[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($v)))}
$baselinePath=(Resolve-Path -LiteralPath $BaselineReceipt).Path
$baseline=Get-Content -LiteralPath $baselinePath -Raw|ConvertFrom-Json
Require ($baseline.status-in@('actual-state-baseline-bitwise-pass','actual-state-baseline-bitwise-and-abba-pass')) 'Faithful baseline proof required first.'
if($Measure){Require (@(Get-Process -Name OpenPoseidon -ErrorAction SilentlyContinue).Count-eq0) 'Hold prototype timing while a game is running.'}
Require ((Get-FileHash -LiteralPath (Join-Path $root 'tests/unit/engine/weather/rain_water_coarse_replay.cpp')).Hash-ceq$baseline.replayCppSha256) 'Audited loader/restore bridge differs from baseline proof.'
$baseDir=Split-Path -Parent $baselinePath
$output=Join-Path $root ('build/rain-water-coarse-fusion/run-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmssfff'))
New-Item -ItemType Directory -Force -Path $output|Out-Null
$weather=Join-Path $root 'engine/Poseidon/World/Weather'
$currentSourceHashes=[ordered]@{}
foreach($property in $baseline.sourceSha256.PSObject.Properties){
 $text=[IO.File]::ReadAllText((Join-Path $weather $property.Name)).Replace("`r`n","`n").TrimEnd([char[]]"`n")+"`n"
 $currentSourceHashes[$property.Name]=Hash-Text $text
 if(!$Production-or$property.Name-notin@('RainWaterField.hpp','RainWaterCost.hpp')){
  Require ((Hash-Text $text)-ceq$property.Value) 'Solver dependency differs from faithful baseline.'
 }
 if($property.Name-ceq'RainWaterField.hpp'){$currentSolver=$text}
}
foreach($label in @('A','B')){
 $text=[IO.File]::ReadAllText((Join-Path $baseDir ('RainWaterReplay'+$label+'.hpp')))
 Require ((Hash-Text $text)-ceq$baseline.generatedSha256.$label) 'Audited baseline generated header changed.'
 if($label-ceq'A'){$originalA=$text}else{$originalB=$text}
}
$candidate=$originalB
if($Production){
 Require ($currentSolver.Contains('phase.End(RainWaterCost::Phase::FusedFlux)')) 'Current production does not contain reviewed fused dispatch.'
 Require (([regex]::Matches($currentSolver,'(?m)^private:$')).Count-eq1) 'Exact production friend seam changed.'
 $candidate=[regex]::Replace($currentSolver,'\bRainWaterField\b','RainWaterFieldReplayB')
 $candidate=[regex]::Replace($candidate,'\bGRainWater\b','GRainWaterReplayB')
 $candidate=$candidate.Replace("private:`n","private:`n    friend struct RainWaterReplayBridge;`n")
 $candidate=$candidate.Replace("namespace Poseidon`n{","namespace Poseidon`n{`ninline uint64_t ReplayFusionSteps=0,ReplayFallbackSteps=0;")
 $candidate=$candidate.Replace('phase.End(RainWaterCost::Phase::FusedFlux);','++ReplayFusionSteps; phase.End(RainWaterCost::Phase::FusedFlux);')
 $candidate=$candidate.Replace('phase.End(RainWaterCost::Phase::RawTransfer);','++ReplayFallbackSteps; phase.End(RainWaterCost::Phase::RawTransfer);')
}else{
foreach($seam in @("namespace Poseidon`n{",'bool hasWater = false;',
 '// Sea is a real outlet, not a source of rainwater above dry terrain.','hasWater |= _depth[i] > 0;',
 "if (hasWater) {`n            size_t edgeX=0,edgeZ=0;",
 "phase.End(RainWaterCost::Phase::LimitedFlux);`n        }`n        for")){
 Require (([regex]::Matches($candidate,[regex]::Escape($seam))).Count-eq1) 'Exact prototype dispatch seam changed.'
}
# Isolated generated class only: production header remains byte-for-byte intact.
$candidate=$candidate.Replace("namespace Poseidon`n{","namespace Poseidon`n{`ninline uint64_t ReplayFusionSteps=0,ReplayFallbackSteps=0;")
$candidate=$candidate.Replace('bool hasWater = false;',@'
bool hasWater = false;
        bool canFuse = _spacing >= 10.0f && _spacing <= 1000000.0f;
'@)
$candidate=$candidate.Replace('// Sea is a real outlet, not a source of rainwater above dry terrain.',@'
// Guard only the prototype dispatch, never clamp or alter physical values.
            canFuse &= std::abs(_bed[i]) <= 1000000.0f;
            // Sea is a real outlet, not a source of rainwater above dry terrain.
'@)
$candidate=$candidate.Replace('hasWater |= _depth[i] > 0;',@'
hasWater |= _depth[i] > 0;
            canFuse &= _depth[i] <= 1024.0f &&
                (_depth[i] == 0.0f || _depth[i] >= std::numeric_limits<float>::min());
'@)
$candidate=$candidate.Replace("if (hasWater) {`n            size_t edgeX=0,edgeZ=0;",@'
if (hasWater) {
            if (canFuse) {
                ++ReplayFusionSteps;
                Edges([&](size_t a,size_t b,bool alongX) {
                    const float f=Transfer(a,b);const size_t donor=f>0?a:b;
                    _delta[a]-=f;_delta[b]+=f;
                    auto& flow=alongX ? _flowX : _flowZ;
                    const float velocity=f*_spacing/StepSeconds/std::max(_depth[donor],0.0001f);
                    flow[a]+=velocity*0.5f;flow[b]+=velocity*0.5f;
                });
                phase.End(RainWaterCost::Phase::LimitedFlux);
            } else {
                ++ReplayFallbackSteps;
            size_t edgeX=0,edgeZ=0;
'@)
$candidate=$candidate.Replace("phase.End(RainWaterCost::Phase::LimitedFlux);`n        }`n        for", "phase.End(RainWaterCost::Phase::LimitedFlux);`n            }`n        }`n        for")
Require ($candidate-cne$originalB-and$candidate.Contains('const float f=Transfer(a,b)')-and$candidate.Contains('++ReplayFallbackSteps;')) 'Prototype insertion seam changed.'
}
[IO.File]::WriteAllText((Join-Path $output 'RainWaterReplayA.hpp'),$originalA,[Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $output 'RainWaterReplayB.hpp'),$candidate,[Text.UTF8Encoding]::new($false))
$receipt=@{status='prepared';prototypeOnly=$true;runnerEditsProductionSolver=$false;performanceAccepted=$false;timingPerformed=$false
 baselineReceipt=$baselinePath;baselineReceiptSha256=(Get-FileHash -LiteralPath $baselinePath).Hash
 candidateSha256=(Hash-Text $candidate);scope='Actual-state observable bitwise equivalence plus named synthetic dispatch/fallback controls; unused scratch intentionally differs, no performance acceptance.'
 proofs=@();timings=@();rounds=$Rounds;prototypeScriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash
 prototypeCppSha256=(Get-FileHash -LiteralPath (Join-Path $root 'tests/unit/engine/weather/rain_water_coarse_fusion.cpp')).Hash}
if($Production){
 $receipt.prototypeOnly=$false;$receipt.currentProductionHeaderCopied=$true;$receipt.currentSourceSha256=$currentSourceHashes
 $receipt.scope='Verified captured baseline A versus exact current production B with renamed identifiers/friend and branch witnesses only. Observable state equality, no initial flow history or installed performance claim.'
 # Reverse only fixture witnesses/identifiers. Production B must round-trip to
 # the full actual header, rather than quietly replaying the earlier prototype.
 $roundtrip=$candidate.Replace("inline uint64_t ReplayFusionSteps=0,ReplayFallbackSteps=0;`n",'')
 $roundtrip=$roundtrip.Replace('++ReplayFusionSteps; ','').Replace('++ReplayFallbackSteps; ','')
 $roundtrip=$roundtrip.Replace("    friend struct RainWaterReplayBridge;`n",'')
 $roundtrip=[regex]::Replace($roundtrip,'\bRainWaterFieldReplayB\b','RainWaterField')
 $roundtrip=[regex]::Replace($roundtrip,'\bGRainWaterReplayB\b','GRainWater')
 Require ($roundtrip-ceq$currentSolver) 'Current production copy contains fixture algorithm edits.'
}
$saved=$env:POSEIDON_RAIN_WATER_COST
try{
 $env:POSEIDON_RAIN_WATER_COST='0'
 $receipt.compiler=(& $Compiler '--version') -join "`n"
 Require ($LASTEXITCODE-eq0) 'Compiler unavailable.'
 $receipt.compilerFlags='-std=c++20 -O0/-O2 -Wall -Wextra -Werror; no fast-math'
 foreach($opt in @('-O0','-O2')){
  $exe=Join-Path $output ('fusion'+$opt+'.exe')
  & $Compiler '-std=c++20' $opt '-Wall' '-Wextra' '-Werror' ('-I'+$output) ('-I'+$weather) (Join-Path $root 'tests/unit/engine/weather/rain_water_coarse_fusion.cpp') '-o' $exe
  Require ($LASTEXITCODE-eq0) 'Prototype compilation failed.'
  foreach($arm in $baseline.arms){
   Require ((Get-FileHash -LiteralPath $arm.path).Hash-ceq$arm.sha256) 'Actual native capture changed.'
   $lines=@(& $exe $arm.path $arm.forcing.rainBits $arm.forcing.solarBits $arm.forcing.windBits)
   Require ($LASTEXITCODE-eq0-and$lines.Count-eq2) 'Prototype exact equivalence or fallback failed.'
   $rows=@($lines|ForEach-Object{$_|ConvertFrom-Json})
   Require ($rows[0].observableStateBitwiseEqual-eq$true-and$rows[0].unusedScratchClaimedEqual-eq$false-and$rows[1].exactFallbackAndTransitions-eq$true) 'Prototype proof scope missing.'
   $baselineProof=@($arm.proofs|Where-Object{$_.optimization-ceq$opt})
   Require ($baselineProof.Count-eq1-and$rows[0].outputFnv64-ceq$baselineProof[0].result.outputFnv64) 'Prototype observable fingerprint differs from captured baseline replay.'
   if($arm.name-ceq'Rain'){Require ($rows[0].fusedSteps-gt0) 'Actual rainy state did not exercise fused dispatch.'}
   $receipt.proofs+=@{optimization=$opt;arm=$arm.name;exeSha256=(Get-FileHash -LiteralPath $exe).Hash;results=$rows}
  }
 }
 if($Measure){
  # Remove ONLY branch witnesses, retaining the prototype's production layout.
  # This counter-free timing header is separate from the verified proof header.
  $bench=Join-Path $output 'benchmark';New-Item -ItemType Directory -Force -Path $bench|Out-Null
  $timed=$candidate.Replace("inline uint64_t ReplayFusionSteps=0,ReplayFallbackSteps=0;`n",'')
  $timed=$timed.Replace('++ReplayFusionSteps;','').Replace('++ReplayFallbackSteps;','')
  Require (!$timed.Contains('ReplayFusionSteps')-and!$timed.Contains('ReplayFallbackSteps')) 'Prototype branch witnesses leaked into timing.'
  [IO.File]::WriteAllText((Join-Path $bench 'RainWaterReplayA.hpp'),$originalA,[Text.UTF8Encoding]::new($false))
  [IO.File]::WriteAllText((Join-Path $bench 'RainWaterReplayB.hpp'),$timed,[Text.UTF8Encoding]::new($false))
  $receipt.timedCandidateSha256=Hash-Text $timed
  $exe=Join-Path $bench 'fusion-benchmark.exe'
  & $Compiler '-std=c++20' '-O2' '-Wall' '-Wextra' '-Werror' ('-I'+$bench) ('-I'+$weather) (Join-Path $root 'tests/unit/engine/weather/rain_water_coarse_replay.cpp') '-o' $exe
  Require ($LASTEXITCODE-eq0) 'Counter-free prototype benchmark compilation failed.'
  Require (@(Get-Process -Name OpenPoseidon -ErrorAction SilentlyContinue).Count-eq0) 'A game appeared before prototype timing.'
  foreach($arm in $baseline.arms){
   $lines=@(& $exe $arm.path $arm.forcing.rainBits $arm.forcing.solarBits $arm.forcing.windBits 'bench' $Rounds)
   Require ($LASTEXITCODE-eq0-and$lines.Count-eq4*$Rounds) 'Prototype bounded ABBA execution failed.'
   Require (@(Get-Process -Name OpenPoseidon -ErrorAction SilentlyContinue).Count-eq0) 'A game appeared during prototype timing.'
   $rows=@($lines|ForEach-Object{$_|ConvertFrom-Json})
   Require (@($rows.outputFnv64|Select-Object -Unique).Count-eq1) 'Timed baseline/prototype observable fingerprints differ.'
   $receipt.timings+=@{arm=$arm.name;rawBlocks=$rows}
  }
  $receipt.timingPerformed=$true
 }
 $receipt.status='prototype-observable-bitwise-pass-performance-unmeasured'
 if($Production){$receipt.status='production-copy-observable-bitwise-pass-performance-unmeasured'}
 if($Measure){
  $receipt.status=if($Production){'production-copy-observable-bitwise-pass-exploratory-abba'}else{'prototype-observable-bitwise-pass-exploratory-abba'}
  $receipt.timingScope=if($Production){'Offline counter-free copy of verified current production header only. Host quietness beyond game absence and installed performance acceptance remain root-owned.'}else{'Offline counter-free generated prototype only. Host quietness beyond game absence and installed performance acceptance remain root-owned.'}
 }
 $receipt|ConvertTo-Json -Depth 15|Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding UTF8
 Write-Output ($receipt.status+'; '+(Join-Path $output 'result.json'))
}finally{$env:POSEIDON_RAIN_WATER_COST=$saved}
