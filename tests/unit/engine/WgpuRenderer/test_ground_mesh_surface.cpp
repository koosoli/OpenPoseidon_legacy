#include "../../../../engine/WgpuRenderer/GroundMeshSurface.hpp"
#include "../../../../engine/WgpuRenderer/include/wgpu_renderer.hpp"
#include <WgpuRenderer/RetailPageInstanceObservation.hpp>
#include <WgpuRenderer/RetailWorldInstanceTransaction.hpp>
#include <WgpuRenderer/RetailWorldStableFrameObservation.hpp>
#include <cassert>
#include <initializer_list>

using Tx = Poseidon::render::RetailWorldInstanceTransaction;
using Stable = Poseidon::render::RetailWorldStableFrameObservation;
using Page = Poseidon::render::RetailPageInstanceObservation;
static Tx::RowExpectation Row(unsigned model, unsigned flags)
{
    Tx::RowExpectation r; r.model = {model,model+10,model+20};
    r.row.model=model; r.row.flags=flags; r.row.center={1200,15,1240,1};
    r.row.world.m[0]=r.row.world.m[5]=r.row.world.m[10]=r.row.world.m[15]=1;
    r.row.world.m[12]=1200;r.row.world.m[13]=15;r.row.world.m[14]=1240;
    return r;
}

int main()
{
    using Poseidon::GroundMeshSurface::OwnerAllowed;
    assert(OwnerAllowed(true,true,true,false,false,false,false));
    assert(OwnerAllowed(true,true,false,true,false,false,false));
    assert(!OwnerAllowed(false,true,true,false,false,false,false));
    assert(!OwnerAllowed(true,false,true,false,false,false,false));
    assert(!OwnerAllowed(true,true,false,false,false,false,false)); // parked vehicle/proxy
    assert(!OwnerAllowed(true,true,true,true,true,false,false));
    assert(!OwnerAllowed(true,true,true,false,false,true,false));
    assert(!OwnerAllowed(true,true,false,true,false,false,true));
    using Poseidon::GroundMeshSurface::SectionAllowed;
    assert(SectionAllowed(true,false,false));
    assert(!SectionAllowed(false,false,false));
    assert(!SectionAllowed(true,true,false));
    assert(!SectionAllowed(true,false,true));
    static_assert(WGR_INSTANCE_SURFACE_RECEIVERS == (128u | 256u));
    static_assert((WGR_INSTANCE_SURFACE_RECEIVERS & (32u | 64u)) == 0);
    // Benign surface tags survive page hide and replacement; unrelated flags do not.
    for (unsigned tags : {0u,128u,256u,384u}) {
        const unsigned page = (tags & WGR_INSTANCE_SURFACE_RECEIVERS) | 32u;
        const unsigned hidden = (tags & WGR_INSTANCE_SURFACE_RECEIVERS) | 64u;
        assert((page & ~WGR_INSTANCE_SURFACE_RECEIVERS) == 32u);
        assert((hidden & ~WGR_INSTANCE_SURFACE_RECEIVERS) == 64u);
        assert((page & WGR_INSTANCE_SURFACE_RECEIVERS) == (hidden & WGR_INSTANCE_SURFACE_RECEIVERS));
        assert(((page | 8u) & ~WGR_INSTANCE_SURFACE_RECEIVERS) != 32u);
        // Exercise the ACTUAL receipt admission/equivalence helpers, not a mask mirror.
        for (auto action : {Tx::Action::Takeover,Tx::Action::Restore,Tx::Action::UpdatePage}) {
            Tx::Request q; q.binding={7,8,9,10,11,12,13,14,15,16,17}; q.action=action;
            q.originalRendererHandle=0x03000001; q.previousCameraGeneration=19;
            q.originalBefore=Row(20,tags | (action==Tx::Action::Takeover?0u:64u));
            q.originalAfter=Row(20,tags | (action==Tx::Action::Restore?0u:64u));
            if(action!=Tx::Action::Takeover) {q.pageBefore=Row(21,page);q.pageRendererHandle=0x05000002;}
            if(action!=Tx::Action::Restore) q.pageAfter=Row(action==Tx::Action::UpdatePage?22:21,page);
            assert(Tx(q).Valid());
            auto bad=q; bad.originalAfter.row.flags ^= WGR_INSTANCE_GROUND_RECEIVER;
            assert(!Tx(bad).Valid());
            bad=q; bad.originalBefore.row.flags |= WGR_INSTANCE_COHERENT_TREE_WIND;
            assert(!Tx(bad).Valid());
        }
        Stable::Request q; q.binding={7,8,9,10,11,12,13,14,15,16,17};
        q.original=Row(20,hidden);q.page=Row(21,page);q.previousCameraGeneration=19;
        q.originalRendererHandle=0x03000001;q.pageRendererHandle=0x05000002;
        assert(Stable(q).Valid());
        q.page.row.flags ^= WGR_INSTANCE_GROUND_RECEIVER;assert(!Stable(q).Valid());
        Page::Request p;p.binding={7,8,9,10,11,12,13};p.expected=Row(13,page).row;
        p.previousCameraGeneration=19;assert(Page(p).Valid());
        p.expected.flags |= WGR_INSTANCE_CANOPY_BUSH;assert(!Page(p).Valid());
    }
}
