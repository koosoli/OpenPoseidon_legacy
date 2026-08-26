#pragma once

#include <string>

namespace Poseidon { class LODShapeWithShadow; }
using Poseidon::LODShapeWithShadow;

namespace Poseidon {
namespace Model {

struct Model;

namespace ShapeAdapter
{
    struct ProxyModelName
    {
        std::string modelName;
        int id = -1;
    };

    ProxyModelName normalizeProxyModelName(const std::string& selectionName);
    LODShapeWithShadow* convertToLODShape(const Model& model, bool reversed = false);

    // MAT-053 -- the bridged-vehicle pilot seat. A bridged A1/A2/A3 vehicle's crew
    // proxies point at Arma-era placeholder models that exist neither as files nor
    // as config classes, so the adapter SKIPS them and no ManProxy is ever Present();
    // the interior camera then has no authored seat to stand on. The skipped proxy
    // RECORD still carries the authored seat transform, so the adapter remembers the
    // first pilot/driver seat per converted shape here and Transport::InsideCamera's
    // degraded path consults it. Registry rather than a LODShape field because the
    // shape's layout is serialized and shared; this is engine-session state.
    // Returns false when the shape never had a skipped crew seat.
    bool GetBridgedPilotSeat(const LODShapeWithShadow* shape, float out[3]);
}

} // namespace Model
} // namespace Poseidon
