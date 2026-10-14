#pragma once

#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/Graphics/Core/TLVertex.hpp>
#include <Poseidon/Foundation/Types/RemoveLinks.hpp>

#include <atomic>
#include <cstdint>

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

	TexMaterial();
	TexMaterial(const char *name);
	void Combine(TLMaterial &dst, const TLMaterial &src);
	const RStringB &GetName() const {return _name;} // required by BankArray

	// PERF (2026-10-14): answers that are pure functions of _name, memoised on the material.
	// The per-section classifiers (ShapeDraw's SectionMaterial*, EngineWgpu's GpuEmat*) each
	// built a std::string from the path and hashed it under a mutex for every draw, every
	// frame -- a few percent of a native world's main thread for no new information. They
	// still compute through their own path-keyed caches the first time, so what is read from
	// disk, and when, is unchanged. Four bits a slot: bit 0 known, bits 1-3 the value.
	enum MemoSlot
	{
		MemoSectionEmatBlends,
		MemoSectionCullNone,
		MemoSectionAlphaRoute,
		MemoGpuEmatBlends,
		MemoGpuEmatCullNone,
		MemoSlotCount
	};
	static_assert(MemoSlotCount <= 8, "eight four-bit slots in the memo word");
	template <class Compute>
	int Memo(MemoSlot slot, Compute&& compute) const
	{
		const uint32_t shift = static_cast<uint32_t>(slot) * 4;
		const uint32_t word = _nameMemo.load(std::memory_order_relaxed);
		if (word & (1u << shift))
			return static_cast<int>((word >> (shift + 1)) & 7u);
		const int value = compute();
		// Two threads racing here computed the same pure function, so either write is right.
		_nameMemo.fetch_or((1u | (static_cast<uint32_t>(value) & 7u) << 1) << shift, std::memory_order_relaxed);
		return value;
	}
	//! Unique per constructed material: a pointer-keyed cache can tell a recycled address
	//! from the material it remembers without comparing paths.
	uint64_t Serial() const {return _serial;}

	private:
	mutable std::atomic<uint32_t> _nameMemo{0};
	uint64_t _serial;
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
