#pragma once

#include <cstdint>

namespace Poseidon::render
{
// A dormant IMAGE may be tolerated only by an already bounded parked model.
// This says nothing about source file freshness or exclusive geometry ownership.
struct ParkedRefillContext
{
    bool enabled = false;
    bool parked = false;
    bool live = false;
    uint32_t references = 0;
};

struct ParkedImageBinding
{
    uint64_t capturedHandle = 0;
    uint32_t capturedLease = 0;
    uint64_t currentHandle = 0;
    uint32_t currentLease = 0;
    bool texturePresent = false;
    bool dynamic = false;
    bool reloadableSource = false;
};

inline bool CanRefillParkedImageActivation(const ParkedRefillContext& context, const ParkedImageBinding& image)
{
    return context.enabled && context.parked && context.references == 0 &&
           image.texturePresent && !image.dynamic && image.reloadableSource &&
           image.capturedHandle != 0 && image.currentHandle == 0 && image.capturedLease != 0 &&
           image.capturedLease == image.currentLease;
}

inline bool CanDeferParkedImage(const ParkedRefillContext& context, const ParkedImageBinding& image)
{
    return !context.live && CanRefillParkedImageActivation(context, image);
}

// The upload's return value alone is insufficient: it must have published a
// nonzero image into the ORIGINAL material slot before activation can reuse it.
inline bool RefilledParkedImageMatches(const ParkedImageBinding& image, uint64_t uploadedHandle,
                                      uint64_t publishedHandle, uint32_t publishedLease)
{
    return uploadedHandle != 0 && uploadedHandle == publishedHandle && image.capturedHandle != 0 &&
           image.capturedLease != 0 && image.capturedLease == publishedLease;
}
}
