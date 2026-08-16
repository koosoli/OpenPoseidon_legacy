# Does the white mast come from bindless slot 0 (an unresolved texture) or from a real radiance path?
# WGR_TEXTURE_FALLBACK_DEBUG=1 recolours slot 0 magenta. Read pixel (1251,654)-(1253,670).
#   magenta -> texture bug, and any untextured BUILDING face turns magenta in the same frame
#   white   -> a real emissive/specular path, and the texture files are exonerated
$arms = [ordered]@{
  'control' = @{ WGR_OBJECT_STREAM_MAX_OBJECTS = '200000' }
  'magenta' = @{ WGR_OBJECT_STREAM_MAX_OBJECTS = '200000'; WGR_TEXTURE_FALLBACK_DEBUG = '1' }
}
& "$PSScriptRoot\capture-arms.ps1" `
  -Out "$PSScriptRoot\magenta" `
  -World 'C:\Users\mail\AppData\Local\Temp\ff-worlds\stratis\stratis.wrp' `
  -Mods 'a3stratis' `
  -Freefly '2200 5400 220 45 -8' -Hour 10 -ShotSpec '120s' -TimeoutMin 10 -Arms $arms
