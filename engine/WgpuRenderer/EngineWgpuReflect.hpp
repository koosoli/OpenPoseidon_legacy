#pragma once

// REFLECT-001 phase B: per-section surface class lanes for WgrDraw3D (see EngineWgpuReflect.cpp).

#include <wgpu_renderer.hpp>

namespace Poseidon
{
class Object;
class Texture;
class TexMaterial;

//! True when WGR_REFLECT or WGR_REFLECT_DEBUG asks for the lanes to be filled.
bool ReflectLanesEnabled();

//! Fill d.mat_reflect0/1 for one draw section; leaves them zero when the switch is off.
void FillReflectLanes(WgrDraw3D& d, const Object* owner, const Texture* texture, const TexMaterial* material);
} // namespace Poseidon
