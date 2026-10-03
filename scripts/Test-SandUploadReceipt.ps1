[CmdletBinding()]
param([string]$Compiler='clang++',[string]$RustCompiler='rustc',[string]$ActualRuntimeEvidence='')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build/sand-upload-receipt'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$exe=Join-Path $out 'sand-receipt.exe'
$binary=Join-Path $out 'saved-contacts-replay.f32'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' ('-I'+(Join-Path $root 'engine')) `
 (Join-Path $root 'tests/unit/engine/weather/test_sand_upload_receipt.cpp') `
 (Join-Path $root 'engine/Poseidon/World/Weather/SandField.cpp') '-o' $exe
if($LASTEXITCODE -ne 0){throw 'Actual SandField/receipt CPU compile failed.'}
& $exe $binary
if($LASTEXITCODE -ne 0){throw 'Actual SandField/receipt CPU test failed.'}
$source=Get-Content (Join-Path $root 'engine/WgpuRenderer/rust/src/terrain/mod.rs') -Raw
function Declaration([string]$name) {
 $begin=$source.IndexOf('fn '+$name+'(')
 if($begin -lt 0){throw ('Missing actual production function '+$name)}
 $brace=$source.IndexOf('{',$begin);$depth=0
 for($i=$brace;$i -lt $source.Length;++$i){
  if($source[$i] -eq '{'){$depth++}elseif($source[$i] -eq '}'){$depth--;if($depth -eq 0){return $source.Substring($begin,$i-$begin+1)}}
 }
 throw ('Unterminated actual production function '+$name)
}
$rust=(Declaration 'valid_sand_view')+"`n"+(Declaration 'sand_upload_receipt')+@'

