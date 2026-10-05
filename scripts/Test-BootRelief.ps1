[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
try {
    cargo test -p wgpu_renderer boot_relief --lib --target-dir build/boot-relief-rust -- --nocapture
    if ($LASTEXITCODE -ne 0) { throw 'Boot relief shader/GPU validation failed.' }
    cargo test -p wgpu_renderer direct_normal_channels_keep_flags_and_exclude_red_from_parallax --lib --target-dir build/boot-relief-rust
    if ($LASTEXITCODE -ne 0) { throw 'Boot relief material flag regression failed.' }
} finally {
    Pop-Location
}
