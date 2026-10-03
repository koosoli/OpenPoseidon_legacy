# Pure read-only fixture payload receipt; no game connection or field mutation.
function Read-SandUploadSnapshot([string]$Path) {
 if (!('PoseidonSandSnapshot20261001' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
public static class PoseidonSandSnapshot20261001 {
 public static string Hash(byte[] data) {
  ulong hash=14695981039346656037UL;
  unchecked { foreach(byte value in data) {hash=(hash^value)*1099511628211UL;} }
  return hash.ToString("x16");
 }
 public static double[] Inspect(byte[] data) {
  if(!BitConverter.IsLittleEndian || data.Length!=1048592) throw new ArgumentException("Invalid little-endian sand payload size.");
  float[] header=new float[4];
  for(int i=0;i<4;i++) {
   header[i]=BitConverter.ToSingle(data,i*4);
   if(float.IsNaN(header[i]) || float.IsInfinity(header[i])) throw new ArgumentException("Nonfinite sand header.");
  }
  if(header[2]!=.125f || header[3]<0 || header[3]>.15f) throw new ArgumentException("Invalid sand header.");
  int negative=0,positive=0;float minimum=0,maximum=0;
  for(int i=4;i<data.Length/4;i++) {
   float value=BitConverter.ToSingle(data,i*4);
   if(float.IsNaN(value) || float.IsInfinity(value) || value < -header[3] || value>(header[3]>0?.02f:0f))
    throw new ArgumentException("Invalid signed sand cell.");
   if(value<0)negative++;if(value>0)positive++;
   minimum=Math.Min(minimum,value);maximum=Math.Max(maximum,value);
  }
  return new double[]{negative,positive,minimum,maximum};
 }
}
'@
 }
 $bytes=[IO.File]::ReadAllBytes($Path)
 $receipt=[PoseidonSandSnapshot20261001]::Inspect($bytes)
 $header=@(for($i=0;$i -lt 4;++$i){[BitConverter]::ToSingle($bytes,$i*4)})
 $sha=[Security.Cryptography.SHA256]::Create()
 try{$sha256=[BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-','')}finally{$sha.Dispose()}
 return @{bytes=$bytes.Length;count=($bytes.Length/4);header=$header;negative=[int]$receipt[0];positive=[int]$receipt[1];minimum=$receipt[2];maximum=$receipt[3];hash=[PoseidonSandSnapshot20261001]::Hash($bytes);sha256=$sha256}
}

function Assert-SandUploadPublication($snapshot,[string]$queued,[string]$received) {
 # The runtime shared-file reader splits on LF and preserves the Windows CR.
 # Normalize only record framing; all receipt fields remain strictly parsed.
 $queued=$queued.TrimEnd([char[]]"`r`n");$received=$received.TrimEnd([char[]]"`r`n")
 $number='[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?'
 $q='SAND_UPLOAD_QUEUED revision=\d+ enabled=(true|false) chunks=\d+ cameraX=('+ $number+') cameraZ=('+ $number+') originX=('+ $number+') originZ=('+ $number+') cell=('+ $number+') limit=('+ $number+') count=(\d+) finite=(true|false) negative=(\d+) positive=(\d+) min=('+ $number+') max=('+ $number+') minX=('+ $number+') minZ=('+ $number+') hash=([a-f0-9]{16})$'
 if($queued -cnotmatch $q){throw 'Malformed queued sand receipt.'}
 $qm=$Matches.Clone()
 $r='SAND_UPLOAD_RECEIVED accepted=(true|false) count=(\d+) header=\[('+ $number+'),\s*('+ $number+'),\s*('+ $number+'),\s*('+ $number+')\] negative=(\d+) positive=(\d+) min=('+ $number+') max=('+ $number+') hash=([a-f0-9]{16}) byteOffset=(\d+) bufferBytes=(\d+)$'
 if($received -cnotmatch $r){throw 'Malformed accepted renderer sand receipt.'}
 $rm=$Matches.Clone()
 if($qm[9] -cne 'true' -or $rm[1] -cne 'true' -or $qm[16] -cne $snapshot.hash -or $rm[11] -cne $snapshot.hash -or
    [int]$qm[8] -ne $snapshot.count -or [int]$rm[2] -ne $snapshot.count -or
    [int]$qm[10] -ne $snapshot.negative -or [int]$rm[7] -ne $snapshot.negative -or
    [int]$qm[11] -ne $snapshot.positive -or [int]$rm[8] -ne $snapshot.positive -or
    [long]$rm[12] -ne 2097184 -or [long]$rm[13] -ne 3145776){throw 'Sand queued/accepted/snapshot identity or allocation differs.'}
 $culture=[Globalization.CultureInfo]::InvariantCulture
 $fields=@(@($qm[4],$snapshot.header[0]),@($qm[5],$snapshot.header[1]),@($qm[6],$snapshot.header[2]),@($qm[7],$snapshot.header[3]),
  @($rm[3],$snapshot.header[0]),@($rm[4],$snapshot.header[1]),@($rm[5],$snapshot.header[2]),@($rm[6],$snapshot.header[3]),
  @($qm[12],$snapshot.minimum),@($qm[13],$snapshot.maximum),@($rm[9],$snapshot.minimum),@($rm[10],$snapshot.maximum))
 for($i=0;$i -lt $fields.Count;++$i){
  $field=$fields[$i]
  $value=[double]::Parse($field[0],$culture)
  # Header logs use5decimal places; extrema logs use8. Only their rounding
  # intervals are tolerated. Hash comparisons above remain exact.
  $tolerance=if($i -lt 8){.0000051}else{.000000006}
  if([double]::IsNaN($value)-or [double]::IsInfinity($value)-or [Math]::Abs($value-$field[1]) -gt $tolerance){throw 'Sand queued/accepted header or extrema differ from exact snapshot.'}
 }
 return @{queued=$queued;received=$received;scope='Exact source publication and renderer acceptance only; GPU completion and appearance unproved'}
}
