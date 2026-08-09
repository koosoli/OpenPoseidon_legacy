
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/Dev/Diag/ScopedTimer.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/World/Model/ModelCache.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>
#include <Poseidon/Input/InputSubsystem.hpp>
#include <Poseidon/World/Simulation/Animation/Animation.hpp>
#include <Poseidon/Graphics/Core/TLVertex.hpp>

#include <Poseidon/Graphics/Rendering/BuildRenderPassDescriptor.hpp>
#include <Poseidon/Graphics/Rendering/Primitives/Edges.hpp>
#include <Poseidon/Graphics/Rendering/Draw/SpecLods.hpp>

#include <Poseidon/Foundation/Algorithms/Qsort.hpp>
#include <Poseidon/Foundation/Common/Filenames.hpp>

#include <Poseidon/Core/Data3D.h>

#include <Poseidon/World/MapTypes.hpp>
#include <stdio.h>
#include <cmath>
#include <memory>
#include <string>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Containers/StaticArray.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/V3Quads.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <Poseidon/Foundation/Types/RemoveLinks.hpp>
#include <Poseidon/Foundation/platform.hpp>
#include <Poseidon/Asset/Formats/Material/RvMaterialSource.hpp>
#include <mutex>
#include <string>
#include <unordered_map>

extern bool DisableTextures;

namespace Poseidon
{

#ifdef _MSC_VER
#pragma warning(disable : 4355)
#endif

inline bool IsSpec(float resolution, float spec)
{
    return fabs(resolution - spec) < spec * 1e-3;
}

bool EnableHWTLState = true;

// Alpha routing asks the decoded texture's histogram, and for anti-aliased cutouts the
// histogram gives the wrong answer. `ClassifyAlpha` calls anything with >=2% partial
// alpha a blend, and an alpha-TESTED leaf with soft edges is mostly partial alpha at the
// edges: the shipped Arma 3 pine's leaf texture is 30.3% partial, the Enoch birch's 5.8%,
// and 44 of 50 plants_f `_ca` textures classify as blend. A blend section leaves the
// opaque pass for a back-to-front pass with no depth write, and every symptom follows --
// no shadows (nothing occludes or casts), a canopy that accumulates instead of
// depth-testing (measured mean 16.6 against a frame mean of 4.1 at 01:00), no crisp leaf
// edge at distance, and dark patches that move because interpenetrating cards sort
// order-dependently.
//
// CWA content is spared by accident of format: ClassifyTextureAlpha short-circuits 1-bit
// alpha straight to Cutout, so the engine has never been asked about an anti-aliased
// cutout before.
//
// The histogram is the wrong authority; the shader family is the right one. A family
// whose alpha is measured to be a cutout demotes Blend -> Cutout here. This only ever
// DEMOTES: a section the histogram already calls Opaque or Cutout is untouched, so
// forcing this on an opaque bark section is a no-op rather than a new alpha test.
//
// Only the families actually measured are listed. Grass and SuperAToC are near-certain
// members -- the latter says alpha-to-coverage in its own name -- but neither has been
// measured here, and adding one is a single line once it has been.
static bool ShaderFamilyIsCutout(const char* family)
{
    static const char* const kCutoutFamilies[] = {"TreeAdv", "TreeAdvTrunk", "TreeAdvTrans"};
    for (const char* known : kCutoutFamilies)
        if (strcmpi(known, family) == 0)
            return true;
    return false;
}

// Resolving this means opening and parsing an RVMAT, which cannot happen per draw, so the
// answer is cached by material path -- including the negative answers, so a missing or
// unparseable material is not retried on every frame.
static bool SectionMaterialIsCutout(const Ref<TexMaterial>& surfMat)
{
    if (surfMat.IsNull())
        return false;
    const char* path = surfMat->GetName().Data();
    if (!path || !*path)
        return false;

    static std::unordered_map<std::string, bool> cache;
    static std::mutex                            cacheMutex;
    const std::string                            key(path);

    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        const auto                  found = cache.find(key);
        if (found != cache.end())
            return found->second;
    }

    bool cutout = false;
    try
    {
        cutout = ShaderFamilyIsCutout(Asset::Material::ParseRvMaterialFile(key).pixelShaderId.c_str());
    }
    catch (...)
    {
        cutout = false; // unavailable material: keep the histogram's answer
    }

    std::lock_guard<std::mutex> lock(cacheMutex);
    cache[key] = cutout;
    return cutout;
}

// The single place that decides whether a section belongs in the back-to-front pass.
static bool SectionIsBlend(const ShapeSectionInfo& sec)
{
    auto* tex = sec.properties.GetTexture();
    if (!tex || tex->GetAlphaClass() != Poseidon::AlphaStats::Blend)
        return false;
    return !SectionMaterialIsCutout(sec.surfMat);
}

