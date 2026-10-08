<# Explicit bounded OSM import (one cell or its 3x3 neighborhood).
   Never invoked by the game; no retries or bulk loop.
   Public Overpass instances are not an application/world-streaming backend.
   Data: https://www.openstreetmap.org/copyright (ODbL 1.0). #>
param(
 [ValidateRange(-84,84)][double]$Latitude=49.61,
 [ValidateRange(-180,180)][double]$Longitude=6.13,
 [string]$CacheDirectory=(Join-Path $env:LOCALAPPDATA 'OpenPoseidon/world-stream'),
 [string]$Endpoint='https://overpass-api.de/api/interpreter',
 [switch]$Force,
 [switch]$PrepareOnly,
 [switch]$Neighborhood,
 [switch]$IncludeIndividualTrees,
 [string]$InputFile=''
)
$ErrorActionPreference='Stop'
$culture=[Globalization.CultureInfo]::InvariantCulture
$Longitude=$Longitude-360*[Math]::Floor(($Longitude+180)/360)
$latCell=[int][Math]::Floor($Latitude/.02);$lonCell=[int][Math]::Floor($Longitude/.02)
if(($latCell+1)*.02-gt84){throw 'Cell exceeds supported latitude boundary'}
$radius=0;if($Neighborhood){$radius=1}
$bbox=@((($latCell-$radius)*.02),(($lonCell-$radius)*.02),(($latCell+1+$radius)*.02),(($lonCell+1+$radius)*.02))|ForEach-Object {$_.ToString('F8',$culture)}
if($Neighborhood -and ($latCell-$radius-lt-4200 -or $latCell+1+$radius-gt4200 -or $lonCell-$radius-lt-9000 -or $lonCell+1+$radius-gt9000)){throw 'Neighborhood query cannot cross the dateline or supported latitude boundary'}
$bounds=$bbox -join ','
$selectors=@('way["highway"]','way["waterway"]','way["natural"~"^(water|wood)$"]','way["landuse"~"^(forest|reservoir)$"]','node["traffic_sign"]','node["highway"~"^(stop|give_way)$"]')
if($IncludeIndividualTrees){$selectors+='node["natural"="tree"]'}
$query='[out:json][timeout:25][maxsize:33554432];('+ (($selectors|ForEach-Object {$_+'('+$bounds+');'}) -join '')+');out tags geom 6000;'
$directory=Join-Path ([IO.Path]::GetFullPath($CacheDirectory)) 'features'
$destination=Join-Path $directory ($latCell.ToString($culture)+'_'+$lonCell.ToString($culture)+'.json')
$summary=@{file=$destination;query=$query;attribution='(c) OpenStreetMap contributors, ODbL 1.0';source='https://www.openstreetmap.org/copyright';endpoint=$Endpoint}
if($PrepareOnly){$summary;return}
if(Test-Path -LiteralPath $destination){if(!$Force){Write-Host 'Cell already cached; no request sent.';$summary;return}}
New-Item -ItemType Directory -Force -Path $directory|Out-Null
if(@(Get-ChildItem -LiteralPath $directory -File -Filter '*.json'|Where-Object {$_.Name-match'^-?\d+_-?\d+\.json$'}).Count-ge64 -and !(Test-Path -LiteralPath $destination)){
 throw '64-cell cache limit reached. Remove an unwanted cell explicitly before preparing another.'
}
if(([uri]$Endpoint).Scheme-ne'https'){throw 'HTTPS endpoint required'}
if($InputFile){
 $inputPath=[IO.Path]::GetFullPath($InputFile)
 if((Get-Item -LiteralPath $inputPath).Length-gt4194304){throw 'Cell exceeds 4MiB'}
 $text=[IO.File]::ReadAllText($inputPath);$json=$text|ConvertFrom-Json
 if($json.remark -or $json.elements -isnot [array] -or @($json.elements).Count-ge6000){throw 'Invalid, incomplete or possibly truncated element list'}
 $temporary=$destination+'.tmp';[IO.File]::WriteAllText($temporary,$text,[Text.UTF8Encoding]::new($false))
 Move-Item -LiteralPath $temporary -Destination $destination -Force
 $summary.sourceFile=$inputPath;$summary.elements=@($json.elements).Count;$summary|ConvertTo-Json|Set-Content -LiteralPath ($destination+'.source.txt')
 [IO.File]::WriteAllText((Join-Path $directory 'ATTRIBUTION.txt'),"Geographic feature data (c) OpenStreetMap contributors, ODbL 1.0.`nhttps://www.openstreetmap.org/copyright`n")
 $summary;return
}
Add-Type -AssemblyName System.Net.Http
$client=[Net.Http.HttpClient]::new();$client.Timeout=[TimeSpan]::FromSeconds(50)
$client.DefaultRequestHeaders.UserAgent.ParseAdd('OpenPoseidon-EarthFeatures-Manual/1.0 (+https://github.com/koosoli/OpenPoseidon)')
$response=$null;$stream=$null;$memory=[IO.MemoryStream]::new()
try{
 $body=[Net.Http.StringContent]::new(('data='+[uri]::EscapeDataString($query)),[Text.Encoding]::UTF8,'application/x-www-form-urlencoded')
 $request=[Net.Http.HttpRequestMessage]::new([Net.Http.HttpMethod]::Post,$Endpoint);$request.Content=$body
 $response=$client.SendAsync($request,[Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult()
 if(!$response.IsSuccessStatusCode){throw ('Overpass HTTP '+[int]$response.StatusCode+'; no automatic retry')}
 if($response.Content.Headers.ContentLength-gt4194304){throw 'Cell exceeds 4MiB; use a local data backend'}
 $stream=$response.Content.ReadAsStreamAsync().GetAwaiter().GetResult();$buffer=New-Object byte[] 16384
 while(($read=$stream.Read($buffer,0,$buffer.Length))-gt0){if($memory.Length+$read-gt4194304){throw 'Cell exceeds 4MiB'};$memory.Write($buffer,0,$read)}
 $summary.bytes=$memory.Length;$summary.preparedUtc=[DateTime]::UtcNow.ToString('o')
 $text=[Text.Encoding]::UTF8.GetString($memory.ToArray());$json=$text|ConvertFrom-Json
 $summary.elements=@($json.elements).Count
 # A receipt is retained even when incomplete data is rejected. It is never a runtime cell.
 $attempt=$destination+'.'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfff')+'.attempt.txt'
 $summary|ConvertTo-Json|Set-Content -LiteralPath $attempt
 if($json.remark){throw ('Incomplete Overpass response: '+$json.remark)}
 if($json.elements -isnot [array]){throw 'Missing element array'}
 if(@($json.elements).Count-ge6000){
  [IO.File]::WriteAllBytes(($attempt+'.rejected-response.txt'),$memory.ToArray())
  throw ('Possibly truncated element list: '+@($json.elements).Count+' elements, '+$memory.Length+' bytes; no retry')
 }
 $temporary=$destination+'.tmp';[IO.File]::WriteAllBytes($temporary,$memory.ToArray())
 Move-Item -LiteralPath $temporary -Destination $destination -Force
 $summary.bytes=$memory.Length;$summary.elements=@($json.elements).Count;$summary.preparedUtc=[DateTime]::UtcNow.ToString('o')
 $summary|ConvertTo-Json|Set-Content -LiteralPath ($destination+'.source.txt')
 [IO.File]::WriteAllText((Join-Path $directory 'ATTRIBUTION.txt'),"Geographic feature data (c) OpenStreetMap contributors, ODbL 1.0.`nhttps://www.openstreetmap.org/copyright`nManual single-cell Overpass import; not a public-server streaming backend.`n")
 $summary
}finally{if($stream){$stream.Dispose()};if($response){$response.Dispose()};$memory.Dispose();$client.Dispose()}
