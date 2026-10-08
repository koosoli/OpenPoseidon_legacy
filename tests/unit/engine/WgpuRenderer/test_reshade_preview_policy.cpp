#include "../../../../engine/Poseidon/Dev/Debug/ReShadePreviewPolicy.hpp"
#include <cassert>

int main()
{
    using OpReShadePreview::SupportsDepthSettings;
    assert(SupportsDepthSettings(true, 1600, 900, 1600, 900, 4));
    assert(SupportsDepthSettings(true, 3441, 1440, 3441, 1440, 1)); // resize, actual native targets
    assert(!SupportsDepthSettings(true, 1600, 900, 1600, 900, 0)); // legacy base getter is unknown
    assert(!SupportsDepthSettings(true, 1600, 900, 1600, 900, 2));
    assert(!SupportsDepthSettings(true, 1600, 900, 1600, 900, 8));
    assert(!SupportsDepthSettings(false, 1600, 900, 1600, 900, 4));
    assert(!SupportsDepthSettings(true, 1072, 603, 1600, 900, 4)); // scaling despite AUTO preference 1
    assert(!SupportsDepthSettings(true, 1600, 900, 1600, 901, 4)); // each dimension must match
    assert(!SupportsDepthSettings(true, 0, 0, 0, 0, 1)); // absent/unready renderer
    assert(!SupportsDepthSettings(true, -1, 900, -1, 900, 4));
    using namespace OpReShadePreview;
    for (unsigned flags : {0u, 1u, 3u, 5u, 7u})
    {
        assert(!FogCompatible(DhGi, flags)); // old/unknown/active fog fails closed
        assert(FogCompatible(DhDepth, flags)); // depth is independent of fog
        assert(FogCompatible(SceneDepth, flags));
    }
    for (unsigned flags : {8u, 9u, 11u, 13u, 15u}) assert(FogCompatible(DhGi, flags));
}