bool Shape::HasBlendSections() const
{
    if (_hasBlendSections < 0)
    {
        _hasBlendSections = 0;
        for (int i = 0; i < NSections(); i++)
        {
            if (SectionIsBlend(GetSection(i)))
            {
                _hasBlendSections = 1;
                break;
            }
        }
    }
    return _hasBlendSections != 0;
}

SectionClassFilter GSectionFilter = SectionClassFilter::All;
bool GSkipGpuOwnedSections = false;
ConformPlane GCurrentConformPlane;
bool GCurrentIsVegetation = false;
bool GGpuTerrainConform = false;

void Shape::Draw(class IAnimator* matSource, const LightList& lights, ClipFlags clip, int spec,
                 const Matrix4& transform, const Matrix4& invTransform)
{
#ifndef ACCESS_ONLY
    // if engine has T&L interface, use it
    // cannot use T&L on some surface types (OnSurface?)

    Engine* engine = GEngine;

    bool tlAble = ((spec & OnSurface) == 0 || engine->GetTLOnSurface()) && (clip & ClipUser0) == 0;

    if (engine->GetTL() && EnableHWTLState && tlAble && _buffer)
    {
        if (BeginFaces() < EndFaces() && NSections() > 0)
        {
            GEngine->PrepareMeshTL(lights, transform, render::SplitLegacy(spec));
            if (spec & (OnSurface | IsOnSurface))
            {
                engine->SetBias(0x10);
            }
            else
            {
                int bias = (spec & ZBiasMask) / ZBiasStep;
                // max. bias value is 3
                engine->SetBias(bias * 5);
            }
            // prepare sections (if neccessary)
            // prepare lights and materials

            // check if shape is dynamic or not
            bool dynamic = matSource->GetAnimated(*this);
            engine->BeginMeshTL(*this, spec, dynamic);
            // check first face properties
            int secBeg = -1;
            int secEnd = -1;
            Texture* secTexture = (Texture*)-1;
            int secSpecial = -1;
            int secMaterial = -1;
            TexMaterial* secSurfMat = nullptr;

            for (int i = 0; i < NSections(); i++)
            {
                const ShapeSection& sec = GetSection(i);
                if (sec.properties.Special() & (IsHidden | IsHiddenProxy))
                {
                    continue;
                }

                // Partial GPU-driven object (wgpu §12): the GPU retained scene already drew
                // this shape's GPU-owned (opaque, Default-surface) sections into the colour +
                // prepass targets, so the CPU repaints only the complement (blend glass,
                // decals). Same predicate the wgpu backend registered the section with, so
                // owned sections are dropped exactly once — never double-drawn, never held.
                if (GSkipGpuOwnedSections && render::IsGpuOwnedSectionSpec(sec.properties.Special()))
                {
                    continue;
                }

                // per-section transparency routing: in the opaque pass draw only
                // opaque+cutout sections, in the back-to-front pass only blend ones.
                if (GSectionFilter != SectionClassFilter::All)
                {
                    const bool isBlend = SectionIsBlend(sec);
                    if ((GSectionFilter == SectionClassFilter::BlendOnly) != isBlend)
                    {
                        continue;
                    }
                }

                if (secBeg < 0)
                {
                    secBeg = i;
                    secEnd = i + 1;
                    secTexture = sec.properties.GetTexture();
                    secSpecial = sec.properties.Special();
                    secMaterial = sec.material;
                    secSurfMat = sec.surfMat;
                }
                else if (sec.properties.GetTexture() == secTexture && sec.properties.Special() == secSpecial &&
                         sec.material == secMaterial && sec.surfMat == secSurfMat && i == secEnd)
                {
                    // extend section
                    secEnd = i + 1;
                }
                else
                {
                    // flush section
                    TLMaterial mat;
                    matSource->GetMaterial(mat, GetSection(secBeg).material);
                    GetSection(secBeg).PrepareTL(mat, lights, spec);
                    GEngine->DrawSectionTL(*this, secBeg, secEnd);
                    // open another section
                    secBeg = i;
                    secEnd = i + 1;
                    secTexture = sec.properties.GetTexture();
                    secSpecial = sec.properties.Special();
                    secMaterial = sec.material;
                    secSurfMat = sec.surfMat;
                }
            }
            if (secEnd > secBeg)
            {
                int matIndex = GetSection(secBeg).material;
                TLMaterial mat;
                matSource->GetMaterial(mat, matIndex);
                // flush section
                GetSection(secBeg).PrepareTL(mat, lights, spec);

                GEngine->DrawSectionTL(*this, secBeg, secEnd);
            }

            engine->EndMeshTL(*this);
        }
    }
    else
    {
        DoAssert(_face._sections.Size() == 0 || _face._sections[_face._sections.Size() - 1].end == EndFaces());

        // The section-class filter can split sections only on the hardware-T&L path
        // above; this non-TL fallback (OnSurface/ClipUser0 surfaces, or shapes with no
        // vertex buffer — e.g. the live tyre-track ribbon, rebuilt every frame) draws
        // the whole shape exactly once, in the pass DrawWholeShapeInPass routes it to:
        // surface overlays in the sorted on-surface (BlendOnly) pass so the road's
        // asphalt cannot repaint over them, everything else in the opaque pass.
        const bool surfaceOverlay = (spec & (OnSurface | IsOnSurface)) != 0 && HasBlendSections();
        if (DrawWholeShapeInPass(GSectionFilter, surfaceOverlay))
        {
            // custom T&L
            _face.Draw(matSource, lights, *this, clip, spec, transform, invTransform);
        }
    }
#endif
}

