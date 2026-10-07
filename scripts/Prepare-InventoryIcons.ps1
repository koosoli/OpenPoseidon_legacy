<# Explicit authoring step: preserve original generated PNGs; pack their alpha
   bounds into small power-of-two runtime textures, then encode prepared PAAs.
   Normal builds copy those versioned PAAs and do not need this Windows step. #>
[CmdletBinding()]
param([string]$Preset='win-x64-clang-rwdi')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$suffix=($Preset -split '-')[-1]
$tools=Join-Path $root "dist/x64-win-$suffix/PoseidonTools.exe"
$prompts=Get-Content (Join-Path $root 'content/inventory-icons/realistic-prompts.json') -Raw|ConvertFrom-Json
$out=Join-Path $root 'build/inventory-icons/packed'
$null=New-Item -ItemType Directory -Path $out -Force
Add-Type -AssemblyName System.Drawing.Common
if (!('InventoryIconPacking' -as [type])) {
    $drawingReferences=@([Drawing.Bitmap].Assembly.Location)
    $drawingReferences+=@([Drawing.Bitmap].Assembly.GetReferencedAssemblies() | ForEach-Object { [Reflection.Assembly]::Load($_).Location })
    Add-Type -ReferencedAssemblies $drawingReferences -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
public static class InventoryIconPacking {
    static int Pow2(int x) { int p=1; while(p<x) p*=2; return p; }
    public static int[] Pack(string source,string output) {
        using(var original=new Bitmap(source)) {
            int x0=original.Width,y0=original.Height,x1=-1,y1=-1;
            for(int y=0;y<original.Height;y++) for(int x=0;x<original.Width;x++)
                if(original.GetPixel(x,y).A>8) { x0=Math.Min(x0,x);y0=Math.Min(y0,y);x1=Math.Max(x1,x);y1=Math.Max(y1,y); }
            if(x1<x0) throw new Exception("Empty inventory icon alpha: "+source);
            x0=Math.Max(0,x0-12); y0=Math.Max(0,y0-12);
            x1=Math.Min(original.Width-1,x1+12);y1=Math.Min(original.Height-1,y1+12);
            int sw=x1-x0+1,sh=y1-y0+1;
            double scale=Math.Min(1.0,Math.Min(496.0/sw,496.0/sh));
            int dw=Math.Max(1,(int)Math.Round(sw*scale)),dh=Math.Max(1,(int)Math.Round(sh*scale));
            int w=Pow2(dw+16),h=Pow2(dh+16);
            using(var packed=new Bitmap(w,h,PixelFormat.Format32bppArgb)) {
                using(var graphics=Graphics.FromImage(packed)) {
                    graphics.Clear(Color.Transparent);
                    graphics.CompositingMode=CompositingMode.SourceCopy;
                    graphics.InterpolationMode=InterpolationMode.HighQualityBicubic;
                    graphics.PixelOffsetMode=PixelOffsetMode.HighQuality;
                    graphics.DrawImage(original,new Rectangle((w-dw)/2,(h-dh)/2,dw,dh),new Rectangle(x0,y0,sw,sh),GraphicsUnit.Pixel);
                }
                packed.Save(output,ImageFormat.Png);
            }
            return new int[]{w,h};
        }
    }
}
'@
}
$records=@()
foreach($icon in $prompts.icons) {
    $source=Join-Path $root $icon.png; $packed=Join-Path $out ($icon.category+'.png'); $paa=Join-Path $root $icon.paa
    $dimensions=[InventoryIconPacking]::Pack($source,$packed)
    & $tools image convert $packed $paa --format DXT5
    if($LASTEXITCODE){throw "Icon encoding failed: $($icon.category)"}
    $records+=@{category=$icon.category;source_png=$icon.png;source_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();runtime_paa=$icon.paa;runtime_sha256=(Get-FileHash $paa).Hash.ToLowerInvariant();runtime_dimensions=@($dimensions)}
}
@{version=2;artwork='Original AI-generated realistic category illustrations';tool='Built-in image_gen';image_inputs=@();license='GPL-3.0-or-later; project Section 7 terms';prompts='realistic-prompts.json';icons=$records}|ConvertTo-Json -Depth 8|Set-Content (Join-Path $root 'content/inventory-icons/manifest.json')
