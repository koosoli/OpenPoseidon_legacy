#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// Additive private ABI. Existing renderer/header/ABI18 layouts are untouched.
// Requires exclusive renderer-owner access in the same frame order as existing
// getters. Default OFF, gated by WGR_GEOMETRY_PAGE_DEMAND_VIEWS=1 AND FIXTURE=1.
// CPU camera provenance only: no COUNT, cached-image publication, visibility,
// pixels, source/mount freshness, GPU completion or retirement authorization.
struct WgrRenderer;
inline constexpr uint32_t WgrDemandViewSnapshotVersion=1;
inline constexpr uint32_t WgrDemandViewSnapshotBytes=8096;
inline constexpr uint32_t WgrDemandViewSnapshotRows=40;
struct WgrDemandViewRow {
    uint32_t id=0,kind=0,status=0,provenance=0;
    uint64_t generation=0,matrix_generation=0;
    uint32_t viewport_width=0,viewport_height=0,viewport_origin_x=0,viewport_origin_y=0;
    std::array<float,4> origin{};
    std::array<float,16> projection{},view{};
    uint32_t flags=0,reserved=0;
};
struct WgrDemandViewSnapshot {
    uint32_t version=1,struct_bytes=8096,status=0,view_count=36;
    uint64_t generation=0,instance_epoch=0,required=0,known=0,unknown=0;
    uint64_t target_token=0,source_generation=0,target_revision=0;
    uint32_t model_id=UINT32_MAX,source_status=0,main_camera_index=UINT32_MAX,main_camera_source=0;
    std::array<WgrDemandViewRow,WgrDemandViewSnapshotRows> rows{};
};
static_assert(sizeof(WgrDemandViewRow)==200);
static_assert(offsetof(WgrDemandViewRow,origin)==48);
static_assert(offsetof(WgrDemandViewRow,projection)==64);
static_assert(offsetof(WgrDemandViewRow,view)==128);
static_assert(sizeof(WgrDemandViewSnapshot)==WgrDemandViewSnapshotBytes);
static_assert(offsetof(WgrDemandViewSnapshot,rows)==96);

// Snapshot status1 all enabled numeric tuples known,2 at least one Unknown,
// 3 invalid configured set/epoch. Source status1 is only an exact existing COUNT
// request identity tag (not COUNT success);0 means no such optional binding.
// Owner must independently join exact hierarchy/source/package/world/model
// admission and frame association before feeding projected demand.
// Row status1 known,2 nonfinite,3 viewport invalid,4 camera missing,5 actual
// target/matrix missing,6 duplicate. Row kinds1 main,2 solar,3 local,4 interior,
// 5 GI,6 reflection. IDs map main1,solar2..5,local6..29,interior30..34,GI35,
// reflection36;37..40 reserved. Flags1 nonjittered,2 retained matrix,4 planned
// matrix. Matrix generation names CPU capture, never cached image publication.
// Solar/local/sky/GI projection contains combined VP and view is identity;
// model must subtract that row's origin, not the current main-camera position.
// Getter0 disabled/no completed attempt,1 copied,2 invalid args/panic. Output
// untouched on0/2. Successful render-return publication is not GPU completion.
#if defined(_WIN32) && !defined(WGR_STATIC)
#define WGR_DEMAND_VIEW_API __declspec(dllimport)
#else
#define WGR_DEMAND_VIEW_API
#endif
extern "C" WGR_DEMAND_VIEW_API uint32_t wgr_demand_view_snapshot(WgrRenderer* renderer,
    WgrDemandViewSnapshot* output,uint32_t bytes,uint32_t version);
#undef WGR_DEMAND_VIEW_API