LODShapeWithShadow* ShapeBank::New(const char* name, bool reversed, bool shadow)
{
    char lowName[128];
    snprintf(lowName, sizeof(lowName), "%s", (const char*)name);
    strlwr(lowName);

    // Composite cache key: name + reversed + shadow flags.  This
    // replaces the previous linear-scan-with-bit-flag-matching: each
    // (name, reversed, shadow) tuple maps 1:1 to a unique cached
    // shape, so exact-tuple keying produces the same partition as
    // the original bit-flag matcher.  Length budget: max P3D path
    // ~128 + 4-char suffix; comfortably fits the asset paths in tree.
    char cacheKey[160];
    snprintf(cacheKey, sizeof(cacheKey), "%s|R%dS%d", lowName, reversed ? 1 : 0, shadow ? 1 : 0);

    int remNeeded = 0;
    if (reversed)
    {
        remNeeded |= REM_REVERSED;
    }
    if (!shadow)
    {
        remNeeded |= REM_NOSHADOW;
    }

    if (LODShapeWithShadow* existing = _cache.Lookup(cacheKey))
    {
        return existing;
    }
    // Lookup returned nullptr — either the key was never inserted, or a
    // previously-inserted shape was destroyed and its weak Link auto-nulled.
    // In the latter case the slot is still keyed but dead; drop it so the
    // upcoming Insert allocates a fresh slot instead of being short-circuited
    // by the "key already present" idempotency path in AssetCache::Insert.
    _cache.Remove(cacheKey);
    // Phase A measurement: cache-miss model load is one of the
    // top-three first-touch hitch sources (the other two are music
    // decode in WaveOAL and per-mip texture upload in
    // GlobLoadTexture).
    const auto _perfShapeLoadStart = ::Poseidon::Dev::Perf::Now();

    // The legacy LODShape loader only understands the old SP3X MLOD layout.
    // Arma samples use P3DM MLOD, which the canonical Model loader decodes
    // before ShapeAdapter expands it into the runtime vertex buffer.  Routing
    // both recognised P3D encodings through that path keeps test-model (and
    // normal-map smoke tests) from silently producing an empty mesh.
    LODShapeWithShadow* shape = nullptr;
    {
        Poseidon::ModelCache cache;
        auto model = cache.load(lowName);
        if (model && (model->sourceFormat == "ODOL" || model->sourceFormat == "MLOD"))
        {
            LOG_INFO(Graphics, "Shape loader: canonical {} adapter for {}", model->sourceFormat, lowName);
            // ShapeAdapter sets shape name from model.sourcePath — must use
            // the original backslash path to match ShapeBank lookup key
            model->sourcePath = lowName;
            shape = Poseidon::Model::ShapeAdapter::convertToLODShape(*model, reversed);
        }
        else
        {
            LOG_INFO(Graphics, "Shape loader: legacy path for {} (canonical format={})", lowName,
                     model ? model->sourceFormat : "<unavailable>");
            shape = new LODShapeWithShadow(lowName, reversed);
        }
    }
    if (!shape)
        shape = new LODShapeWithShadow();

    shape->SetRemarks(shape->Remarks() | remNeeded);
    if (!shadow)
    {
        shape->OrSpecial(NoShadow);
    }
    _cache.Insert(cacheKey, Link<LODShapeWithShadow>(shape));

    const double _perfShapeLoadMs = ::Poseidon::Dev::Perf::ElapsedMs(_perfShapeLoadStart);
    if (_perfShapeLoadMs >= 1.0)
    {
        LOG_DEBUG(Graphics, "PERF: ShapeBank::New {} took {:.2f}ms ({} LODs)", lowName, _perfShapeLoadMs,
                  shape->NLevels());
    }
    ::Poseidon::Dev::Perf::EmitTraceEventAssetNum(Poseidon::Foundation::LogCategory::Graphics, "ShapeBank::New",
                                                  _perfShapeLoadStart, static_cast<const char*>(lowName), "lods",
                                                  shape->NLevels());
    return shape;
}

