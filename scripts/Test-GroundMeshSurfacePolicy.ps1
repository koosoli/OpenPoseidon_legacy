[CmdletBinding()]
param([string]$Compiler = 'clang++')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$output = Join-Path $repo 'build/ground-mesh-policy'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$exe = Join-Path $output 'ground_mesh_surface.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' ('-I' + (Join-Path $repo 'engine')) (Join-Path $repo 'tests/unit/engine/WgpuRenderer/test_ground_mesh_surface.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'Ground mesh policy compilation failed.' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Ground mesh/actual residency receipt policy failed.' }
$direct = Get-Content -Raw (Join-Path $repo 'engine/WgpuRenderer/rust/src/gfx3d/shader3d.wgsl')
$retained = Get-Content -Raw (Join-Path $repo 'engine/WgpuRenderer/rust/src/gfx3d/gpu_driven.wgsl')
$conform = Get-Content -Raw (Join-Path $repo 'engine/WgpuRenderer/rust/src/shaders/conform.wgsl')
if (([regex]::Matches($direct, [regex]::Escape('return shade_fragment(in, direct_ground_receiver(in));'))).Count -ne 2) { throw 'Ordinary and early-Z entries lost explicit receiver admission.' }
if (!$retained.Contains('(in.surface_receivers & 1u) != 0u') -or !$retained.Contains('(in.surface_receivers & 2u) == 0u') -or $retained.Contains('in.snow_receiver != 0u')) { throw 'Snow and ground receiver bits are not independent.' }
if (!$retained.Contains('(inst.flags >> 7u) & 3u') -or $retained.Contains('@location(15)')) { throw 'Receiver varying packing changed.' }
foreach ($shader in @($direct,$retained)) {
    if (!$shader.Contains('rigid_ground_fragment(in.world_pos + frame.cam_pos.xyz, in.normal, in.world_pos)')) { throw 'Missing actual per-fragment bare-ground proof.' }
}
foreach ($proof in @('textureDimensions(hm)', 'dims.x != hm_params.hm_width', 'dims.y != hm_params.hm_height', 'abs(world_abs.y - bare_surface_y(world_abs.xz)) <= 0.15', 'dot(authored_normal, camera_relative) <= 0.0', '!bare_surface_valid(world_abs.xz)')) {
    if (!$conform.Contains($proof)) { throw ('Missing conservative map/side proof: ' + $proof) }
}
$bare = $conform.Substring($conform.IndexOf('fn bare_surface_valid'), $conform.IndexOf('fn surface_y(') - $conform.IndexOf('fn bare_surface_valid'))
if ($bare.Contains('snow.') -or $bare.Contains('mud_height_offset') -or $bare.Contains('sand_height_offset')) { throw 'Rigid classification must not consume fragment storage/soft displacement.' }
foreach ($file in @('EngineWgpu.cpp','EngineWgpuRetailPages.cpp','EngineWgpuRetailWorldInstances.cpp','RetailPageInstanceObservation.hpp','RetailWorldInstanceTransaction.hpp','RetailWorldStableFrameObservation.hpp')) {
    $source=Get-Content -Raw (Join-Path $repo ('engine/WgpuRenderer/' + $file))
    if ($source.Contains('~WGR_INSTANCE_SNOW_RECEIVER') -or $source.Contains('& WGR_INSTANCE_SNOW_RECEIVER') -or $source.Contains('&= WGR_INSTANCE_SNOW_RECEIVER')) { throw ('Receiver tag lost through residency mask: ' + $file) }
}
Write-Host 'Ground owner policy, actual paging receipt gates and shader source contracts passed. GPU/runtime remain separate.'
