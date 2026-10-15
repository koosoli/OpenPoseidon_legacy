#pragma once

#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/Graphics/Core/TLVertex.hpp>
#include <Poseidon/Foundation/Types/RemoveLinks.hpp>

#include <string>

// Surface material properties; primary consumer is ShapeSection.

namespace Poseidon
{
class TexMaterial: public RefCountWithLinks
{
	public:
	RStringB _name;

	// also stored in PolyProperties
	Ref<Texture> _tex;

	Color _emmisive; // shining polygons
	Color _ambient;
	Color _diffuse;
	Color _forcedDiffuse; // simulates half- or fully-lit polys
	Color _specular;
	// higher value means sharper highlight; zero means no highlight
	float _specularPower;

	Ref<Texture> _bumpmap;
	float _bumpmapFactor;

	Ref<Texture> _detailmap;
	float _detailmapFactor;

	// The per-texel specular map an Arma RVMAT names -- Super's Stage5 `_smdi`, as a
	// canonical path or a procedural `#(...)color(...,SMDI)` -- whose green channel
	// scales _specular texel by texel. Empty for OFP materials.
	std::string _specularMapName;
	mutable float _specularMapLevel = -1.0f; // resolved on first use; -1 = not yet

	TexMaterial();
	TexMaterial(const char *name);
	void Combine(TLMaterial &dst, const TLMaterial &src);
	// The mean of that map's specular channel, 1 when there is none: what a backend
	// that cannot sample the map should scale _specular by. Reads the map's header
	// (TAGG AVGC) the first time, so call it from the render thread.
	float SpecularMapLevel() const;
	const RStringB &GetName() const {return _name;} // required by BankArray
};

} // namespace Poseidon
#include <Poseidon/Asset/Cache/AssetCache.hpp>
namespace Poseidon
{

// Result of New / TextureToMaterial should be assigned to a
// Ref<TexMaterial>.  Materials persist in the cache until an explicit
// Clear() rather than being released when no external Ref holds them:
// per-mission count is small (~10) and per-instance cost negligible
// (colour + texture refs), so the residency buys unified cache
// semantics across asset types.
class TexMaterialBank
{
	public:
	TexMaterialBank();
	~TexMaterialBank();

	// Find-or-create.  Returns the existing material if `name` is
	// already cached; otherwise constructs `new TexMaterial(name)`,
	// inserts, and returns the fresh ref.
	Ref<TexMaterial> New(const char *name);

	// resolve a texture-driven material via CfgTextureToMaterial
	TexMaterial *TextureToMaterial(Texture *tex);

	// Free every cached material.  Called from world / mission teardown.
	void Clear();

	private:
	::Poseidon::AssetCache<TexMaterial, Ref<TexMaterial>> _cache;
};

extern TexMaterialBank GTexMaterialBank;
} // namespace Poseidon
