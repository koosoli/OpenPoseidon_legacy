#pragma once
#include "include/wgpu_renderer.hpp"
#include "../Poseidon/World/Weather/RainWaterField.hpp"
#include <utility>

namespace Poseidon {
inline bool SameRainWaterSource(const WgrRainWaterSourceKey& a,const WgrRainWaterSourceKey& b) {
    return a.world_token==b.world_token&&a.generation==b.generation&&a.height_revision==b.height_revision&&
        a.terrain_range==b.terrain_range&&a.terrain_spacing==b.terrain_spacing;
}
inline bool SameRainWaterPublication(const WgrRainWaterPublication& a,const WgrRainWaterPublication& b) {
    // Time and current liquid rain may change without replacing physical state.
    return SameRainWaterSource(a.source,b.source)&&a.revision==b.revision&&a.flags==b.flags&&
        a.reserved==b.reserved&&a.coarse.generation==b.coarse.generation&&a.coarse.reserved==b.coarse.reserved&&
        a.coarse.domain.x==b.coarse.domain.x&&a.coarse.domain.y==b.coarse.domain.y&&a.coarse.domain.z==b.coarse.domain.z&&
        a.coarse.control.x==b.coarse.control.x&&a.coarse.control.y==b.coarse.control.y&&a.coarse.control.w==b.coarse.control.w;
}
inline bool PackRainWaterFine(const RainWaterField::FinePublication& source,size_t parentCount,
    std::vector<WgrRainWaterFineCell>& result) {
    result.clear();if(source.surfaces.size()>WGR_RAIN_WATER_FINE_MAX_CELLS)return false;
    result.reserve(source.surfaces.size());
    for(const auto& s:source.surfaces) {
        const auto& o=s.owner;const auto& g=s.geometry;
        WgrRainWaterFineCell cell{{float(o.x),float(o.z),float(o.size),float(o.Head())},
            {g.Corner00(),g.Corner10(),g.Corner01(),g.Corner11()},
            {o.flowX,o.flowZ,float(o.Depth()),0},s.parent,{0,0,0}};
        const auto finite=[](const WgrVec4& v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z)&&std::isfinite(v.w);};
        if(s.parent>=parentCount||o.size!=6.25||!g.Finite()||!std::isfinite(o.volume)||o.volume<0||
           !finite(cell.rect)||!finite(cell.corners)||!finite(cell.flow_depth)||cell.flow_depth.z<0) {
            result.clear();return false;
        }
        result.push_back(cell);
    }
    return true;
}
inline void AccumulateRainWaterPublication(WgrRainWaterPublication& pending,std::vector<WgrVec4>& pendingCoarse,
    std::vector<WgrRainWaterFineCell>& pendingFine,const WgrRainWaterPublication& incoming,
    std::vector<WgrVec4> coarse,std::vector<WgrRainWaterFineCell> fine) {
    // Never combine fresh fine ownership with old coarse bytes (or vice versa).
    // A parameter-only camera preserves both slices only for the identical key.
    if(!SameRainWaterPublication(pending,incoming)||!coarse.empty()||!fine.empty()||
       incoming.coarse.control.w<.5f||!(incoming.flags&WGR_RAIN_WATER_SOURCE_READY)) {
        pendingCoarse=std::move(coarse);pendingFine=std::move(fine);
    }
    pending=incoming;
}
} // namespace Poseidon
