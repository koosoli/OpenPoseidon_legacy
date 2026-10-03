#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Poseidon
{
enum class RotorLandDrawOutcome : unsigned {Hidden,Shape,Near,Mip,DecalCall,Count};
// CPU-only observations, not backend acceptance or GPU/pixel visibility.
struct RotorLandDrawReceipt
{
    std::array<uint64_t,static_cast<unsigned>(RotorLandDrawOutcome::Count)> counts{};
    float alphaMin=0,alphaMax=0,halfXMin=0,halfXMax=0,halfYMin=0,halfYMax=0;
    float radiusMin=0,radiusMax=0,distanceMin=0,distanceMax=0;
    float ageMin=0,ageMax=0;
    uint64_t invalid=0;
    bool Record(RotorLandDrawOutcome outcome,float alpha=0,float halfX=0,float halfY=0,
                float radius=0,float distance=0,float age=0) {
        const auto i=static_cast<unsigned>(outcome);
        if(i>=counts.size())return false;
        const auto prior=counts[i]++;
        if(outcome!=RotorLandDrawOutcome::DecalCall)return true;
        if(!std::isfinite(alpha)||!std::isfinite(halfX)||!std::isfinite(halfY)||
           !std::isfinite(radius)||!std::isfinite(distance)||!std::isfinite(age)||alpha<0||alpha>1||
           halfX<=0||halfY<=0||radius<=0||distance<0||age<0) {++invalid;return false;}
        const auto range=[first=prior==invalid](float value,float& lo,float& hi) {
            if(first)lo=hi=value;
            else {lo=std::min(lo,value);hi=std::max(hi,value);}
        };
        range(alpha,alphaMin,alphaMax);range(halfX,halfXMin,halfXMax);range(halfY,halfYMin,halfYMax);
        range(radius,radiusMin,radiusMax);range(distance,distanceMin,distanceMax);
        range(age,ageMin,ageMax);
        return true;
    }
    uint64_t Calls() const {uint64_t n=0;for(const auto count:counts)n+=count;return n;}
};
}
