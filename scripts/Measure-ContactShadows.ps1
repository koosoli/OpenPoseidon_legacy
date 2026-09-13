param(
 [ValidateRange(0,2)][int]$Mode=0,
 [ValidateSet('native','msaa','dlss')][string]$AA='native',
 [ValidateRange(0,2)][double]$SunRadius=0.266,
 [ValidateRange(1,10)][int]$Repeats=1,
 [string]$Label='',
 [switch]$Reference
)
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
$root=Split-Path $PSScriptRoot -Parent
$out=Join-Path $root 'build/contact-shadows'
$name="mode$Mode-$AA-radius$SunRadius"
if ($Reference) {$name="reference-$name"}
if ($Label) {$name+="-$Label"}
$user=Join-Path $out ("user-"+$name)
New-Item -ItemType Directory -Force $user | Out-Null
& "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $user -DlssMode $(if ($AA -eq 'dlss') {1} else {0}) -MsaaSamples $(if ($AA -eq 'msaa') {4} else {1})
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
& "$PSScriptRoot/farfield-bench.ps1" @options
if ($LASTEXITCODE -ne 0) {throw 'Contact shadow capture failed'}