fn main() {
 let path=std::env::args().nth(1).expect("snapshot path");
 let bytes=std::fs::read(&path).unwrap();
 assert_eq!(bytes.len(),262148*4);
 let data:Vec<f32>=bytes.chunks_exact(4).map(|b|f32::from_le_bytes(b.try_into().unwrap())).collect();
 assert!(valid_sand_view(&data));
 let words=std::fs::read_to_string(format!("{path}.expected")).unwrap();
 let expected:Vec<u64>=words.split_whitespace().map(|w|w.parse().unwrap()).collect();
 let(hash,negative,positive,min,max)=sand_upload_receipt(&data);
 assert_eq!((hash,negative as u64,positive as u64),(expected[0],expected[1],expected[2]));
 assert_eq!((u64::from(min.to_bits()),u64::from(max.to_bits())),(expected[3],expected[4]));
 assert!(min < -0.08 && max>0.0 && max<=0.02);
 let mut invalid=data.clone();invalid[4]=f32::NAN;assert!(!valid_sand_view(&invalid));
 invalid=data.clone();invalid[4]=-0.151;assert!(!valid_sand_view(&invalid));
 invalid=data.clone();invalid[4]=0.021;assert!(!valid_sand_view(&invalid));
 invalid=data.clone();invalid[3]=0.0;assert!(!valid_sand_view(&invalid));
 println!("Actual Rust sand admission/receipt matches actual C++ snapshot PASS hash={hash:016x}");
}
'@
$file=Join-Path $out 'actual-rust-admission.rs'
Set-Content -LiteralPath $file -Value $rust -Encoding UTF8
$rustExe=Join-Path $out 'rust-receipt.exe'
& $RustCompiler '--edition=2021' '-Dwarnings' '--crate-name' 'actual_sand_admission' $file '-o' $rustExe
if($LASTEXITCODE -ne 0){throw 'Actual extracted Rust admission/receipt compile failed.'}
& $rustExe $binary
if($LASTEXITCODE -ne 0){throw 'Actual Rust admission/receipt CPU test failed.'}
. (Join-Path $PSScriptRoot 'Read-SandUploadSnapshot.ps1')
$snapshot=Read-SandUploadSnapshot $binary
$culture=[Globalization.CultureInfo]::InvariantCulture
$header=@($snapshot.header|ForEach-Object{([double]$_).ToString('R',$culture)})
$minimum=$snapshot.minimum.ToString('F8',$culture);$maximum=$snapshot.maximum.ToString('F8',$culture)
$queued='SAND_UPLOAD_QUEUED revision=4 enabled=true chunks=8 cameraX=5973.80420 cameraZ=3325.57300 originX='+$header[0]+' originZ='+$header[1]+' cell='+$header[2]+' limit='+$header[3]+' count=262148 finite=true negative='+$snapshot.negative+' positive='+$snapshot.positive+' min='+$minimum+' max='+$maximum+' minX=5975.81250 minZ=3325.56250 hash='+$snapshot.hash
$received='SAND_UPLOAD_RECEIVED accepted=true count=262148 header=['+($header -join ', ')+'] negative='+$snapshot.negative+' positive='+$snapshot.positive+' min='+$minimum+' max='+$maximum+' hash='+$snapshot.hash+' byteOffset=2097184 bufferBytes=3145776'
$null=Assert-SandUploadPublication $snapshot $queued $received
$null=Assert-SandUploadPublication $snapshot ($queued+"`r") ($received+"`r`n")
if(($queued+"`r") -notmatch ('SAND_UPLOAD_QUEUED .*hash='+$snapshot.hash+'\r?$')){throw 'CRLF queued receipt lookup failed.'}
foreach($bad in @($received.Replace('accepted=true','accepted=false'),$received.Replace('count=262148','count=262144'),$received.Replace('byteOffset=2097184','byteOffset=2097168'),$received.Replace(('min='+$minimum),'min=-0.15'),$received.Replace($snapshot.hash,'0000000000000000'))){
 $failed=$false;try{$null=Assert-SandUploadPublication $snapshot $queued $bad}catch{$failed=$true}
 if(!$failed){throw 'Mismatched sand accepted receipt passed.'}
}
foreach($bad in @($queued.Replace('finite=true','finite=false'),$queued.Replace('cell=0.125','cell=0.25'),$queued.Replace('count=262148','count=262144'))){
 $failed=$false;try{$null=Assert-SandUploadPublication $snapshot $bad $received}catch{$failed=$true}
 if(!$failed){throw 'Mismatched sand queued receipt passed.'}
}
if($snapshot.sha256 -cne (Get-FileHash -LiteralPath $binary).Hash){throw 'Snapshot same-read SHA256 differs.'}
# Mutation tests exercise the exact saved f32 view; never a live game/store.
$bytes=[IO.File]::ReadAllBytes($binary);$invalid=Join-Path $out 'invalid-cell.f32'
foreach($value in @([single]::NaN,[single](-.151),[single]0.021)){
 $bad=[byte[]]$bytes.Clone();[Array]::Copy([BitConverter]::GetBytes([single]$value),0,$bad,16,4);[IO.File]::WriteAllBytes($invalid,$bad)
 $failed=$false;try{$null=Read-SandUploadSnapshot $invalid}catch{$failed=$true}
 if(!$failed){throw 'Invalid signed snapshot cell passed.'}
}
if($ActualRuntimeEvidence){
 $actual=Read-SandUploadSnapshot (Join-Path $ActualRuntimeEvidence 'latest-sand-snapshot.f32')
 $text=[IO.File]::ReadAllText((Join-Path $ActualRuntimeEvidence 'engine.log'));$end=$text.LastIndexOf("`n")
 if($end -lt 0){throw 'Actual source log has no complete records.'}
 $rows=$text.Substring(0,$end).Split("`n")
 $actualQueued=@($rows|Where-Object{$_ -match ('SAND_UPLOAD_QUEUED .*hash='+$actual.hash+'\r?$')}|Select-Object -Last 1)
 $actualReceived=@(Get-Content -LiteralPath (Join-Path $ActualRuntimeEvidence 'stderr.txt')|Where-Object{$_ -match ('SAND_UPLOAD_RECEIVED accepted=true .*hash='+$actual.hash+' ')}|Select-Object -Last 1)
 if($actualQueued.Count -ne 1 -or $actualReceived.Count -ne 1){throw 'Actual queued/received source record missing.'}
 $null=Assert-SandUploadPublication $actual $actualQueued[0] $actualReceived[0]
 Write-Host ('Saved actual source/Rust/snapshot publication replay PASS hash='+$actual.hash+' negative='+$actual.negative+' positive='+$actual.positive)
}
Write-Host 'Sand publication CPU closure PASS; GPU completion and installed appearance remain unproved.'
