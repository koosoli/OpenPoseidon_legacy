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
    // ahead of the conversion. Tables retain main-thread bank ownership while
    // geometry conversion can move to a worker. Native Reforger texture composition
    // can make this step expensive; the original converted-world timing is not a
    // bound on this path. Indices are parallel to the IR: textures[lod][materialIndex],
    // surfMats[lod][sectionIndex].
    struct AdapterBankTables
    {
        std::vector<std::vector<Ref<Poseidon::Texture>>> textures;
        std::vector<std::vector<Ref<Poseidon::TexMaterial>>> surfMats;
    };

    // MAIN THREAD ONLY: touches the global texture bank and material bank, and loads
    // texture headers for non-ODOL models (the MLOD specials scan reads them).
    void BuildAdapterBankTables(const Model& model, AdapterBankTables& tables);

    // With `tables`, the conversion performs NO bank calls. Proxy creation stays
    // INLINE by default (it must run before the tail's OptimizeShapes, which
    // reorders skinning data) -- so a tables call is only worker-safe with
    // `proxiesInline=false`, and such a shape is INCOMPLETE (no proxies, and the
    // tail ran without them) until the tail split lands. That is why the async
    // adapt path is opt-in.
    // `finishTail=false` (worker-side conversion only): stop before the ODOL tail
    // (proxies + OptimizeShapes + everything ordered after them); the main-thread
    // install MUST then run FinishOdolAdapterTail before OptimizeOneShape.
    LODShapeWithShadow* convertToLODShape(const Model& model, bool reversed = false,
                                          const AdapterBankTables* tables = nullptr, bool finishTail = true);

    // MAIN THREAD ONLY. No-op for non-ODOL models (their tail has no proxies and
    // completes inside the conversion).
    void FinishOdolAdapterTail(LODShapeWithShadow* shape, const Model& model, bool reversed);

    // MAIN THREAD ONLY (config probe, file table, recursive ShapeBank::New).
    // The conversion runs this itself at the correct point (before its tail's
    // OptimizeShapes) unless proxiesInline=false was passed.
    void CreateAdapterProxies(LODShapeWithShadow* shape, const Model& model);

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
