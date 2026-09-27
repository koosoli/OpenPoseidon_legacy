#pragma once
#include <Poseidon/Graphics/Rendering/ControlledMlodPreflight.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageShapeExport.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODLoader.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>
#include <Poseidon/Foundation/Algorithms/Sha256.hpp>
#include <memory>
#include <new>
#include <exception>

namespace Poseidon::GeometryPages
{
struct ControlledMlodAsset
{
    ShapeExport original;
    std::array<float,2> sourceResolutions{};
    size_t originalSourceBytes=0;
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
        for (const auto* mesh : {&result.original.coarse,&result.original.fine})
            for (const auto& v : mesh->vertices) {
                if (v.conform) return ControlledMlodStatus::Unsupported;
                for (float f : {v.pos.X(),v.pos.Y(),v.pos.Z(),v.norm.X(),v.norm.Y(),v.norm.Z(),
                    v.tangent.X(),v.tangent.Y(),v.tangent.Z(),v.binormal.X(),v.binormal.Y(),v.binormal.Z(),
                    v.t0.u,v.t0.v,v.t1.u,v.t1.v})
                    if (!std::isfinite(f)) return ControlledMlodStatus::Invalid;
            }
        result.sourceResolutions=facts.resolutions;result.originalSourceBytes=bytes.size();
        destination=std::move(result);return ControlledMlodStatus::Exported;
    } catch(const std::bad_alloc&) {return ControlledMlodStatus::AllocationFailed;}
      catch(const std::exception&) {return ControlledMlodStatus::Invalid;}
}
} // namespace Poseidon::GeometryPages
