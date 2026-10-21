// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// RFG-158 -- window panes of native Enfusion (Arma Reforger) buildings that break.
//
// A Reforger pane is a part hung off its window frame's socket bone (RFG-064) whose prefab
// derives from DestructibleGlass_Base.et: one hit point, an SCR_WindowHitZone, and a broken
// phase model (`m_PhaseModel`, `..._dst.xob`) that stays in the frame. The loader lists that
// broken model beside the intact one; this registry knows which streamed placements are panes.
//
// Panes get no fire geometry -- that would make every fire-geometry user (AI line of fire, the
// cursor, blast occlusion) see them. Instead bullets and explosions ask this registry directly
// (Landscape::BreakNativeGlassAlongSegment / BreakNativeGlassInRadius); a pane never stops a
// projectile. A broken pane's placement switches to the broken model and is handed back to the
// object stream, which admits it again as the broken model -- and keeps it broken across camera
// eviction, since admission reads the placement's model.
//
// POSEIDON_ENFUSION_GLASS_BREAK=0 keeps the loader from registering anything.

#include <Poseidon/Foundation/Math/Math3D.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Poseidon
{

class EnfusionGlassRegistry
{
  public:
    struct Pane
    {
        uint32_t placement = 0xffffffffu;   //!< streamed placement, or none (radius-placed object)
        int id = -1;                        //!< object id
        uint32_t brokenModel = 0xffffffffu; //!< object-stream model index of the broken phase
        float position[3] = {0.0f, 0.0f, 0.0f};
        bool broken = false;
    };

    static bool Enabled();

    void Clear();
    void AddPane(uint32_t placement, int id, uint32_t brokenModel, Vector3Par position);
    size_t Count() const { return _panes.size(); }
    size_t BrokenCount() const { return _broken; }
    Pane& Get(uint32_t index) { return _panes[index]; }
    const Pane& Get(uint32_t index) const { return _panes[index]; }
    void MarkBroken(uint32_t index);

    //! Intact panes in the cells the segment passes (padded by `pad` metres).
    void Candidates(Vector3Par from, Vector3Par to, float pad, std::vector<uint32_t>& out) const;
    //! Intact panes within `radius` of `centre` (by their registered position).
    void InRadius(Vector3Par centre, float radius, std::vector<uint32_t>& out) const;
    //! The intact-or-broken pane nearest `pos` within `radius`, or -1 (test commands).
    int Nearest(Vector3Par pos, float radius) const;

  private:
    static int64_t Key(int64_t cx, int64_t cz) { return (cx << 32) ^ (cz & 0xffffffffll); }
    std::vector<Pane> _panes;
    std::unordered_map<int64_t, std::vector<uint32_t>> _cells; //!< 8 m cells
    size_t _broken = 0;
};

} // namespace Poseidon
