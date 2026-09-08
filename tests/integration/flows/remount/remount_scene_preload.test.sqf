// Adapted from upstream a61130ca: scene-owned effects must survive remount.
triSetLanguage "English"
triSimUntil { triGameMode == 2 }
triSimUntil { triScenePreloadCount > 0 }
_before = triScenePreloadCount
_r = triRemount
triAssertEq [_r, "OK"]
triSimFrames 60
triSimUntil { triGameMode == 2 && triLoadedShapeCount > 0 }
triAssertEq [triScenePreloadCount, _before]
triEndTest
