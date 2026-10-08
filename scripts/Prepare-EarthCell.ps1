<# Explicit one-cell OSM import. Never invoked by the game; no retries or bulk loop.
   Public Overpass instances are not an application/world-streaming backend.
   Data: https://www.openstreetmap.org/copyright (ODbL 1.0). #>
param(
 [ValidateRange(-84,84)][double]$Latitude=49.61,
 [ValidateRange(-180,180)][double]$Longitude=6.13,
 [string]$CacheDirectory=(Join-Path $env:LOCALAPPDATA 'OpenPoseidon/world-stream'),
 [string]$Endpoint='https://overpass-api.de/api/interpreter',
 [switch]$Force,
 [switch]$PrepareOnly
)
$ErrorActionPreference='Stop'
$culture=[Globalization.CultureInfo]::InvariantCulture
$Longitude=$Longitude-360*[Math]::Floor(($Longitude+180)/360)
$latCell=[int][Math]::Floor($Latitude/.02);$lonCell=[int][Math]::Floor($Longitude/.02)
$bbox=@(($latCell*.02),($lonCell*.02),(($latCell+1)*.02),(($lonCell+1)*.02))|ForEach-Object {$_.ToString('F8',$culture)}
$query='[out:json][timeout:25][maxsize:33554432];way["building"]('+($bbox -join ',')+');out tags geom 2000;'
$directory=Join-Path ([IO.Path]::GetFullPath($CacheDirectory)) 'buildings'
$destination=Join-Path $directory ($latCell.ToString($culture)+'_'+$lonCell.ToString($culture)+'.json')
$summary=@{file=$destination;query=$query;attribution='(c) OpenStreetMap contributors, ODbL 1.0';source='https://www.openstreetmap.org/copyright';endpoint=$Endpoint}
if($PrepareOnly){$summary;return}
if(Test-Path -LiteralPath $destination){if(!$Force){Write-Host 'Cell already cached; no request sent.';$summary;return}}
New-Item -ItemType Directory -Force -Path $directory|Out-Null
if(@(Get-ChildItem -LiteralPath $directory -File -Filter '*.json'|Where-Object {$_.Name-match'^-?\d+_-?\d+\.json$'}).Count-ge64 -and !(Test-Path -LiteralPath $destination)){
 throw '64-cell cache limit reached. Remove an unwanted cell explicitly before preparing another.'
}
if(([uri]$Endpoint).Scheme-ne'https'){throw 'HTTPS endpoint required'}
Add-Type -AssemblyName System.Net.Http
$client=[Net.Http.HttpClient]::new();$client.Timeout=[TimeSpan]::FromSeconds(50)
$client.DefaultRequestHeaders.UserAgent.ParseAdd('OpenPoseidon-EarthCell-Manual/1.0 (+https://github.com/koosoli/OpenPoseidon)')
$response=$null;$stream=$null;$memory=[IO.MemoryStream]::new()
try{
 $body=[Net.Http.StringContent]::new(('data='+[uri]::EscapeDataString($query)),[Text.Encoding]::UTF8,'application/x-www-form-urlencoded')
 $request=[Net.Http.HttpRequestMessage]::new([Net.Http.HttpMethod]::Post,$Endpoint);$request.Content=$body
 $response=$client.SendAsync($request,[Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult()
 if(!$response.IsSuccessStatusCode){throw ('Overpass HTTP '+[int]$response.StatusCode+'; no automatic retry')}
 if($response.Content.Headers.ContentLength-gt4194304){throw 'Cell exceeds 4MiB; use a local data backend'}
 $stream=$response.Content.ReadAsStreamAsync().GetAwaiter().GetResult();$buffer=New-Object byte[] 16384
 while(($read=$stream.Read($buffer,0,$buffer.Length))-gt0){if($memory.Length+$read-gt4194304){throw 'Cell exceeds 4MiB'};$memory.Write($buffer,0,$read)}
 $text=[Text.Encoding]::UTF8.GetString($memory.ToArray());$json=$text|ConvertFrom-Json
 if($json.remark){throw ('Incomplete Overpass response: '+$json.remark)}
 if($null-eq$json.elements -or @($json.elements).Count-gt2000){throw 'Invalid or oversized element list'}
 $temporary=$destination+'.tmp';[IO.File]::WriteAllBytes($temporary,$memory.ToArray())
 Move-Item -LiteralPath $temporary -Destination $destination -Force
 $summary.bytes=$memory.Length;$summary.elements=@($json.elements).Count;$summary.preparedUtc=[DateTime]::UtcNow.ToString('o')
 $summary|ConvertTo-Json|Set-Content -LiteralPath ($destination+'.source.txt')
 [IO.File]::WriteAllText((Join-Path $directory 'ATTRIBUTION.txt'),"Building data (c) OpenStreetMap contributors, ODbL 1.0.`nhttps://www.openstreetmap.org/copyright`nManual single-cell Overpass import; not a public-server streaming backend.`n")
 $summary
}finally{if($stream){$stream.Dispose()};if($response){$response.Dispose()};$memory.Dispose();$client.Dispose()}
