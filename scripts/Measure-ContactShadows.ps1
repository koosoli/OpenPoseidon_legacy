param(
 [ValidateRange(0,2)][int]$Mode=0,
 [ValidateSet('native','msaa','dlss')][string]$AA='native',
 [ValidateRange(0,8)][double]$SunRadius=0.266,
 [ValidateRange(1,10)][int]$Repeats=1,
 [string]$Label='',
 [switch]$Reference,
 [switch]$Motion,
 [switch]$PlayerMotion,
 [switch]$Close
)
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
$root=Split-Path $PSScriptRoot -Parent
$out=Join-Path $root 'build/contact-shadows'
$name="mode$Mode-$AA-radius$SunRadius"
if ($Reference) {$name="reference-$name"}
if ($Motion) {$name+="-motion"}
if ($PlayerMotion) {$name+="-player"}
if ($Close) {$name+="-close"}
if ($Label) {$name+="-$Label"}
$user=Join-Path $out ("user-"+$name)
New-Item -ItemType Directory -Force $user | Out-Null
& "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $user -DlssMode $(if ($AA -eq 'dlss') {1} else {0}) -MsaaSamples $(if ($AA -eq 'native') {1} else {4})
$settings=@{
 POSEIDON_USER_DIR=$user; POSEIDON_VSYNC='0'; POSEIDON_LOCKSTEP_HZ='0'
 WGR_CONTACT_SHADOWS=[string]$Mode
 WGR_SUN_ANGULAR_RADIUS=$SunRadius.ToString([Globalization.CultureInfo]::InvariantCulture)
 WGR_AUTO_EXPOSURE='0'; WGR_EXPOSURE='0.15'
 WGR_LOD_GOVERNOR_RANGE='1'; WGR_PASS1_STATS='1'
 POSEIDON_FRAME_TRACE=(Join-Path $out ("trace-"+$name))
}
$options=@{
 Label=$name; Mission='tests/perf/missions/contact_shadows.eden'; Out=$out; Env=$settings
 Repeats=$Repeats; Width=1600; Height=900; Windowed=$true; WarmupSeconds=45
 RequireAll=$true; SampleMemory=$true
}
if ($Reference) {
 $options.Mission='tests/perf/missions/perf_field.eden'
 $options.World='C:/Program Files (x86)/Steam/steamapps/common/Arma Reforger/addons'
 $options.Freefly=@(4665.50,7132.80,192.09,123.7,-11.8)
 $options.WorldHour=16
 $options.WarmupSeconds=120
 $settings.POSEIDON_REFORGER_WORLD='worlds/eden'
 $settings.POSEIDON_REFORGER_OBJECTS='1'
 $settings.POSEIDON_REFORGER_STREAM='1'
 $settings.WGR_NATIVE_FAR_AUTHORED='1'
}
if ($Motion) {
 if ($Reference) {throw 'Motion fixture is the OFP town, not the static native reference'}
 $options.WarmupSeconds=85
 $options.MotionSamples=8
 $options.MotionInterval=1
}
if ($PlayerMotion) {
 if ($Reference -or $Motion) {throw 'PlayerMotion is a separate close-up fixture'}
 $options.Mission='tests/perf/missions/contact_player.eden'
 $options.WarmupSeconds=30
 $options.MotionSamples=16
 $options.MotionInterval=0.25
}
if ($Close) {
 if ($Reference -or $Motion -or $PlayerMotion) {throw 'Close is a separate static fixture'}
 $options.Mission='tests/perf/missions/contact_soft.eden'
}
& "$PSScriptRoot/farfield-bench.ps1" @options
if ($LASTEXITCODE -ne 0) {throw 'Contact shadow capture failed'}
foreach ($capture in Get-ChildItem -LiteralPath (Join-Path $out $name) -Filter 'run-??.json') {
 $report=Get-Content -LiteralPath $capture.FullName -Raw | ConvertFrom-Json
 $expectedMsaa=if ($AA -eq 'native') {1} else {4}
 if ($report.build.msaa_samples -ne $expectedMsaa) {throw "Wrong actual MSAA: $($report.build.msaa_samples), expected $expectedMsaa"}
 if ([bool]$report.build.dlss_active -ne ($AA -eq 'dlss')) {throw "Wrong actual DLSS route: $($report.build.dlss_reason)"}
 if ($report.build.output_width -ne 1600 -or $report.build.output_height -ne 900) {throw 'Wrong output resolution'}
}
