#pragma once

#include <Poseidon/Core/Types.hpp>
#include <Poseidon/Graphics/Rendering/Primitives/Poly.hpp>
#include <Poseidon/Foundation/Containers/StaticArray.hpp>

#include <Poseidon/Graphics/Rendering/Lighting/Material.hpp>
// binary copy will do

#include <Poseidon/Foundation/Containers/StreamArray.hpp>

// info about shape section properties (texture and material)

namespace Poseidon
{
struct ShapeSectionInfo
{
	PolyProperties properties; // face properties
	int material; // special material index
	Ref<TexMaterial> surfMat; // generic surface material

	bool operator == (const ShapeSectionInfo &sec) const
	{
		return
		(
			properties.GetTexture()==sec.properties.GetTexture() &&
			properties.Special()==sec.properties.Special() &&
			surfMat==sec.surfMat &&
			material==sec.material
		);
	}
	bool operator != (const ShapeSectionInfo &sec) const
	{
		return
		(
			properties.GetTexture()!=sec.properties.GetTexture() ||
			properties.Special()!=sec.properties.Special() ||
			surfMat!=sec.surfMat ||
			material!=sec.material
		);
	}
};

// Does the wgpu RETAINED path refuse to own this section on grounds its SPEC does not carry?
//
// render::IsGpuOwnedSectionSpec reads the spec bits; ClassifyGpuSection reads the spec AND the
// texture's alpha histogram, and declines anything the histogram calls Blend. A section whose
// spec is plain opaque over a translucent texture therefore falls between them: declined by the
// GPU, skipped by the CPU as "GPU-owned", drawn by neither. Every caller that asks "is this the
// GPU's?" must subtract this, or it loses geometry (Shape::Draw's skip) or mis-classifies the
// object's coverage as Full and never calls the CPU draw at all (RegisterGpuModel).
//
// Defined in ShapeDraw.cpp beside SectionIsBlend, which is the routing rule it has to agree with.
bool SectionRetainedRefuses(const ShapeSectionInfo &sec);

// THE ownership question, asked once and answered in one place (2026-09-02). Every caller that
// wants to know whether the wgpu RETAINED path draws a section -- Shape::Draw's skip for a
// Partial object, RegisterGpuModel's coverage decision, ClassifyGpuSection's admission -- asks
// this, so the three can no longer disagree in the direction that loses geometry.
//
// True when the spec is plain opaque and the retained path does not refuse the texture
// (today's rule), AND ALSO when the spec is IsAlpha but the texture is a cutout: a Cutout-class
// histogram, a legacy plant cutout, or a Blend-class texture whose authored material routes it
// to a hard alpha test (TreeAdv leaf cards, whose antialiased alpha the histogram calls Blend).
// That second half is the per-draw path's own rule -- "a face authored IsAlpha over a Cutout-
// class texture draws in the OPAQUE pass as a hard alpha test" (EngineWgpu DrawSectionTL) --
// and it is what lets a tree whose LOD 0 is all IsAlpha leaf cards register that LOD instead
// of its coarser neighbour. True translucency (Blend class, Default route) stays the CPU's.
bool SectionGpuOwned(const ShapeSectionInfo &sec);

struct ShapeSection: public ShapeSectionInfo
{
	Offset beg,end; // beg,end in PolyPlainArray of Shape

	void SerializeBin(SerializeBinStream &f, Shape *shape);
	void PrepareTL
	(
		const TLMaterial &mat, const LightList &lights, int spec
	) const;
};

class IAnimator;

class FaceArray: public StreamArray<Poly,StaticArray<char> >
{
	typedef StreamArray<Poly,StaticArray<char> > base;

	friend class Shape; // shape needs access to sections

	StaticArray<ShapeSection> _sections; // faces divided by sections

	public:
	FaceArray() {};
	~FaceArray() {};

	FaceArray( int size, bool dynamic );
	void ReserveFaces( int size, bool dynamic );
	void SetSections( const ShapeSection *sec, int nSec);
	
	void Draw
	(
		const IAnimator *matSource,
		const LightList &lights,
		const Shape &mesh, ClipFlags clip, int spec,
		const Matrix4 &transform, const Matrix4 &invTransform
	) const;
	void Draw
	(
		const IAnimator *matSource,
		TLVertexTable &tlTable,
		const LightList &lights,
		const Shape &mesh, ClipFlags clip, int spec,
		const Matrix4 &invTransform
	) const;

	void Clip
	(
		const FaceArray &faces, TLVertexTable &tlMesh,
		const Camera &camera, ClipFlags clipFlags, bool doCull=true
	);
	void SurfaceSplit
	(
		const FaceArray &faces, TLVertexTable &tlMesh,
		Scene &scene, ClipFlags clipFlags, float y
	);
	
	Poly *AddClipped
	(
		const Poly &face, TLVertexTable &tlMesh, Scene &scene,
		ClipFlags clipFlags
	);
	Poly *AddNoClip
	(
		const Poly &face, TLVertexTable &tlMesh, Scene &scene
	);

	bool VerifyStructure() const;
};
} // namespace Poseidon

using Poseidon::ShapeSection;
