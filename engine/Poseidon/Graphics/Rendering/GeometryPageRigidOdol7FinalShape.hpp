#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageRigidOdol7Candidate.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageShapeExport.hpp>
#include <Poseidon/Graphics/Rendering/BuildRenderPassDescriptor.hpp>
#include <Poseidon/Graphics/Rendering/Shape/PreparedGpuSectionClassification.hpp>
#include <Poseidon/Graphics/Rendering/Shape/ResumableModelAdmission.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/Streams/RetailRigidAssetProfile.hpp>
#include <Poseidon/World/MapTypes.hpp>

namespace Poseidon::GeometryPages
{
// Main-owner synchronous source hold, not a certificate manufactured by the
// helper. decodedSha256 must be the immutable result of ReadDecoded used by the
// canonical loader/candidate gate. The caller retains the corresponding bytes,
// final shape and owner birth throughout this operation. bank is the ACTUAL
// owner-selected mount; metadata checks do not establish global VFS precedence.
struct RigidOdol7FinalSourceHold
{
    std::shared_ptr<const BankCompressedReadRequest> lease;
    const QFBank* bank=nullptr;
    std::array<uint8_t,32> decodedSha256{};
    uint64_t sourceAdmissionEpoch=0,ownerEpoch=0,modelBirth=0;
};
// Returned by the owner inspector from this EXACT current final ShapeSection.
// Ref objects retain the observed incarnations. facts come from the ordinary
// actual material/alpha resolver; current comes from initialized backend source
// provenance and the held source/slot lease, never merely a path recapture.
// current.sourceName remains alive throughout ExportRigidOdol7FinalShape.
// The helper checks/reuses these gates, but cannot independently authenticate
// backend Init, handle/slot birth or renderer binding using a supplied boolean.
struct RigidOdol7FinalMaterialHold
{
    Ref<Texture> texture;
    Ref<TexMaterial> surface;
    render::PreparedGpuSectionFacts facts;
    render::ResumableModelAdmission::TextureProof current;
    uint64_t materialBindingBirth=0;
};
struct RigidOdol7FinalMaterialIdentity
{
    render::ResumableModelAdmission::Member member;
    uint64_t textureToken=0,handle=0,materialBindingBirth=0;
    uint32_t slotLease=0;
    std::string sourceName;
    render::RenderPassDescriptor descriptor;
};
struct RigidOdol7FinalExport
{
    ShapeExport actual;
    std::array<int,2> finalLevels{-1,-1}; // coarse, fine after pruning/compaction
    std::array<uint32_t,2> sourceLods{};
    std::array<float,2> resolutions{};
    bool actualNoShadow=false;
    // Always true: this operation MUST NOT suppress conventional unselected
    // levels/passes. In particular, source0/1 both lodnoshadow=true does not
    // authorize removal of skala_new's source2/3 shadow representations.
    bool requiresOriginalOtherPasses=true;
    uint64_t sourceAdmissionEpoch=0,ownerEpoch=0,modelBirth=0,shapeQueryRevision=0;
    BankReadMemberIdentity sourceMember;
    RigidOdol7FinalMaterialIdentity material;
};
enum class RigidOdol7FinalStatus { Exported, Unsupported, Invalid, StaleSource, StaleMaterial,
                                 Capacity, AllocationFailed, WrongOwner };
namespace RigidOdol7FinalDetail
{
inline bool SamePath(std::string_view a,std::string_view b)
{
    if(a.empty()||a.size()!=b.size()||a.size()>=128)return false;
    auto canonical=[](unsigned char c) {if(c=='/')c='\\';if(c>='A'&&c<='Z')c+=32;return c;};
    for(size_t i=0;i<a.size();++i)if(canonical(a[i])!=canonical(b[i]))return false;
    return true;
}
inline bool SourceCurrent(const RigidOdol7Candidate& candidate,const RigidOdol7FinalSourceHold& hold)
{
    bool hash=false;for(auto b:candidate.source.decodedSha256)hash|=b!=0;
    const auto at=candidate.source.canonicalPath.find_last_of("/\\");
    const auto leaf=std::string_view(candidate.source.canonicalPath).substr(at==std::string::npos?0:at+1);
    return hash && candidate.source.decodedBytes && candidate.source.decodedBytes<=128*1024 &&
        hold.lease && hold.bank && hold.sourceAdmissionEpoch && hold.ownerEpoch && hold.modelBirth &&
        hold.sourceAdmissionEpoch!=UINT64_MAX && hold.ownerEpoch!=UINT64_MAX && hold.modelBirth!=UINT64_MAX &&
        RetailRigidAssetProfile::ByModelPath(candidate.source.canonicalPath) &&
        RetailRigidAssetProfile::ByModelPath(candidate.source.canonicalPath)->MatchesModelMember(leaf) &&
        SamePath(leaf,hold.lease->CanonicalMember()) &&
        hold.decodedSha256==candidate.source.decodedSha256 &&
        hold.lease->DecodedBytes()==candidate.source.decodedBytes &&
        hold.bank->MatchesMountedCompressedMember(hold.lease->CanonicalMember().c_str(),*hold.lease);
}
inline const RigidOdol7VisualCandidate* FindSource(const RigidOdol7Candidate& candidate,uint32_t index)
{
    const RigidOdol7VisualCandidate* found=nullptr;
    if(candidate.visual.size()>16)return nullptr;
    for(const auto& visual:candidate.visual)if(visual.sourceLod==index) {
        if(found)return nullptr;found=&visual;
    }
    return found;
}
inline int FindFinal(const LODShape& shape,float resolution)
{
    if(!std::isfinite(resolution)||shape.NLevels()>16)return -1;
    int result=-1;
    for(int i=0;i<shape.NLevels();++i)if(shape.Resolution(i)==resolution && shape.IsNormalLevel(i) && shape.Level(i)) {
        if(result>=0)return -1;result=i;
    }
    return result;
}
// Actual MeshBuild capture and strict source comparison. No permitted recenter,
// reverse, approximate normal equality or vertex/face reorder in this slice.
// This confirms current payload equivalence only, not lifetime/static config.
inline RigidOdol7FinalStatus Geometry(const Shape& shape,const RigidOdol7VisualCandidate& source,
                                     ExportedMesh& destination)
{
    if(source.vertices.empty()||source.vertices.size()>4096||source.indices.empty()||
       source.indices.size()>8192*3 || source.indices.size()%3 ||
       source.vertices.size()!=source.positions.size() || source.triangleMaterials.size()!=source.indices.size()/3)
        return RigidOdol7FinalStatus::Capacity;
    if(shape.NVertex()!=int(source.vertices.size())||shape.NNorm()!=shape.NVertex()||
       shape.NTex()!=shape.NVertex()||shape.NSections()!=1||shape.NProxies()||shape.NAnimationPhases()||
       shape.NNamedSel()||shape.HasUV1()||shape.Special())return RigidOdol7FinalStatus::Unsupported;
    const auto& section=shape.GetSection(0);
    if(section.material!=0||uint32_t(section.properties.Special())!=source.sectionSpecial ||
       (source.sectionSpecial&~0x2000u))return RigidOdol7FinalStatus::Unsupported;
    if(section.beg!=shape.BeginFaces()||section.end!=shape.EndFaces())return RigidOdol7FinalStatus::Invalid;
    size_t triangles=0;Offset cursor=shape.BeginFaces();
    while(cursor<section.end) {
        const auto& face=shape.Face(cursor);
        if(face.N()!=3&&face.N()!=4)return RigidOdol7FinalStatus::Unsupported;
        if(uint32_t(face.Special())!=source.sectionSpecial)return RigidOdol7FinalStatus::Unsupported;
        if(size_t(face.N()-2)>8192-triangles)return RigidOdol7FinalStatus::Capacity;
        triangles+=size_t(face.N()-2);
        for(int i=0;i<face.N();++i)if(size_t(face.GetVertex(i))>=source.vertices.size())return RigidOdol7FinalStatus::Invalid;
        const auto previous=cursor;shape.NextFace(cursor);
        if(cursor<=previous||cursor>section.end)return RigidOdol7FinalStatus::Invalid;
    }
    if(triangles*3!=source.indices.size())return RigidOdol7FinalStatus::Unsupported;
    for(int i=0;i<shape.NVertex();++i)if(uint32_t(shape.Clip(i))&~0x3fu)return RigidOdol7FinalStatus::Unsupported;
    ExportedMesh result;result.vertices.resize(source.vertices.size());result.positions.resize(source.vertices.size());
    result.indices.resize(source.indices.size());result.materials.assign(triangles,0);
    render::mesh::BuildVertices(shape,result.vertices.data());
    std::vector<VertexIndex> packed(source.indices.size());render::mesh::BuildIndices(shape,packed.data());
    for(size_t i=0;i<packed.size();++i) {
        if(uint32_t(packed[i])!=source.indices[i]||source.indices[i]>=source.vertices.size())
            return RigidOdol7FinalStatus::Unsupported;
        result.indices[i]=uint32_t(packed[i]);
    }
    for(size_t i=0;i<result.vertices.size();++i) {
        const auto& v=result.vertices[i];const auto& raw=source.vertices[i];const auto& p=source.positions[i];
        if(p.x!=raw.position.x||p.y!=raw.position.y||p.z!=raw.position.z)
            return RigidOdol7FinalStatus::Invalid;
        if(v.pos.X()!=raw.position.x||v.pos.Y()!=raw.position.y||v.pos.Z()!=raw.position.z||
           v.norm.X()!=-raw.normal.x||v.norm.Y()!=-raw.normal.y||v.norm.Z()!=-raw.normal.z||
           v.t0.u!=raw.u||v.t0.v!=raw.v||v.t1.u!=raw.u||v.t1.v!=raw.v||v.conform)
            return RigidOdol7FinalStatus::Unsupported;
        for(float f:{v.pos.X(),v.pos.Y(),v.pos.Z(),v.norm.X(),v.norm.Y(),v.norm.Z(),v.t0.u,v.t0.v,v.t1.u,v.t1.v,
                     v.tangent.X(),v.tangent.Y(),v.tangent.Z(),v.binormal.X(),v.binormal.Y(),v.binormal.Z()})
            if(!std::isfinite(f))return RigidOdol7FinalStatus::Invalid;
        result.positions[i]={v.pos.X(),v.pos.Y(),v.pos.Z()};
    }
    for(auto material:source.triangleMaterials)if(material)return RigidOdol7FinalStatus::Invalid;
    destination=std::move(result);return RigidOdol7FinalStatus::Exported;
}
inline bool Material(const Shape& shape,const RigidOdol7VisualCandidate& source,
                     const RigidOdol7FinalMaterialHold& hold)
{
    const auto& section=shape.GetSection(0);const auto& f=hold.facts;
    if(!std::isfinite(f.emitterScale)||f.emitterScale<0)return false;
    for(auto value:f.foldedEmissive)if(!std::isfinite(value))return false;
    if(!hold.texture || hold.texture.GetRef()!=section.properties.GetTexture() ||
       hold.surface.GetRef()!=section.surfMat.GetRef() || !hold.materialBindingBirth || hold.materialBindingBirth==UINT64_MAX ||
       !SamePath(hold.texture->Name(),source.texturePath) ||
       !SamePath(hold.current.sourceName,source.texturePath) || !hold.current.handle || !hold.current.slotLease ||
       !f.complete || f.capturedGroups!=render::PreparedGpuSectionFacts::All ||
       !f.gpuOwned || !f.texturePresent || !f.materialFullyOpaque || f.alphaClass!=AlphaStats::Opaque || f.alphaHoles ||
       f.indexCount!=int(source.indices.size()) ||
       f.descriptor!=render::BuildRenderPassDescriptor(render::SplitLegacy(section.properties.Special())))return false;
    render::ResumableModelAdmission sourceGate;
    if(!sourceGate.Begin({1,0,1,0,true}) || !sourceGate.AddTexture(hold.current))return false;
    const auto decision=render::ClassifyPreparedGpuSection(f,hold.current.handle);
    return decision.route==render::PreparedGpuSectionRoute::Opaque && decision.variant==0 && decision.alphaRef==0;
}
inline bool SameMaterial(const RigidOdol7FinalMaterialHold& a,const RigidOdol7FinalMaterialHold& b)
{
    return a.texture.GetRef()==b.texture.GetRef() && a.surface.GetRef()==b.surface.GetRef() &&
        a.materialBindingBirth==b.materialBindingBirth && a.current.textureToken==b.current.textureToken &&
        a.current.sourceName==b.current.sourceName && a.current.member==b.current.member &&
        a.current.handle==b.current.handle && a.current.slotLease==b.current.slotLease &&
        a.facts.descriptor==b.facts.descriptor && a.facts.foldedEmissive==b.facts.foldedEmissive &&
        a.facts.emitterScale==b.facts.emitterScale;
}
}
// inspectMaterial(finalLevel, exactSection) must observe the ACTUAL owner binding
// without resolving/loading/uploading a new one. Called before and after capture;
// refusal/exception leaves destination unchanged. No Shape mutation/GPU work.
// Caller separately validates world/config/model birth and preserves original
// other-pass coverage at publication and every reuse. Export success is only a
// bounded source-compatible payload snapshot; it is NOT GPU coverage authority.
template<class InspectMaterial>
inline RigidOdol7FinalStatus ExportRigidOdol7FinalShape(const LODShape& shape,
    const RigidOdol7Candidate& candidate,uint32_t coarseSourceLod,uint32_t fineSourceLod,
    const RigidOdol7FinalSourceHold& sourceHold,InspectMaterial&& inspectMaterial,
    RigidOdol7FinalExport& destination)
{
    if(!Foundation::IsMainThread())return RigidOdol7FinalStatus::WrongOwner;
    try {
    if(!RigidOdol7FinalDetail::SourceCurrent(candidate,sourceHold))return RigidOdol7FinalStatus::StaleSource;
    if(candidate.source.decodedBytes>128*1024 || candidate.mapType!=11 ||
       !RigidOdol7FinalDetail::SamePath(shape.Name(),candidate.source.canonicalPath) ||
       shape.GetAllowAnimation() || shape.GetMapType()!=MapRock || shape.Special() ||
       (shape.Remarks()&REM_REVERSED) || !shape.QueryPolicyRevision() || coarseSourceLod==fineSourceLod)
        return RigidOdol7FinalStatus::Unsupported;
    const auto* coarse=RigidOdol7FinalDetail::FindSource(candidate,coarseSourceLod);
    const auto* fine=RigidOdol7FinalDetail::FindSource(candidate,fineSourceLod);
    if(!coarse||!fine||coarse->resolution<=fine->resolution)return RigidOdol7FinalStatus::Invalid;
    const int coarseLevel=RigidOdol7FinalDetail::FindFinal(shape,coarse->resolution);
    const int fineLevel=RigidOdol7FinalDetail::FindFinal(shape,fine->resolution);
    if(coarseLevel<0||fineLevel<0)return RigidOdol7FinalStatus::Unsupported;
    const auto& c=*shape.Level(coarseLevel);const auto& f=*shape.Level(fineLevel);
    const bool noShadow=c.HasNoShadowProperty();
    if(noShadow!=f.HasNoShadowProperty())return RigidOdol7FinalStatus::Unsupported;
        RigidOdol7FinalExport result;result.sourceAdmissionEpoch=sourceHold.sourceAdmissionEpoch;
        result.ownerEpoch=sourceHold.ownerEpoch;result.modelBirth=sourceHold.modelBirth;
        result.shapeQueryRevision=shape.QueryPolicyRevision();result.actualNoShadow=noShadow;
        result.finalLevels={coarseLevel,fineLevel};result.sourceLods={coarseSourceLod,fineSourceLod};
        result.resolutions={coarse->resolution,fine->resolution};
        auto status=RigidOdol7FinalDetail::Geometry(c,*coarse,result.actual.coarse);
        if(status!=RigidOdol7FinalStatus::Exported)return status;
        status=RigidOdol7FinalDetail::Geometry(f,*fine,result.actual.fine);
        if(status!=RigidOdol7FinalStatus::Exported)return status;
        const auto cm=inspectMaterial(coarseLevel,c.GetSection(0));
        const auto fm=inspectMaterial(fineLevel,f.GetSection(0));
        if(!RigidOdol7FinalDetail::Material(c,*coarse,cm)||!RigidOdol7FinalDetail::Material(f,*fine,fm)||
           !RigidOdol7FinalDetail::SameMaterial(cm,fm))return RigidOdol7FinalStatus::StaleMaterial;
        const auto afterC=inspectMaterial(coarseLevel,c.GetSection(0));
        const auto afterF=inspectMaterial(fineLevel,f.GetSection(0));
        if(!RigidOdol7FinalDetail::Material(c,*coarse,afterC)||!RigidOdol7FinalDetail::Material(f,*fine,afterF)||
           !RigidOdol7FinalDetail::SameMaterial(cm,afterC)||!RigidOdol7FinalDetail::SameMaterial(fm,afterF))
            return RigidOdol7FinalStatus::StaleMaterial;
        if(shape.QueryPolicyRevision()!=result.shapeQueryRevision || c.HasNoShadowProperty()!=noShadow ||
           f.HasNoShadowProperty()!=noShadow || !RigidOdol7FinalDetail::SourceCurrent(candidate,sourceHold))
            return RigidOdol7FinalStatus::StaleSource;
        if(!sourceHold.lease->Encoded().CopyMemberIdentity(result.sourceMember))return RigidOdol7FinalStatus::StaleSource;
        result.actual.source.sourceSha256=candidate.source.decodedSha256;
        result.actual.source.geometryOptions=0x4f3746494e414c31ull; // strict actual final Shape pair v1
        result.actual.source.materialOptions=uint64_t(coarse->sectionSpecial)|(uint64_t(noShadow)<<32);
        result.actual.source.vertexLayout=sizeof(SVertex);result.actual.source.materialMapping=1;
        result.actual.source.coarseRepresentation=coarseSourceLod;result.actual.source.fineRepresentation=fineSourceLod;
        result.material={cm.current.member,cm.current.textureToken,cm.current.handle,cm.materialBindingBirth,
            cm.current.slotLease,std::string(cm.current.sourceName),cm.facts.descriptor};
        destination=std::move(result);return RigidOdol7FinalStatus::Exported;
    } catch(const std::bad_alloc&) {return RigidOdol7FinalStatus::AllocationFailed;}
      catch(...) {return RigidOdol7FinalStatus::Invalid;}
}
}
