# AST-012C — official Binarize differential-fixture protocol

**Status:** active discovery protocol; no new ODOL layout is claimed.

## Purpose

Later ODOL fields must not be inferred from one retail asset or by applying the
revision-73 layout to a different revision.  For every new later-ODOL feature,
use an editable official Arma 3 Samples MLOD and its output from BI Binarize as
a controlled source/binary pair.  Compare the resulting common Model IR
semantically, not byte-for-byte: LODs, geometry, UV channels, sections,
selections, proxies, materials, skeleton data, animation metadata, bounds and
the tangent frame are separate claims.

Neither the Samples inputs nor Binarize outputs are repository artefacts.  They
remain on the owner's `P:` work drive and are identified only by paths and
hashes below.  This also prevents accidental publication of either Arma sample
content or retail game assets.

## First generated fixture

| Item | Value |
|---|---|
| Official source | `Arma 3 Samples/Addons/Test_scope3D_01/Test_scope3D_01.p3d` |
| Isolated working copy | `P:\\Samples_F\\Test_scope3D_01\\Test_scope3D_01.p3d` |
| Source SHA-256 / bytes | `a22f4278dcc6bc3d9fae8c71c95f4f715ec69256d1248b7f1e42022c0eccf9f9` / 302,209 |
| BI Binarize | `2.21.0.152929` |
| Generated output | `P:\\poseidon-diff\\odol\\Test_scope3D_01.p3d` |
| Output signature / revision | `ODOL` / 75 |
| Output SHA-256 / bytes | `0c3c81997ddaa047c13d6b5535847163991ea08142928372182fb1434931e8b9` / 61,130 |

The source material deliberately contains a neutral procedural NOHQ Stage 1
normal map (`color(0.5,0.5,1,1,NOHQ)`).  It is therefore a useful candidate
for separating normal-map channel decoding from tangent-frame errors once the
project-root mapping is repeatably resolving its material references.

## Reproduction

Run only against an isolated copy, never the installed Samples directory:

```powershell
& 'D:\SteamLibrary\steamapps\common\Arma 3 Tools\Binarize\binarize.exe' `
  -silent -always `
  '-binpath=D:\SteamLibrary\steamapps\common\Arma 3 Tools\Binarize' `
  '-addon=P:\Samples_F\Test_scope3D_01' `
  'P:\Samples_F\Test_scope3D_01' `
  'P:\poseidon-diff\odol' `
  'Test_scope3D_01.p3d'
```

Run that command with the current directory set to `P:\\`.  Binarize resolves
virtual material and texture paths relative to its current directory; launching
it from the repository produced a smaller, material-unresolved shell and is not
valid evidence.  `-binpath` must name the Binarize directory, not its `bin`
child: Binarize appends `\\bin` itself.  Binarize may derive PAA textures in the
isolated working copy; this is expected and must not touch the installed sample
tree.

## First material-only variant

The isolated working copy's `Data/nodiffuse.rvmat` was changed only at Stage 1
from neutral `color(0.5,0.5,1,1,NOHQ)` to
`color(1,0.5,1,1,NOHQ)`, binarized from `P:\\`, and then restored from a
byte-identical backup.  The output is `ODOL` revision 75, 61,128 bytes,
SHA-256 `28279ddb7ea44382331baf27f4d6c3b4905d79bdefdf36e44741e5ce05e1b516`.

It differs from the neutral output in 54,999 byte positions and is two bytes
shorter.  This is expected from repacked/compressed arrays after an embedded
material change, so it cannot identify a field by offsets alone.  It does prove
that BI Binarize responds to the isolated normal-map source change while all
geometry inputs remain identical.  Semantic decoding of revision 75 is the
next gate.

## Next evidence gate

Revision 75 is currently unsupported by the reader.  Before adding a parser or
feeding its packed ST values to rendering, produce at least two controlled
variants that differ in precisely one source feature, then show that the
candidate fields change consistently and that the resulting parser consumes
every declared range.  Initial variants should isolate UV count, named
selection and a non-neutral normal map.  A third-party parser may be used as an
independent comparison, but BI source plus BI Binarize are the primary oracle.
