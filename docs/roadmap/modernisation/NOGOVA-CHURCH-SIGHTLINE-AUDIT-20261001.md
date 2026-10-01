# Nogova church sightline and foundation audit, 1 October 2026

The reported floating church remains unidentified. This additional source audit
does not justify a building offset or placement change. The complete retail
church and chapel census supplies actual owner IDs and reproducible candidate
views for installed inspection.

## Coordinate and identity proof

`triFreeFlyPose "2592.94 5422.69 75.80 100 6.2"` calls
`DebugOverlay::SetFreeFlyTestPose`, `ParseZeusGoto`, and `GoToZeusCoordinates`.
The first three values are X, Z and **absolute** Y, not terrain clearance. Yaw
zero points along +Z; yaw 100 points mostly +X and slightly -Z. Positive
elevation looks above the horizon. Its world direction is
`(sin(yaw)*cos(el), sin(el), cos(yaw)*cos(el))`. This order matches the prior
root captures and the freefly readout; swapping the coordinates would inspect
a different location.

The retail Noe source has 177224 placements. RVW4 export writes **ordinal `i`**
as its ID; it does not preserve the OPRW ID. This matters on Noe: exporter
ordinal 90179 is actual owner 1328, and ordinal 73821 is owner 43298.
The new audit verifies every exported 48-byte frame against the entire
177224-record original OPRW tail, beginning at byte 455222, with its exact
negative terminator. It recovers actual IDs and verifies the consistent
original name-index/model mapping. Root's earlier installed probes for 1328
and 43298 independently confirm this identity and frame mapping.

There are 34 placements across five exact church/chapel/ruin models. Including
chapels corrects the ambiguous statement that the nearest religious building
is four kilometres away: the nearest chapel is 1595.70 m away, but yaw 22.14
degrees is 77.86 degrees left of the reported heading. The nearest full
`kostel` is 2749.46 m away at yaw 139.60 degrees, 39.60 degrees right of it.
The two principal churches close to the reported ray remain 43298 and 1328.

## Source-qualified candidate views

All positions below are **X,Z,Y**, unlike the matrices in the JSON, whose
translation is X,Y,Z. Predicted elevations apply the existing legacy
`ObjectCreateFromShape` correction using the exact model BoundingCenter and
the original 50 m float height grid. Installed root probes already agree for
1328 and 43298 to within 0.00002 m. Other candidates still require their
actual runtime frame and terrain query.

| Actual ID | Exact model | Predicted X,Z,Y | Distance; top yaw/elevation | Near candidate freefly X,Z,Y,yaw,el |
| --- | --- | --- | --- | --- |
| 107268 | `o\\misc\\kapl.p3d` | 3194.284,6900.741,124.663 | 1595.70 m; 22.14/1.79 | 3134.284 6900.741 120.282 90 6.026 |
| 158202 | `data3d\\kostel.p3d` | 4374.860,3328.826,27.658 | 2749.46 m; 139.60/-0.81 | 4314.860 3328.826 19.304 90 2.042 |
| 43298 | `data3d\\kostel.p3d` | 6530.781,4221.033,117.163 | 4117.11 m; 106.97/0.70 | 6470.781 4221.033 113.980 90 -2.893 |
| 1328 | `data3d\\kostel3.p3d` | 7362.711,4772.176,134.233 | 4813.93 m; 97.77/0.91 | 7302.711 4772.176 123.892 90 -4.293 |

From the original reported position, first target each actual source roof:

```text
2592.94 5422.69 75.80 106.96990 0.70384   # owner 43298
2592.94 5422.69 75.80 97.76624 0.90786    # owner 1328
```

The reported 6.2-degree elevation is approximately 5.3 to 5.5 degrees above
these model-top targets. No screen FOV or pixel identification is inferred.
Near poses use the source terrain plus four metres, then aim at the model's
bottom plus three metres. They are candidates, not guaranteed collision-free
camera positions; root should query actual terrain and identity first and
adjust only the camera if an intervening object blocks the view.

## Foundation and sightline results

The original ODOL7 points are already in engine-centred model space. The
adapter restores the model's stored BoundingCenter and original points;
adding that centre again to the mesh would mismeasure its foundation. The
audit applies the complete original basis to every visual LOD's vertices
within 10 cm of its minimum Y, then samples the original anti-diagonal
heightfield at each world X/Z. It applies the existing legacy lowering only
when the transformed negative BoundingCenter anchor is above terrain.

Owner 1328's bottom points are **1.948 to 2.382 m below** the original terrain;
43298's are **1.989 to 2.032 m below** it. Owner 158202 also has a buried
foundation. The nearest chapel's points range from -0.051 to +0.035 m. Across
all 34 candidates, bottom-point gaps range from -2.648 to +0.131 m. These are
source predictions; they do not prove that every installed church is drawn
correctly or that the owner's reported church is one of these models.

A five-metre-spaced source terrain ray towards each model's maximum visual
Y at its owner X/Z leaves 4.65 m clearance for 43298 and 17.33 m for 1328.
The closer 158202 ray crosses a source ridge by 3.86 m. Four small chapels
near the reported yaw (3937,16784,3927,3932) cross ridges by roughly
15 to 75 m and are poor identification targets from this camera. These
queries use the original 50 m grid, not the installed L3 subdivision,
CDLOD raster, object occluders or actual per-pixel roof surface. They are
bounded candidate selection evidence only.

## Reproduction and retained evidence

The checked-in JSON records all actual IDs, original frames, source hashes,
every visual foundation LOD, candidate poses, and coarse sightline maxima.
Run with the existing CPU exporter output:

```powershell
python scripts/Audit-NogovaChurchSightline.py `
  --export build/map-source-audit-final/noe.rvw4 `
  --wrp build/map-source-audit-final/noe/noe.wrp `
  --install 'D:/SteamLibrary/steamapps/common/ARMA Cold War Assault' `
  --output build/map-source-audit-final/noe-church-sightline.json
```

The 1 October source-only run passed all 177224 frame/tail checks and decoded
all five ODOL7 model types without a game or GPU launch. It uses the existing
strict stock PBO/ODOL decoder. Retail source hashes:

| Decoded source | SHA256 |
| --- | --- |
| `noe.wrp` | `9b27b11661d3cb30f9f0749a9a4616bfc58d723c4750666d1e674dd4213598d2` |
| `kostel.p3d` | `196c81981f2d9594a0a5b9642ccecdd626ca9879796494e5cb1d1398dc8eec7e` |
| `kostel3.p3d` | `14fb625d820383b5e32b1e45d563c4eeace8caaed44c2d699bd0a3f64af9484c` |
| `kostel_trosky.p3d` | `e04e7a7df49378de1a7fa7002765819b860858286489a92a6a894be4e29b667c` |
| `o\\hous\\kostelin.p3d` | `ac37542303f56aa60a4f328085148ed58bfc1fffb4d7abe00fcdb06891dec568` |
| `o\\misc\\kapl.p3d` | `ab190aabf63e2a2dff4082a8ad51e8707c09f5256bf18f70166a0e9db0659c22` |

Root's prior captures and installed probes are documented separately in
`docs/roadmap/decisions/NOGOVA-CHURCH-PLACEMENT-20261001.md`. I inspected those
two saved PNGs; no church could be reliably identified. The next useful root
gate is exact-owner near foundation plus targeted far views with the same
installed pair. Source burial and an unidentified screenshot are not grounds
to offset all buildings or close the user's floating-church report.
