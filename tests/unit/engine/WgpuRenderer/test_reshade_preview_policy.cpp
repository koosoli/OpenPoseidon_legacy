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
}
