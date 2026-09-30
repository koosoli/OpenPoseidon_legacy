[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
if (!('TerrainPuddleMetrics' -as [type])) {
    $drawingRefs = @([Drawing.Bitmap].Assembly.Location,[Drawing.Color].Assembly.Location)
    $drawingRefs += @([Drawing.Bitmap].Assembly.GetReferencedAssemblies() |
        Where-Object Name -like 'System.Private.Windows.*' | ForEach-Object { [Reflection.Assembly]::Load($_).Location })
    Add-Type -Path (Join-Path $PSScriptRoot 'TerrainPuddleMetrics.cs') -ReferencedAssemblies $drawingRefs
}
function Assert($condition,[string]$message) { if (!$condition) { throw $message } }
$images = @()
try {
    for ($i=0; $i -lt 4; ++$i) { $images += [Drawing.Bitmap]::new(10,10) }
    $dryA,$wetA,$dryB,$wetB = $images
    # Central ROI is x/y [2,8), 36 pixels. Ignore a changed UI pixel outside it.
    $wetA.SetPixel(0,0,[Drawing.Color]::White)
    $wetA.SetPixel(3,3,[Drawing.Color]::FromArgb(8,0,0))
    $wetA.SetPixel(4,3,[Drawing.Color]::FromArgb(7,7,7))
    $wetB.SetPixel(3,3,[Drawing.Color]::FromArgb(9,0,0))
    $pair = [TerrainPuddleMetrics]::CompareBitmaps($dryA,$wetA,8)
    Assert ($pair.Pixels -eq 36 -and $pair.ChangedPixels -eq 1 -and $pair.MaxChannel -eq 8) 'ROI or inclusive threshold wrong.'
    Assert ([Math]::Abs($pair.MeanAbsChannel-(29.0/108)) -lt 1e-12) 'RGB mean incorrectly includes outside ROI or uses max-channel mean.'
    $mask = [TerrainPuddleMetrics]::MaskOverlapBitmaps($dryA,$wetA,$dryB,$wetB,8)
    Assert ($mask.NonEmpty -and $mask.IoU -eq 1 -and $mask.AgreementFraction -eq 1) 'Stable causal mask failed.'
    $wetB.SetPixel(3,3,[Drawing.Color]::Black)
    $wetB.SetPixel(6,6,[Drawing.Color]::White)
    $mask = [TerrainPuddleMetrics]::MaskOverlapBitmaps($dryA,$wetA,$dryB,$wetB,8)
    Assert ($mask.Union -eq 2 -and $mask.Intersection -eq 0 -and $mask.IoU -eq 0) 'Relocated puddle mask falsely stable.'
    Assert ([Math]::Abs($mask.AgreementFraction-(34.0/36)) -lt 1e-12) 'Agreement mismatch.'
    $empty = [TerrainPuddleMetrics]::MaskOverlapBitmaps($dryA,$dryA,$dryB,$dryB,8)
    Assert (!$empty.NonEmpty -and $empty.IoU -eq 0) 'Empty response must not pass stable puddle acceptance.'
    $wrong = [Drawing.Bitmap]::new(11,10)
    try {
        $rejected = $false
        try { $null = [TerrainPuddleMetrics]::CompareBitmaps($dryA,$wrong,8) } catch { $rejected = $true }
        Assert $rejected 'Mismatched capture dimensions accepted.'
    } finally { $wrong.Dispose() }
    $rejected = $false
    try { $null = [TerrainPuddleMetrics]::CompareBitmaps($dryA,$wetA,0) } catch { $rejected = $true }
    Assert $rejected 'Zero difference threshold accepted.'
    Write-Host 'Terrain puddle metrics: ROI, threshold, RGB mean, stable/relocated/empty masks, dimensions and invalid threshold passed.'
} finally { foreach ($image in $images) { $image.Dispose() } }

