// An in-process re-mount must drop the texture bank's loaded set. A bank that keeps it serves
// every texture by name from the previous mount, so a mod that replaces a file under the same
// path draws the old image. Under WGPU the base engine released a bank nothing loads into.

triSetLanguage "English"
triSimUntil { triGameMode == 2 && triLoadedShapeCount > 0 }
_before = triTextureBankReleases

_r = triRemount
triAssertEq [_r, "OK"]
triSimFrames 60
triSimUntil { triGameMode == 2 && triLoadedShapeCount > 0 }

triAssertEq [triTextureBankReleases, _before + 1]
triEndTest
