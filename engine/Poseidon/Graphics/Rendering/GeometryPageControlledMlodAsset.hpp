#pragma once
#include <Poseidon/Graphics/Rendering/ControlledMlodPreflight.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageShapeExport.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODLoader.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>
#include <Poseidon/Foundation/Algorithms/Sha256.hpp>
#include <memory>
#include <new>
#include <exception>
#include <Poseidon/Graphics/Core/TLVertex.hpp>
#include <limits>

namespace Poseidon::GeometryPages
{
// Fixed copied values from the ACTUAL plain ShapeSection before destruction.
// These are the same base CreateMaterial(HWhite,type) values used by
// ClassifyGpuSection; not a WGPU material/shader/pass classification. Unknown
// texture, surface material, special or shading type refuses rather than loading.
struct ControlledMlodPlainMaterial
{
    std::array<float,4> emissive{},ambient{},diffuse{},specular{},forcedDiffuse{};
    int32_t materialType=0,specFlags=0,specularPower=0;
    bool operator==(const ControlledMlodPlainMaterial&) const = default;
};
namespace ControlledMlodAssetDetail
{
inline ControlledMlodStatus CapturePlainMaterial(const ShapeSection& section,ControlledMlodPlainMaterial& destination)
{
    if(!Foundation::IsMainThread())return ControlledMlodStatus::WrongOwner;
    if(section.material!=0 || section.properties.Special()!=0 ||
       section.properties.GetTexture()!=nullptr || !section.surfMat.IsNull())
        return ControlledMlodStatus::Unsupported;
    TLMaterial actual;
    // The checked type0 branch is CreateMaterialNormal: no config/bank lookup,
    // texture upload or family translation. Keep the ACTUAL color alpha too.
    CreateMaterial(actual,HWhite,section.material);
    const auto copy=[](ColorVal c){return std::array<float,4>{c.R(),c.G(),c.B(),c.A()};};
    ControlledMlodPlainMaterial result;
    result.materialType=section.material;result.specFlags=actual.specFlags;result.specularPower=actual.specularPower;
    result.emissive=copy(actual.emmisive);result.ambient=copy(actual.ambient);result.diffuse=copy(actual.diffuse);
    result.specular=copy(actual.specular);result.forcedDiffuse=copy(actual.forcedDiffuse);
    for(const auto* color:{&result.emissive,&result.ambient,&result.diffuse,&result.specular,&result.forcedDiffuse})
        for(float value:*color)if(!std::isfinite(value))return ControlledMlodStatus::Invalid;
    destination=result;return ControlledMlodStatus::Eligible;
}
// Geometric value bound only, not identity/eligibility, collision or lifetime
// certification. Caller keeps the already-exported snapshot immutable. Bounded
// two-stream walk includes both authored fallback and original fine positions;
// existing selected-CLOD decoder COPIES source vertices and therefore inherits
// this bound under its separate exact-source association. No recenter/transform.
inline ControlledMlodStatus OriginRadius(const ShapeExport& source,float& destination)
{
    double squared=0;
    for(const auto* mesh:{&source.coarse,&source.fine}) {
        if(mesh->vertices.empty() || mesh->vertices.size()!=mesh->positions.size())return ControlledMlodStatus::Invalid;
        if(mesh->vertices.size()>4096)return ControlledMlodStatus::Capacity;
        for(size_t i=0;i<mesh->vertices.size();++i) {
            const auto& vertex=mesh->vertices[i].pos;const auto& position=mesh->positions[i];
            const float x=vertex.X(),y=vertex.Y(),z=vertex.Z();
            if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z))return ControlledMlodStatus::Invalid;
            if(std::abs(x)>10000 || std::abs(y)>10000 || std::abs(z)>10000)return ControlledMlodStatus::Unsupported;
            if(x!=position.x || y!=position.y || z!=position.z)return ControlledMlodStatus::Invalid;
            const double distance=double(x)*x+double(y)*y+double(z)*z;
            squared=std::max(squared,distance);
        }
    }
    if(!std::isfinite(squared))return ControlledMlodStatus::Invalid;
    // Float source values are exact in double; three products/two additions have
    // much less error than one float ULP. Always step outward after sqrt/cast,
    // including exactly representable radii, so the model bound never rounds in.
    const float radius=std::nextafter(float(std::sqrt(squared)),std::numeric_limits<float>::infinity());
    if(!std::isfinite(radius) || radius<=0 || double(radius)*radius<squared)return ControlledMlodStatus::Invalid;
    destination=radius;return ControlledMlodStatus::Eligible;
}
}
struct ControlledMlodAsset
{
    ShapeExport original;
    std::array<float,2> sourceResolutions{};
    size_t originalSourceBytes=0;
    ControlledMlodPlainMaterial coarseMaterial,fineMaterial;
    float originRadius=0; // origin-relative enclosing sphere, not bbox-centred radius

};
// Consumes a caller-owned whole original file snapshot. Hash and the SINGLE
// original loader invocation observe these same bytes. No file/VFS/DDC/cache,
// bank/config read, texture initialization, CLOD bake or runtime consumer.
// Raw source hash is not the synthetic pilot geometry key. Result should remain
// immutable; no collision/helper/all-pass or source-disk-freshness proof implied.
// P3DM winding retains the existing loader's ASSUMED convention pending visuals.
inline ControlledMlodStatus ExportControlledMlodOwned(std::vector<uint8_t> bytes,ControlledMlodAsset& destination)
{
    if(!Foundation::IsMainThread())return ControlledMlodStatus::WrongOwner;
    ControlledMlodRawFacts facts;
    const auto preflight=PreflightControlledMlod(bytes,facts);
    if(preflight!=ControlledMlodStatus::Eligible)return preflight;
    try {
        // Bypass ModelCache: its derived/shared cache is unnecessary for this
        // exclusive one-shot source and could weaken the immutable association.
        auto model=Asset::Formats::MLODLoader::loadFromBuffer(
            reinterpret_cast<const char*>(bytes.data()),int(bytes.size()),"offline-controlled-mlod.p3d");
        if(!model.compile())return ControlledMlodStatus::Invalid;
        std::unique_ptr<LODShapeWithShadow> shape(Model::ShapeAdapter::ConvertControlledMlod(model));
        if(!shape)return ControlledMlodStatus::Unsupported;
        ShapeExportSelection selection;selection.controlledAuthoredRigid=true;
        selection.coarseLevel=facts.resolutions[0]>facts.resolutions[1]?0:1;
        selection.fineLevel=1-selection.coarseLevel;
        Foundation::Sha256 hash;hash.Update(bytes.data(),bytes.size());const auto hex=hash.Hex();
        const auto digit=[](char c){return uint8_t(c<='9'?c-'0':c-'a'+10);};
        for(size_t i=0;i<32;++i)selection.source.sourceSha256[i]=uint8_t(digit(hex[i*2])*16+digit(hex[i*2+1]));
        selection.source.geometryOptions=0x4d4c4f445031ull; // original-byte subset/context policy v1
        selection.source.producerVersion=1;selection.source.vertexLayout=sizeof(SVertex);
        selection.source.materialMapping=1;selection.source.coarseRepresentation=selection.coarseLevel;
        selection.source.fineRepresentation=selection.fineLevel;
        ControlledMlodAsset result;
        const auto status=ExportShapePair(*shape,selection,result.original);
        if(status!=ExportStatus::Exported)return status==ExportStatus::Capacity?
            ControlledMlodStatus::Capacity:ControlledMlodStatus::Invalid;
        if(shape->NLevels()!=2||shape->Level(0)->NSections()!=1||shape->Level(1)->NSections()!=1)
            return ControlledMlodStatus::Unsupported;
        if(ControlledMlodAssetDetail::CapturePlainMaterial(shape->Level(selection.coarseLevel)->GetSection(0),result.coarseMaterial)!=ControlledMlodStatus::Eligible ||
           ControlledMlodAssetDetail::CapturePlainMaterial(shape->Level(selection.fineLevel)->GetSection(0),result.fineMaterial)!=ControlledMlodStatus::Eligible)
            return ControlledMlodStatus::Unsupported;
        for (const auto* mesh : {&result.original.coarse,&result.original.fine})
            for (const auto& v : mesh->vertices) {
                if (v.conform) return ControlledMlodStatus::Unsupported;
                for (float f : {v.pos.X(),v.pos.Y(),v.pos.Z(),v.norm.X(),v.norm.Y(),v.norm.Z(),
                    v.tangent.X(),v.tangent.Y(),v.tangent.Z(),v.binormal.X(),v.binormal.Y(),v.binormal.Z(),
                    v.t0.u,v.t0.v,v.t1.u,v.t1.v})
                    if (!std::isfinite(f)) return ControlledMlodStatus::Invalid;
            }
        const auto bounded=ControlledMlodAssetDetail::OriginRadius(result.original,result.originRadius);
        if(bounded!=ControlledMlodStatus::Eligible)return bounded;
        result.sourceResolutions=facts.resolutions;result.originalSourceBytes=bytes.size();
        destination=std::move(result);return ControlledMlodStatus::Exported;
    } catch(const std::bad_alloc&) {return ControlledMlodStatus::AllocationFailed;}
      catch(const std::exception&) {return ControlledMlodStatus::Invalid;}
}
} // namespace Poseidon::GeometryPages
