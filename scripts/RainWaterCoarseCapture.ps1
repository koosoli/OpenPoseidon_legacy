# Pure validation/readback outside the measured game window. Does not run a
# game, advance a clock, alter water or write a capture itself.
function Read-RainWaterCoarseCapture([string]$Path){
 $stream=[IO.File]::OpenRead($Path);$reader=[IO.BinaryReader]::new($stream)
 try{
  $magic=[Text.Encoding]::ASCII.GetString($reader.ReadBytes(8))
  if($magic -cne 'RWCAP001'){throw 'Unknown coarse capture format.'}
  $width=$reader.ReadUInt32();$height=$reader.ReadUInt32()
  if($width-lt2-or$height-lt2-or$width-gt513-or$height-gt513){throw 'Coarse capture exceeds admitted dimensions.'}
  $bytes=88+[int64]$width*$height*8
  if($stream.Length-ne$bytes-or$bytes-gt(88+[int64]513*513*8)){throw 'Coarse capture byte count is incomplete or unbounded.'}
  $header=[ordered]@{format=$magic;width=$width;height=$height;spacing=$reader.ReadSingle();originX=$reader.ReadSingle();originZ=$reader.ReadSingle();seaLevel=$reader.ReadSingle();pendingSeconds=$reader.ReadDouble();generation=$reader.ReadUInt64();revision=$reader.ReadUInt64();rainVolume=$reader.ReadDouble();infiltrationVolume=$reader.ReadDouble();evaporationVolume=$reader.ReadDouble();outletVolume=$reader.ReadDouble();headerBytes=$stream.Position;bytes=$bytes}
  if($header.headerBytes-ne88){throw 'Coarse capture header size changed.'}
  foreach($key in @('spacing','originX','originZ','seaLevel','pendingSeconds','rainVolume','infiltrationVolume','evaporationVolume','outletVolume')){if(![double]::IsFinite([double]$header[$key])){throw 'Nonfinite coarse capture header.'}}
  if($header.spacing-le0-or$header.pendingSeconds-lt0){throw 'Invalid coarse spacing/pending time.'}
  return $header
 }finally{$reader.Dispose();$stream.Dispose()}
}
function Assert-RainWaterCoarseCapture($Receipt,$Header,$Before,$After,[string]$ExpectedPath,[int64]$TimeMs){
 if($Receipt.readonly-isnot[bool]-or$Receipt.sourceCurrent-isnot[bool]-or$Before.fineActive-isnot[bool]-or$After.fineActive-isnot[bool]-or$Before.sourceReady-isnot[bool]-or$After.sourceReady-isnot[bool]){throw 'Coarse authority fields must be actual booleans.'}
 if($Receipt.readonly-ne$true-or$Receipt.sourceCurrent-ne$true-or$Receipt.format-cne'RWCAP001'-or$Receipt.floatEncoding-cne'ieee754-binary32-le'-or$Receipt.sourceWitness-cne'bit-exact-current-native-coarse-bed'){throw 'Coarse capture lacks exact native source authority.'}
 if([IO.Path]::GetFullPath($Receipt.path)-ine[IO.Path]::GetFullPath($ExpectedPath)){throw 'Coarse capture path differs from designated arm file.'}
 if($Receipt.timeScale-ne0-or$Receipt.timeMs-ne$TimeMs){throw 'Coarse capture did not preserve the paused clock.'}
 if($Receipt.world-isnot[string]-or!$Receipt.world-or$Receipt.worldToken-notmatch'^\d+$'-or$Receipt.sourceRevision-notmatch'^\d+$'){throw 'Coarse capture world/source identity absent.'}
 if($Before.fineActive-ne$false-or$After.fineActive-ne$false-or$Before.sourceReady-ne$true-or$After.sourceReady-ne$true){throw 'Coarse capture requires actual ready legacy field.'}
 foreach($key in @('width','height','spacing','volume','rainVolume','infiltrationVolume','evaporationVolume','outletVolume','pendingSeconds','generation','revision')){if($null-eq$Before.$key-or$Before.$key-ne$After.$key){throw ('Capture mutated actual field: '+$key)}}
 foreach($key in @('width','height','spacing')){if($Header[$key]-ne$Before.$key-or$Receipt.$key-ne$Before.$key){throw ('Coarse capture header/state differs: '+$key)}}
 if($Receipt.pendingSeconds-ne$Before.pendingSeconds){throw 'Coarse capture JSON pending receipt/state differs.'}
 # Native binary64 is authoritative. cJSON decimal numbers can lose one ULP;
 # demand exact canonical bits in all three responses instead of a tolerance.
 foreach($key in @('pendingSeconds','rainVolume','infiltrationVolume','evaporationVolume','outletVolume')){
  $bitKey=$key+'Bits';$native=[BitConverter]::DoubleToInt64Bits([double]$Header[$key]).ToString('X16',[Globalization.CultureInfo]::InvariantCulture)
  foreach($state in @($Receipt,$Before,$After)){
   if($state.$bitKey-isnot[string]-or$state.$bitKey-cnotmatch'^[0-9A-F]{16}$'-or$state.$bitKey-cne$native){throw ('Coarse capture exact binary64 differs: '+$key)}
  }
 }
 foreach($key in @('generation','revision')){if($Header[$key]-ne$Before.$key){throw ('Coarse capture exact identity differs: '+$key)}}
 if([uint64]::Parse($Receipt.generationExact)-ne$Header.generation-or[uint64]::Parse($Receipt.revisionExact)-ne$Header.revision){throw 'Coarse capture exact integer identities disagree.'}
 foreach($key in @('originX','originZ','seaLevel','headerBytes','bytes')){if($Receipt.$key-ne$Header[$key]){throw ('Coarse capture receipt/header differs: '+$key)}}
 if($Receipt.nativeBedBitsMatched-ne[double]$Header.width*$Header.height-or$Receipt.maximumRainMetresPerSecond-ne.000025-or$Receipt.stepSeconds-ne.25){throw 'Coarse capture native count/forcing policy differs.'}
}
