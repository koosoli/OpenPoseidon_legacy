#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalCut.hpp>

namespace Poseidon::GeometryPages
{
// Pure object-space demand. One finite conservative threshold per baked group;
// projection/per-view policy must supply these values and their source identity.
// Activation is closed over *all* incoming parents of a shared refined group.
// Thus every parent cluster referencing an active child is replaced together.
struct LocalizedHierarchicalCutPlan
{
    HierarchicalCutPlan cut;
    std::vector<uint32_t> activeGroups;
};
struct LocalizedHierarchicalDemand
{
    HierarchicalIdentity identity;
    std::span<const float> groupThresholds;
};

inline bool PlanLocalizedHierarchicalCut(const HierarchicalPackage& package,
    const LocalizedHierarchicalDemand& demand,const HierarchicalResidentPages& resident,
    LocalizedHierarchicalCutPlan& destination)
{
    if(!ValidHierarchicalMetadata(package)||!(package.identity==demand.identity)||
        !(package.identity==resident.identity)||demand.groupThresholds.size()!=package.groups.size()||
        resident.pages.size()!=package.pages.size())return false;
    for(float threshold:demand.groupThresholds)
        if(!std::isfinite(threshold)||threshold<0||threshold==FLT_MAX)return false;
    for(auto bit:resident.pages)if(bit>1)return false;
    try {
        LocalizedHierarchicalCutPlan result;result.cut.identity=package.identity;
        std::array<uint8_t,64> active{},required{};
        // A group with error above its local allowance needs its own clusters.
        // Refined IDs always precede parent IDs, so this ascending pass closes
        // each active child over every incoming parent, transitively. It may
        // activate more coarse groups than requested, never fewer.
        for(uint32_t g=0;g<package.groups.size();++g) {
            if(package.groups[g].simplified.error>demand.groupThresholds[g])active[g]=1;
            if(!active[g])continue;
            for(const auto& cluster:package.clusters)
                if(cluster.refined==int(g))active[cluster.group]=1;
        }
        for(uint32_t g=0;g<package.groups.size();++g)
            if(active[g])result.activeGroups.push_back(g);
        // Root pages remain pinned even when the requested local frontier is
        // wholly resident. A missing root never grants a drawable selection.
        bool rootsReady=true;
        for(auto page:package.rootPages) {
            required[page]=1;result.cut.requiredPages.push_back(page);
            if(!resident.pages[page]) {rootsReady=false;result.cut.missingPages.push_back(page);}
        }
        for(uint32_t id=0;id<package.clusters.size();++id) {
            const auto& cluster=package.clusters[id];
            if(!active[cluster.group] || (cluster.refined>=0 && active[cluster.refined]))continue;
            result.cut.requestedClusters.push_back(id);
            if(!required[cluster.page]) {
                required[cluster.page]=1;result.cut.requiredPages.push_back(cluster.page);
                if(!resident.pages[cluster.page])result.cut.missingPages.push_back(cluster.page);
            }
        }
        if(result.cut.requestedClusters.empty())return false;
        if(rootsReady) {
            if(result.cut.missingPages.empty()) {
                result.cut.state=HierarchicalCutState::RequestedCut;
                result.cut.selectedClusters=result.cut.requestedClusters;
            } else {
                result.cut.state=HierarchicalCutState::RootFallback;
                result.cut.selectedClusters=package.rootClusters;
            }
        }
        destination=std::move(result);return true;
    }catch(const std::bad_alloc&){return false;}
}
}