void Shape::ConvertToVBuffer(VBType type)
{
    if (NVertex() <= 0)
    {
        return;
    }
    if (!ENGINE_CONFIG.enableHWTL)
    {
        return;
    }
    DoAssert(!_buffer);
    if (!_buffer)
    {
        _buffer = GEngine->CreateVertexBuffer(*this, type);
    }
}

void Shape::ReleaseVBuffer()
{
    _buffer.Free();
}

void ShapeBank::ReleaseAllVBuffers()
{
    ForEach(
        [](LODShapeWithShadow& shape)
        {
            for (int l = 0; l < shape.NLevels(); l++)
            {
                Shape* level = shape.Level(l);
                level->ReleaseVBuffer();
            }
        });
}

void LODShape::OptimizeRendering()
{
    LODShape* shape = this;
    bool reload = false;
    for (int l = 0; l < shape->NLevels(); l++)
    {
        Shape* level = shape->Level(l);
        bool optimizeHW = false;
        bool optimizeSSE = false;
        if (ENGINE_CONFIG.enableHWTL)
        {
            optimizeHW = level->NVertex() > 0 && level->NFaces() > 0;
        }
        if (ENGINE_CONFIG.enablePIII)
        {
            optimizeSSE = !shape->GetAllowAnimation() && level->NVertex() >= 16;
        }
        if (optimizeHW || optimizeSSE)
        {
            ClipFlags globalLight = shape->GetAndHints() & ClipLightMask;
            if (globalLight != (shape->GetOrHints() & ClipLightMask))
            {
                globalLight = 0;
            }
            switch (globalLight)
            {
                case ClipLightCloud:
                case ClipLightSky:
                case ClipLightStars:
                case ClipLightLine:
                    optimizeHW = optimizeSSE = false;
            }
            if (level->Special() & (IsLight | OnSurface))
            {
                optimizeHW = optimizeSSE = false;
            }
        }
        // no optimize on geometry levels
        if (shape->Resolution(l) > 900)
        {
            // ignore some special levels
            if (l == shape->FindFireGeometryLevel() || l == shape->FindGeometryLevel() ||
                l == shape->FindViewGeometryLevel() || l == shape->FindViewPilotGeometryLevel() ||
                l == shape->FindViewCargoGeometryLevel() || l == shape->FindViewCommanderGeometryLevel() ||
                l == shape->FindViewGunnerGeometryLevel() || l == shape->FindMemoryLevel() || l == shape->FindPaths() ||
                l == shape->FindLandContactLevel() || l == shape->FindRoadwayLevel())
            {
                continue;
            }
        }
#if USE_QUADS
        if (optimizeSSE)
        {
            if (level->PosQuad().Size() <= 0)
            {
                level->ConvertToQArray();
            }
        }
#endif
        if (optimizeHW)
        {
            const RStringB tentString("tent");
            VBType type = shape->GetAllowAnimation() ? VBDynamic : VBStatic;
            if (shape->GetPropertyDammage() == tentString)
            {
                type = VBDynamic;
            }
            level->ConvertToVBuffer(type);
        }
        else
        {
            // check if there are some normal arrays
            if (level->NVertex() == 0)
            {
// back conversion not possible
#if USE_QUADS
                if (level->GetVertexBuffer() || level->PosQuad().Size() > 0)
#else
                if (level->GetVertexBuffer())
#endif
                {
                    // shape is not empty
                    // we need to reload shape
                    reload = true;
                }
            }
        }
    } // for (l)
    if (reload)
    {
        Fail("Reload not possible");
        RptF("Reloading %s", shape->Name());
    }
}

void ShapeBank::OptimizeAll()
{
    // first of all: flush all vertex buffers
    // recreate vertex buffers
    if (ENGINE_CONFIG.enableHWTL)
    {
        ReleaseAllVBuffers();
        LOG_DEBUG(Graphics, "ShapeBank::OptimizeAll");
    }
    ForEach([](LODShapeWithShadow& shape) { shape.OptimizeRendering(); });
}

void ShapeBank::Clear()
{
    _cache.Clear();
}

ShapeBank Shapes;

void PrepareTexture(Texture* texture, float z2, int special, float areaOTex);

void Shape::PrepareTextures(float z2, int special) const
{
#ifndef ACCESS_ONLY
    // hint mipmap for all textures used on this shape
    for (int t = 0; t < _textures.Size(); t++)
    {
        Texture* txt = _textures[t];
        if (txt)
        {
            float areaOTex = _areaOTex[t];
            PrepareTexture(txt, z2, special, areaOTex);
        }
    }
#endif
}

} // namespace Poseidon
