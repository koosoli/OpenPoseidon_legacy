#pragma once

#include <string>
#include <vector>

#include <Poseidon/Foundation/Types/Pointers.hpp>

namespace Poseidon
{
class Texture;
class TexMaterial;
} // namespace Poseidon

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

    // Everything convertToLODShape needs from the MAIN-THREAD-ONLY banks, resolved
    // ahead of the conversion. This is the Phase-6 split: resolving these tables is
    // <100 ms across a whole traverse (measured), while the conversion they feed is
    // 3+ s -- so the tables are built on the main thread and the conversion can move
    // to a worker. Indices are parallel to the IR: textures[lod][materialIndex],
    // surfMats[lod][sectionIndex].
    struct AdapterBankTables
    {
        std::vector<std::vector<Ref<Poseidon::Texture>>> textures;
        std::vector<std::vector<Ref<Poseidon::TexMaterial>>> surfMats;
    };

    // MAIN THREAD ONLY: touches the global texture bank and material bank, and loads
    // texture headers for non-ODOL models (the MLOD specials scan reads them).
    void BuildAdapterBankTables(const Model& model, AdapterBankTables& tables);

    // With `tables`, the conversion performs NO bank calls (worker-safe except for
    // proxy object creation, still done here). Without, it resolves the banks inline
    // exactly as it always has.
    LODShapeWithShadow* convertToLODShape(const Model& model, bool reversed = false,
                                          const AdapterBankTables* tables = nullptr);

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
