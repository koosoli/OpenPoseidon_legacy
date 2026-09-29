#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalPackage.hpp>

namespace Poseidon::GeometryPages
{
struct HierarchicalResidentPages
{
    HierarchicalIdentity identity;
    std::span<const uint8_t> pages;
};
enum class HierarchicalCutState { MissingRoots,RootFallback,RequestedCut };
struct HierarchicalCutPlan
{
    HierarchicalIdentity identity;
    HierarchicalCutState state=HierarchicalCutState::MissingRoots;
    std::vector<uint32_t> requestedClusters,requiredPages,missingPages,selectedClusters;
};
// Pure bounded CPU reference for an immutable Build-produced package. Threshold
// is object-space simplification error, not a certified raster-pixel bound.
// Root pages are mandatory pins even after refinement. Every selected cut is
// complete: individual ready children never partially replace their parents.
// This does not yet authorize spatial per-group mixed-threshold DAG refinement.
inline bool PlanHierarchicalCut(const HierarchicalPackage& package,float threshold,
    const HierarchicalResidentPages& resident,HierarchicalCutPlan& destination)
{
    if(!ValidHierarchicalMetadata(package)||!(package.identity==resident.identity)||resident.pages.size()!=package.pages.size()||
        !std::isfinite(threshold)||threshold<0||threshold==FLT_MAX)return false;
    for(auto bit:resident.pages)if(bit>1)return false;
    try {
        HierarchicalCutPlan result;result.identity=package.identity;
        std::array<uint8_t,64> required{};
        bool rootsReady=true;
        // Root-first order is also request priority. Required includes pinned
        // root bytes and desired page bytes exactly once, never placement counts.
        for(auto p:package.rootPages){required[p]=1;result.requiredPages.push_back(p);if(!resident.pages[p]){rootsReady=false;result.missingPages.push_back(p);}}
        for(uint32_t id=0;id<package.clusters.size();++id) {
            const auto& c=package.clusters[id];
            if(package.groups[c.group].simplified.error>threshold&&
                (c.refined<0||package.groups[c.refined].simplified.error<=threshold)) {
                result.requestedClusters.push_back(id);
                if(!required[c.page]){required[c.page]=1;result.requiredPages.push_back(c.page);if(!resident.pages[c.page])result.missingPages.push_back(c.page);}
            }
        }
        if(result.requestedClusters.empty())return false;
        if(rootsReady) {
            if(result.missingPages.empty()){result.state=HierarchicalCutState::RequestedCut;result.selectedClusters=result.requestedClusters;}
            else {result.state=HierarchicalCutState::RootFallback;result.selectedClusters=package.rootClusters;}
        }
        destination=std::move(result);return true;
    }catch(const std::bad_alloc&){return false;}
}
struct HierarchicalPageBatch
{
    std::vector<uint32_t> pages;
    uint64_t logicalBytes=0,uploadBytes=0;
};
// Produce independently schedulable page IDs under BOTH preparation and upload
// allowances. Owner supplies available bytes after live/staging/retired debt.
// In-flight pages consume their existing owner reservation and aren't duplicated.
// Returning an empty batch is an honest capacity stall, never an oversize bypass.
inline bool SelectHierarchicalPageBatch(const HierarchicalPackage& package,const HierarchicalCutPlan& plan,
    std::span<const uint8_t> inFlight,uint32_t pageCountLimit,uint64_t availableLogicalBytes,
    uint64_t availableUploadBytes,HierarchicalPageBatch& destination)
{
    if(!ValidHierarchicalMetadata(package)||!(plan.identity==package.identity)||inFlight.size()!=package.pages.size()||
        !pageCountLimit||pageCountLimit>64||plan.missingPages.size()>package.pages.size())return false;
    for(auto bit:inFlight)if(bit>1)return false;
    std::array<uint8_t,64> seen{};
    try {
        HierarchicalPageBatch result;
        for(auto p:plan.missingPages) {
            if(p>=package.pages.size()||seen[p])return false;seen[p]=1;
            const auto& page=package.pages[p];
            if(inFlight[p]||result.pages.size()==pageCountLimit||
                page.logicalBytes>availableLogicalBytes-result.logicalBytes||page.uploadBytes>availableUploadBytes-result.uploadBytes)continue;
            result.pages.push_back(p);result.logicalBytes+=page.logicalBytes;result.uploadBytes+=page.uploadBytes;
        }
        destination=std::move(result);return true;
    }catch(const std::bad_alloc&){return false;}
}
}
