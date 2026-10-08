param([string]$CacheDirectory=(Join-Path $env:LOCALAPPDATA 'OpenPoseidon/world-stream'),
 [ValidateSet('Cell','District','Context')][string]$Region='Cell')
$ErrorActionPreference='Stop'
$directory=[IO.Path]::GetFullPath((Join-Path $CacheDirectory 'surface'))
$repo=[IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
if($directory.StartsWith($repo+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Geographic data must remain outside the repository'}
$south=49.60;$west=6.12;$north=49.62;$east=6.14;$size=2048;$name='lux-ortho-2023'
if($Region-eq'District'){$south=49.52;$west=6.00;$north=49.70;$east=6.26;$size=4096;$name='lux-district-2023'}
if($Region-eq'Context'){$south=49.40;$west=5.70;$north=50.20;$east=6.60;$size=4096;$name='lux-context-2023'}
$image=Join-Path $directory ($name+'.jpg');$receipt=Join-Path $directory ($name+'.json')
if((Test-Path -LiteralPath $image) -or (Test-Path -LiteralPath $receipt)){
 if(!(Test-Path -LiteralPath $image) -or !(Test-Path -LiteralPath $receipt)){throw 'Incomplete existing cache; inspect it before preparing again'}
 $existing=Get-Content -Raw -LiteralPath $receipt|ConvertFrom-Json
 if((Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash.ToLowerInvariant() -ne $existing.sha256){throw 'Existing cache SHA mismatch; inspect it before preparing again'}
 Write-Output "Using existing prepared $Region image: $image";return
}
# One manual WMS request, no retries, no background/worldwide imagery download.
# WMS 1.3 EPSG:4326 axis order is latitude,longitude; row zero is north.
$bounds=(@($south,$west,$north,$east)|ForEach-Object{$_.ToString('R',[Globalization.CultureInfo]::InvariantCulture)})-join'%2C'
$url="https://wmts1.geoportail.lu/opendata/service?SERVICE=WMS&VERSION=1.3.0&REQUEST=GetMap&LAYERS=ortho_2023&STYLES=&CRS=EPSG%3A4326&BBOX=$bounds&WIDTH=$size&HEIGHT=$size&FORMAT=image%2Fjpeg"
$max=16*1024*1024
Add-Type -AssemblyName System.Net.Http
$client=[Net.Http.HttpClient]::new();$client.Timeout=[TimeSpan]::FromSeconds(60)
$client.DefaultRequestHeaders.UserAgent.ParseAdd('OpenPoseidon-local-surface-prototype/1.0')
$response=$null;$stream=$null;$memory=[IO.MemoryStream]::new()
try{
 $response=$client.GetAsync($url,[Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult()
 $response.EnsureSuccessStatusCode()|Out-Null
 if($response.Content.Headers.ContentType.MediaType-ne'image/jpeg'){throw 'WMS did not return JPEG'}
 if($response.Content.Headers.ContentLength-gt$max){throw 'WMS compressed image exceeds 16 MiB'}
 $stream=$response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
 $buffer=[byte[]]::new(65536);$deadline=[DateTime]::UtcNow.AddSeconds(60)
 $cancel=[Threading.CancellationTokenSource]::new([TimeSpan]::FromSeconds(60))
 try{
  while(($n=$stream.ReadAsync($buffer,0,$buffer.Length,$cancel.Token).GetAwaiter().GetResult())-gt0){
   if([DateTime]::UtcNow-gt$deadline -or $memory.Length+$n-gt$max){throw 'WMS body exceeds time/byte bound'}
   $memory.Write($buffer,0,$n)
  }
 }finally{$cancel.Dispose()}
 $bytes=$memory.ToArray()
 if($bytes.Length-lt4 -or $bytes[0]-ne255 -or $bytes[1]-ne216){throw 'Invalid JPEG signature'}
 # Inspect JPEG SOF dimensions before permitting any pixel decoder allocation.
 $offset=2;$width=0;$height=0
 while($offset+3-lt$bytes.Length){
  if($bytes[$offset]-ne255){throw 'Invalid JPEG marker'}
  while($bytes[$offset]-eq255){$offset++}
  $marker=$bytes[$offset];$offset++
  if($marker-eq217 -or $marker-eq218){break}
  if($marker-eq1 -or ($marker-ge208 -and $marker-le215)){continue}
  $length=256*[int]$bytes[$offset]+[int]$bytes[$offset+1]
  if($length-lt2 -or $offset+$length-gt$bytes.Length){throw 'Invalid JPEG segment'}
  if($marker-ge192 -and $marker-le207 -and $marker-notin@(196,200,204)){
   if($length-lt7){throw 'Invalid JPEG frame'}
   $height=256*[int]$bytes[$offset+3]+[int]$bytes[$offset+4]
   $width=256*[int]$bytes[$offset+5]+[int]$bytes[$offset+6];break
  }
  $offset+=$length
 }
 if($width-ne$size -or $height-ne$size){throw "Unexpected JPEG dimensions: ${width}x${height}"}
 New-Item -ItemType Directory -Force -Path $directory|Out-Null
 $temporary=$image+'.partial';[IO.File]::WriteAllBytes($temporary,$bytes)
 $hash=(Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash.ToLowerInvariant()
 $manifest=[ordered]@{
  provider='Administration du cadastre et de la topographie (ACT)';licence='CC0-1.0';layer='ortho_2023';year=2023
  source='https://data.public.lu/fr/datasets/orthophoto-officielle-du-grand-duche-de-luxembourg-edition-2023/'
  licenceSource='https://data.public.lu/fr/datasets/bd-l-ortho-webservices-wms-et-wmts/'
  request=$url;crs='EPSG:4326';axisOrder='south latitude,west longitude,north latitude,east longitude';rowZero='north'
  south=$south;west=$west;north=$north;east=$east;width=$size;height=$size;bytes=$bytes.Length;sha256=$hash;region=$Region
  preparedUtc=[DateTime]::UtcNow.ToString('o')
 }
 $manifest|ConvertTo-Json|Set-Content -LiteralPath ($receipt+'.partial') -Encoding utf8
 Move-Item -LiteralPath $temporary -Destination $image
 Move-Item -LiteralPath ($receipt+'.partial') -Destination $receipt
 Write-Output "Prepared one CC0 ACT 2023 $Region image: $image ($($bytes.Length) bytes; SHA256 $hash)"
}finally{if($stream){$stream.Dispose()};if($response){$response.Dispose()};$memory.Dispose();$client.Dispose()}
