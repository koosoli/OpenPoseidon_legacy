#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageRigidOdol7FinalShape.hpp>
#include <Poseidon/IO/Streams/ArchiveSourceBinding.hpp>
#include <Poseidon/IO/Streams/PatnikArchiveReason.hpp>
#include <Poseidon/IO/Streams/RetailRigidAssetProfile.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/World/Model/ModelCache.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>

namespace Poseidon::GeometryPages
{
enum class RigidOdol7OwnerCaptureStatus
{
    Prepared, Disabled, WrongOwner, MissingSource, ReadFailed, InvalidSource,
    Unsupported, Capacity, StaleSource, AdaptFailed, AllocationFailed
};
class RigidOdol7OwnerCapture;
inline RigidOdol7OwnerCaptureStatus PrepareRigidOdol7OwnerCapture(
    RigidOdol7OwnerCapture& out,const Ref<Texture>* privatePrimary=nullptr);
namespace RigidOdol7OwnerDetail
{
// Paths select a bounded diagnostic profile; mounted leases and actual parsed
// bytes remain the authority. Invalid startup selection disables all capture.
inline const RetailRigidAssetProfile::Profile& Metadata()
{
    const auto* selected=RetailRigidAssetProfile::Selected();
    return selected?*selected:*RetailRigidAssetProfile::ById(RetailRigidAssetProfile::Id::SkalaNew);
}
inline const char* const ModelPath=Metadata().modelLogicalPath.data();
inline const char* const ModelMember=Metadata().modelMember.data();
inline const char* const PrimaryPac=Metadata().primaryTexturePath.data();
inline bool Enabled()
{
    const char* value=std::getenv("WGR_GEOMETRY_PAGE_RETAIL_SOURCE");
    return value && std::strcmp(value,"1")==0 && RetailRigidAssetProfile::Selected();
}
// Explicit native-bank diagnostic selection, not the global ModelCache/VFS
// precedence rule. No loose override, mount alias or failed open is followed.
inline QFBank* ResolveBank()
{
    if(!RetailRigidAssetProfile::Selected() || !Foundation::IsMainThread() || !GUseFileBanks)return nullptr;
    auto* bank=QIFStreamB::AutoBank(ModelPath);
    if(!bank || !RigidOdol7FinalDetail::SamePath((const char*)bank->GetPrefix(),"data3d\\"))return nullptr;
    const char* name=bank->GetOpenName();
    if(!name)return nullptr;
    const size_t bytes=std::strlen(name);
    constexpr std::string_view suffix="dta\\data3d.pbo";
    if(bytes<suffix.size() || bytes>=1024 ||
       !RigidOdol7FinalDetail::SamePath(std::string_view(name+bytes-suffix.size(),suffix.size()),suffix) ||
       (bytes>suffix.size() && name[bytes-suffix.size()-1]!='\\' && name[bytes-suffix.size()-1]!='/'))return nullptr;
    return bank;
}
inline bool ParseHash(const std::string& hex,std::array<uint8_t,32>& out)
{
    if(hex.size()!=64)return false;
    auto digit=[](char c)->int {if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;
        if(c>='A'&&c<='F')return c-'A'+10;return -1;};
    std::array<uint8_t,32> result{};bool nonzero=false;
    for(size_t i=0;i<result.size();++i) {
        const int a=digit(hex[2*i]),b=digit(hex[2*i+1]);if(a<0||b<0)return false;
        result[i]=uint8_t(a*16+b);nonzero|=result[i]!=0;
    }
    if(!nonzero)return false;out=result;return true;
}
// Additional finite whole-shape limits BEFORE live bank lookup/adaptation.
// Candidate's visual limits alone do not bound retained collision/roadway LODs.
inline bool WithinAdaptBudget(const Model::Model& model)
{
    if(model.lodLevels.size()>16)return false;
    size_t vertices=0,faces=0,selections=0;
    for(const auto& lod:model.lodLevels) {
        const auto& mesh=lod.mesh;
        if(mesh.vertices.size()>8192-vertices || mesh.triangles.size()>8192-faces ||
           mesh.quads.size()>8192-faces-mesh.triangles.size() ||
           mesh.selections.size()>128-selections || mesh.materials.size()>16 || mesh.sections.size()>16 ||
           !mesh.proxies.empty() || !mesh.frames.empty())return false;
        vertices+=mesh.vertices.size();faces+=mesh.triangles.size()+mesh.quads.size();
        selections+=mesh.selections.size();
        for(const auto& selection:mesh.selections)
            if(selection.vertexIndices.size()>8192 || selection.vertexWeights.size()>8192 ||
               selection.triangleIndices.size()>8192 || selection.sectionIndices.size()>16 ||
               selection.name.size()>128)return false;
    }
    return true;
}
}
// One private main-owner capture. This bypasses ShapeBank's shared cached shape,
// but uses the actual live adapter, bank resources and normal ODOL tail. It does
// not install an Object/model, authorize any GPU coverage, or replace other LODs.
// Retire/destroy on the owner thread while the application's texture banks live.
// Source lease/decoded byte debt is held until this object really exits; no bank
// pointer is retained. The mutable renderer birth/epochs remain caller authority.
class RigidOdol7OwnerCapture
{
    friend RigidOdol7OwnerCaptureStatus PrepareRigidOdol7OwnerCapture(
        RigidOdol7OwnerCapture&,const Ref<Texture>*);
    std::vector<char> _decoded;
    std::shared_ptr<const BankCompressedReadRequest> _lease;
    RigidOdol7Candidate _candidate;
    std::unique_ptr<LODShapeWithShadow> _shape;
public:
    RigidOdol7OwnerCapture()=default;
    RigidOdol7OwnerCapture(const RigidOdol7OwnerCapture&)=delete;
    RigidOdol7OwnerCapture& operator=(const RigidOdol7OwnerCapture&)=delete;
    RigidOdol7OwnerCapture(RigidOdol7OwnerCapture&&) noexcept=default;
    RigidOdol7OwnerCapture& operator=(RigidOdol7OwnerCapture&&) noexcept=default;
    const RigidOdol7Candidate& Candidate() const {return _candidate;}
    const LODShapeWithShadow* FinalShape() const {return _shape.get();}
    const std::vector<char>& DecodedBytes() const {return _decoded;}
    const std::shared_ptr<const BankCompressedReadRequest>& Lease() const {return _lease;}
    size_t RetainedDecodedCapacityBytes() const {return _decoded.capacity()*sizeof(char);}
    // Short-lived synchronous export hold only. Never persist its QFBank* in a
    // fixture, request, callback, worker result or frame-to-frame state. Owner
    // must not remount/mutate banks while using this hold/inspector synchronously.
    std::optional<RigidOdol7FinalSourceHold> CurrentSourceHold(
        uint64_t sourceAdmissionEpoch,uint64_t ownerEpoch,uint64_t modelBirth) const
    {
        if(!RigidOdol7OwnerDetail::Enabled() || !Foundation::IsMainThread() || !_shape || !_lease ||
           !sourceAdmissionEpoch || !ownerEpoch || !modelBirth || sourceAdmissionEpoch==UINT64_MAX ||
           ownerEpoch==UINT64_MAX || modelBirth==UINT64_MAX)return {};
        auto* bank=RigidOdol7OwnerDetail::ResolveBank();
        if(!bank || !bank->MatchesMountedCompressedMember(RigidOdol7OwnerDetail::ModelMember,*_lease))return {};
        RigidOdol7FinalSourceHold hold{_lease,bank,_candidate.source.decodedSha256,
            sourceAdmissionEpoch,ownerEpoch,modelBirth};
        if(!RigidOdol7FinalDetail::SourceCurrent(_candidate,hold))return {};
        return hold;
    }
};
// Transactional output; source/header/material reads are explicit bounded owner
// diagnostic work. No EnsureUploaded, synthetic alpha facts, cached-Init retrofit,
// worker, pages, model registration or visual/all-pass routing occurs here.
// Optional privatePrimary changes only the exact authored primary PAC refs in
// the local adapter tables. The caller must prove on this same strong Ref both
// before and after capture that its private Init-born source is current. Texture's
// public interface cannot certify archive birth or native handle; this method
// checks exact name, cold state and positive dimensions, and never retrofits
// the global texture cache. Material staging remains a subsequent caller step.
inline RigidOdol7OwnerCaptureStatus PrepareRigidOdol7OwnerCapture(
    RigidOdol7OwnerCapture& out,const Ref<Texture>* privatePrimary)
{
    using Status=RigidOdol7OwnerCaptureStatus;
    if(!RigidOdol7OwnerDetail::Enabled())return Status::Disabled;
    if(!Foundation::IsMainThread())return Status::WrongOwner;
    if(privatePrimary && (!privatePrimary->GetRef() ||
        !PatnikArchiveReason::Matches(RigidOdol7OwnerDetail::PrimaryPac,(*privatePrimary)->Name()) ||
        (*privatePrimary)->IsGpuResident() || (*privatePrimary)->AWidth(0)<=0 ||
        (*privatePrimary)->AHeight(0)<=0))return Status::InvalidSource;
    try {
        auto* bank=RigidOdol7OwnerDetail::ResolveBank();if(!bank)return Status::MissingSource;
        auto lease=bank->CaptureCompressedReadRequest(RigidOdol7OwnerDetail::ModelMember,
            BankCompressedReadRequest::MaxEncodedBytes,BankCompressedReadRequest::MaxDecodedBytes);
        if(!lease)return Status::MissingSource;
        RigidOdol7OwnerCapture result;
        result._lease=std::make_shared<const BankCompressedReadRequest>(std::move(*lease));
        std::string hex;
        if(!result._lease->ReadDecoded(result._decoded,hex))return Status::ReadFailed;
        if(result._decoded.empty() || result._decoded.size()!=result._lease->DecodedBytes() ||
           result._decoded.capacity()>BankCompressedReadRequest::MaxDecodedBytes)return Status::Capacity;
        RigidOdol7SourceBinding source;source.canonicalPath=RigidOdol7OwnerDetail::ModelPath;
        source.decodedBytes=result._decoded.size();
        if(!RigidOdol7OwnerDetail::ParseHash(hex,source.decodedSha256))return Status::InvalidSource;
        std::string error;
        auto parsed=ModelCache::LoadOwnedBytes(result._decoded.data(),result._decoded.size(),source.canonicalPath,error);
        if(!parsed)return Status::InvalidSource;
        const auto candidate=InspectRigidOdol7Candidate(*parsed,source,result._candidate);
        if(candidate==RigidOdol7Status::Capacity || !RigidOdol7OwnerDetail::WithinAdaptBudget(*parsed))return Status::Capacity;
        if(candidate!=RigidOdol7Status::Candidate)return Status::Unsupported;
        // Re-resolve after parsing; no pointer into the relocatable bank array is
        // carried over any live adapter/config/texture operation.
        bank=RigidOdol7OwnerDetail::ResolveBank();
        if(!bank || !bank->MatchesMountedCompressedMember(RigidOdol7OwnerDetail::ModelMember,*result._lease))
            return Status::StaleSource;
        {
            ShapeBank::CpuOnlyLoadScope cpuOnly;
            ArchiveSourceBinding::RetailPacReadScope pacPurpose(RigidOdol7OwnerDetail::ModelPath);
            Model::ShapeAdapter::AdapterBankTables tables;
            Model::ShapeAdapter::BuildAdapterBankTables(*parsed,tables);
            if(privatePrimary) {
                if(tables.textures.size()!=parsed->lodLevels.size())return Status::AdaptFailed;
                size_t replaced=0;
                for(size_t lod=0;lod<parsed->lodLevels.size();++lod) {
                    const auto& materials=parsed->lodLevels[lod].mesh.materials;
                    auto& row=tables.textures[lod];
                    if(row.size()!=materials.size())return Status::AdaptFailed;
                    for(size_t mat=0;mat<materials.size();++mat) {
                        if(!PatnikArchiveReason::Matches(RigidOdol7OwnerDetail::PrimaryPac,
                            materials[mat].texturePath.c_str()))continue;
                        if(!row[mat] || !PatnikArchiveReason::Matches(RigidOdol7OwnerDetail::PrimaryPac,
                            row[mat]->Name()))return Status::AdaptFailed;
                        row[mat]=*privatePrimary;
                        ++replaced;
                    }
                }
                if(!replaced)return Status::AdaptFailed;
            }
            result._shape.reset(Model::ShapeAdapter::convertToLODShape(*parsed,false,&tables,true));
        }
        if(!result._shape || !RigidOdol7FinalDetail::SamePath(result._shape->Name(),source.canonicalPath))
            return Status::AdaptFailed;
        bank=RigidOdol7OwnerDetail::ResolveBank();
        if(!bank || !bank->MatchesMountedCompressedMember(RigidOdol7OwnerDetail::ModelMember,*result._lease))
            return Status::StaleSource;
        // parsed and tables are released here; actual final Shape and candidate
        // retain the payload. Capacity is byte storage, never a claimed GPU cost.
        out=std::move(result);return Status::Prepared;
    } catch(const std::bad_alloc&) {return Status::AllocationFailed;}
      catch(...) {return Status::InvalidSource;}
}
}
