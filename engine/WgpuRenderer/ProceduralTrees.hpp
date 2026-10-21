// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "include/wgpu_renderer.hpp"
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/Network/NetworkObject.hpp>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Poseidon
{
class EngineWgpu;
class Object;

// The owner's tree test: OFP trees near the camera swapped for procedural trees grown by a port of
// dgreenheck/ez-tree (see ProceduralTrees.cpp). Off unless Dev::ProceduralTrees().enabled.
class ProceduralTrees
{
  public:
    ProceduralTrees(EngineWgpu& engine, WgrRenderer* renderer) : _engine(engine), _renderer(renderer) {}
    ~ProceduralTrees();
    void Update(const Vector3& camera);

  private:
    struct Variant
    {
        WgrMesh bark = 0, leaves = 0;
        uint32_t barkIndices = 0, leafIndices = 0;
        WgrTexture barkTex = 0, leafTex = 0;
        float height = 1; // the generated tree's height, model units
        bool built = false;
    };
    struct Claim
    {
        OLink<Object> obj;
        const Object* key = nullptr;
        int coverage = 0;    // the object's GPU coverage before the swap
        bool retained = false; // it had a GPU-driven instance (re-admitted on release)
        int variant = 0;
        float x = 0, y = 0, z = 0, yaw = 0, scale = 1;
    };
    Variant& Get(int variant);
    void Release(Claim& c);
    void ReleaseAll();

    EngineWgpu& _engine;
    WgrRenderer* _renderer;
    std::vector<Variant> _variants;
    std::unordered_map<const Object*, Claim> _claims;
    float _scan = 0;
    bool _wasEnabled = false;
};
} // namespace Poseidon
