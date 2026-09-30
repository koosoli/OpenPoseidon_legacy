#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPagePrototype.hpp>
#include <Poseidon/World/Model/StaticSourceAudit.hpp>
#include <array>
#include <new>
#include <string>
#include <type_traits>

namespace Poseidon::GeometryPages
{
// Pure source inspection / offline raw-IR export. This is NOT eligibility for a
// live config-backed Object, a mounted-source lease, opaque texture evidence,
// SVertex packing, final Shape/material classification or render admission.
// The caller supplies the SHA/size/path of the SAME decoded source used by the
// audited loader. A boolean IR inspection cannot authenticate that association.
struct RigidOdol7SourceBinding
{
    std::array<uint8_t,32> decodedSha256{};
    size_t decodedBytes=0;
    std::string canonicalPath;
};
// Distinct from SVertex: stored normals/UVs, no normal sign change, generated
// tangent frame, recentering or renderer basis claim. MeshInput.positions is
// exactly this record's model-space position; only the offline raw-IR contract.
struct RigidOdol7IrVertex
{
    Position position, normal;
    float u=0,v=0;
};
static_assert(std::is_trivially_copyable_v<RigidOdol7IrVertex>);
static_assert(sizeof(RigidOdol7IrVertex)==8*sizeof(float));
struct RigidOdol7VisualCandidate
{
    uint32_t sourceLod=0, sectionSpecial=0;
    float resolution=0;
    bool lodNoShadow=false;
    Model::GeometryBasis sourceBasis; // retains Unknown; never promotes confidence
    std::string texturePath;
    std::vector<Position> positions;
    std::vector<RigidOdol7IrVertex> vertices;
    std::vector<uint32_t> indices, triangleMaterials;
    MeshInput Input() const
    {
        return {positions,{reinterpret_cast<const uint8_t*>(vertices.data()),
            vertices.size()*sizeof(RigidOdol7IrVertex)},sizeof(RigidOdol7IrVertex),indices,triangleMaterials};
    }
};
struct RigidOdol7LodEvidence
{
    uint32_t sourceLod=0,vertices=0,faces=0;
    float resolution=0;
    Model::LodPurpose purpose=Model::LodPurpose::Unknown;
};
struct RigidOdol7Candidate
{
    RigidOdol7SourceBinding source;
    uint32_t mapType=0;
    std::array<int8_t,12> sourceRoleLods{}; // directory roles, distinct from resolution purpose
    std::vector<RigidOdol7LodEvidence> lodEvidence; // no nonvisual payload copied/replaced
    std::vector<RigidOdol7VisualCandidate> visual;
};
enum class RigidOdol7Status { Candidate, Unsupported, Invalid, Capacity, AllocationFailed };
namespace RigidOdol7Detail
{
inline bool Path(const std::string& path)
{
    if(path.empty() || path.size()>=128 || path.find("..")!=std::string::npos)return false;
    for(unsigned char c:path)if(c<32 || c>=127)return false;
    return true;
}
inline bool Finite(Model::Vector3 value)
{
    return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z);
}
inline RigidOdol7Status Visual(const Model::LODLevel& lod,uint32_t index,
                              size_t& vertexTotal,size_t& triangleTotal,
                              RigidOdol7VisualCandidate& out)
{
    const auto& mesh=lod.mesh;
    if(!mesh.selections.empty() || !mesh.proxies.empty() || !mesh.frames.empty() ||
       !mesh.vertexMass.empty() || !lod.uvChannels.empty() || lod.uvSetCount!=0 ||
       uint32_t(mesh.orHints)||uint32_t(mesh.andHints)||uint32_t(mesh.special))
        return RigidOdol7Status::Unsupported;
    if(mesh.vertices.empty() || mesh.vertices.size()>4096-vertexTotal ||
       mesh.triangles.size()>8192 || mesh.quads.size()>4096 ||
       mesh.triangles.size()+mesh.quads.size()*2>8192-triangleTotal)
        return RigidOdol7Status::Capacity;
    if(mesh.sections.size()!=1 || mesh.materials.size()!=1)return RigidOdol7Status::Unsupported;
    const auto& section=mesh.sections[0];const auto& material=mesh.materials[0];
    const auto special=uint32_t(section.hints);
    // Accept only tiling on/off. No alpha, lighting, hidden, animated or biased
    // face/section semantics are discarded. Final texture opacity is still unknown.
    if(section.materialIndex!=0 || section.specialMaterial!=0 || (special&~0x2000u) ||
       !Path(material.texturePath) || !material.materialPath.empty() ||
       !material.embeddedStages.empty() || uint32_t(material.flags) ||
       material.metallic!=0 || material.roughness!=1 || material.emissive!=0)
        return RigidOdol7Status::Unsupported;
    if(mesh.properties.size()>16)return RigidOdol7Status::Capacity;
    bool noShadow=false;
    for(const auto& property:mesh.properties) {
        if(property.name!="lodnoshadow" || property.value!="1" || noShadow)
            return RigidOdol7Status::Unsupported;
        noShadow=true;
    }
    // ODOL7 Section.startTriangle/triangleCount are BYTE ranges in the original
    // x86 Poly stream, not IR triangle counts. Reconstruct exact original face
    // order, including the quad fan (0,1,2),(0,2,3), matching MeshBuild.
    struct Face {uint32_t order;bool quad;size_t at;};
    std::vector<Face> faces;faces.reserve(mesh.triangles.size()+mesh.quads.size());
    for(size_t i=0;i<mesh.triangles.size();++i)faces.push_back({mesh.triangles[i].originalIndex,false,i});
    for(size_t i=0;i<mesh.quads.size();++i)faces.push_back({mesh.quads[i].originalIndex,true,i});
    if(faces.empty())return RigidOdol7Status::Invalid;
    std::sort(faces.begin(),faces.end(),[](const Face& a,const Face& b){return a.order<b.order;});
    uint32_t byteEnd=0;
    for(size_t i=0;i<faces.size();++i) {
        if(faces[i].order!=i)return RigidOdol7Status::Invalid;
        byteEnd+=8+2*(1+(faces[i].quad?4:3));
    }
    if(section.faceRangeKnown || section.startTriangle!=0 || section.triangleCount!=byteEnd)
        return RigidOdol7Status::Invalid;
    // Source ODOL7 may retain vertices that no face uses. An unused zero
    // normal is inert but must remain byte-exact for final Shape comparison;
    // every vertex that can reach a triangle still needs a nonzero normal.
    std::array<uint8_t,4096> referenced{};
    for(const auto& face:faces) {
        const auto mark=[&](const auto& polygon,size_t corners) {
            for(size_t corner=0;corner<corners;++corner) {
                const uint32_t vertex=polygon.indices[corner];
                if(vertex>=mesh.vertices.size())return false;
                referenced[vertex]=1;
            }
            return true;
        };
        if(face.quad ? !mark(mesh.quads[face.at],4) : !mark(mesh.triangles[face.at],3))
            return RigidOdol7Status::Invalid;
    }
    out.sourceLod=index;out.resolution=lod.resolution;out.sectionSpecial=special;
    out.lodNoShadow=noShadow;out.texturePath=material.texturePath;out.sourceBasis=lod.basis;
    out.vertices.reserve(mesh.vertices.size());out.positions.reserve(mesh.vertices.size());
    for(size_t vertexIndex=0;vertexIndex<mesh.vertices.size();++vertexIndex) {
        const auto& vertex=mesh.vertices[vertexIndex];
        if((uint32_t(vertex.flags)&~0x3fu) || vertex.hasTangentFrame ||
           vertex.uv1.u!=0 || vertex.uv1.v!=0)
            return RigidOdol7Status::Unsupported;
        if(!Finite(vertex.position)||!Finite(vertex.normal)||
           !std::isfinite(vertex.uv.u)||!std::isfinite(vertex.uv.v))return RigidOdol7Status::Invalid;
        const auto p=vertex.position,n=vertex.normal;
        if(std::abs(p.x)>10000 || std::abs(p.y)>10000 || std::abs(p.z)>10000)
            return RigidOdol7Status::Unsupported;
        if(referenced[vertexIndex] && double(n.x)*n.x+double(n.y)*n.y+double(n.z)*n.z<=0)
            return RigidOdol7Status::Invalid;
        out.positions.push_back({p.x,p.y,p.z});
        out.vertices.push_back({{p.x,p.y,p.z},{n.x,n.y,n.z},vertex.uv.u,vertex.uv.v});
    }
    const size_t triangles=mesh.triangles.size()+mesh.quads.size()*2;
    out.indices.reserve(triangles*3);out.triangleMaterials.assign(triangles,0);
    auto append=[&](const auto& face,size_t corners) {
        if(face.materialIndex!=0 || uint32_t(face.flags)!=special)return RigidOdol7Status::Unsupported;
        for(size_t i=0;i<corners;++i)if(face.indices[i]>=mesh.vertices.size())return RigidOdol7Status::Invalid;
        for(size_t i=2;i<corners;++i) {
            out.indices.push_back(face.indices[0]);out.indices.push_back(face.indices[i-1]);
            out.indices.push_back(face.indices[i]);
        }
        return RigidOdol7Status::Candidate;
    };
    for(const auto& face:faces) {
        const auto result=face.quad?append(mesh.quads[face.at],4):append(mesh.triangles[face.at],3);
        if(result!=RigidOdol7Status::Candidate)return result;
    }
    vertexTotal+=mesh.vertices.size();triangleTotal+=triangles;
    return RigidOdol7Status::Candidate;
}
}
// Destination changes only on success. No VFS/cache/config/material reads,
// Shape construction, hierarchy bake, GPU call or simulation mutation.
inline RigidOdol7Status InspectRigidOdol7Candidate(const Model::Model& model,
    const RigidOdol7SourceBinding& source,RigidOdol7Candidate& destination)
{
    bool hash=false;for(auto b:source.decodedSha256)hash|=b!=0;
    if(!hash || !source.decodedBytes || source.decodedBytes>128*1024 ||
       !RigidOdol7Detail::Path(source.canonicalPath) || model.sourcePath!=source.canonicalPath)
        return RigidOdol7Status::Invalid;
    if(model.sourceVersion!=7 || !Model::HasStaticOdolSourceAudit(model) || model.allowAnimation ||
       model.special!=0 || uint32_t(model.orHints)||uint32_t(model.andHints) ||
       model.mapType!=11)return RigidOdol7Status::Unsupported; // ODOL7 MapRock;22 is MapHide
    if(model.lodLevels.empty()||model.lodLevels.size()>16)return RigidOdol7Status::Capacity;
    try {
        RigidOdol7Candidate result;result.source=source;result.mapType=model.mapType;
        result.sourceRoleLods={model.memoryIdx,model.geometryIdx,model.geometryFireIdx,model.geometryViewIdx,
            model.geometryViewPilotIdx,model.geometryViewGunnerIdx,model.geometryViewCommanderIdx,
            model.geometryViewCargoIdx,model.landContactIdx,model.roadwayIdx,model.pathsIdx,model.hitpointsIdx};
        for(auto role:result.sourceRoleLods)if(role < -1 || role>=int(model.lodLevels.size()))
            return RigidOdol7Status::Invalid;
        result.lodEvidence.reserve(model.lodLevels.size());result.visual.reserve(model.lodLevels.size());
        size_t vertexTotal=0,triangleTotal=0;float previous=-1;
        for(size_t i=0;i<model.lodLevels.size();++i) {
            const auto& lod=model.lodLevels[i];const auto& mesh=lod.mesh;
            if(!std::isfinite(lod.resolution)||lod.resolution<0 ||
               lod.purpose!=Model::ClassifyLodResolution(lod.resolution))return RigidOdol7Status::Invalid;
            if(!mesh.frames.empty()||!mesh.proxies.empty())return RigidOdol7Status::Unsupported;
            if(mesh.vertices.size()>8192 || mesh.triangles.size()>8192 || mesh.quads.size()>8192)
                return RigidOdol7Status::Capacity;
            result.lodEvidence.push_back({uint32_t(i),uint32_t(mesh.vertices.size()),
                uint32_t(mesh.triangles.size()+mesh.quads.size()),lod.resolution,lod.purpose});
            if(lod.purpose!=Model::LodPurpose::Visual) {
                if(lod.purpose!=Model::LodPurpose::Geometry && lod.purpose!=Model::LodPurpose::ViewGeometry &&
                   lod.purpose!=Model::LodPurpose::FireGeometry && lod.purpose!=Model::LodPurpose::Roadway)
                    return RigidOdol7Status::Unsupported;
                continue;
            }
            if(lod.resolution<=previous)return RigidOdol7Status::Invalid;
            previous=lod.resolution;RigidOdol7VisualCandidate visual;
            const auto status=RigidOdol7Detail::Visual(lod,uint32_t(i),vertexTotal,triangleTotal,visual);
            if(status!=RigidOdol7Status::Candidate)return status;
            if(!result.visual.empty() && visual.texturePath!=result.visual[0].texturePath)
                return RigidOdol7Status::Unsupported;
            result.visual.push_back(std::move(visual));
        }
        if(result.visual.empty())return RigidOdol7Status::Unsupported;
        destination=std::move(result);return RigidOdol7Status::Candidate;
    } catch(const std::bad_alloc&) {return RigidOdol7Status::AllocationFailed;}
}
}
