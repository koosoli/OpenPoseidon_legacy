#pragma once
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/Foundation/Algorithms/Sha256.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/IO/Streams/RetailRigidAssetProfile.hpp>
#include <array>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <span>

namespace Poseidon
{
// Separate compressed-model proof, never a raw PAC binding. Only QFBank's
// successful decoded read may create it. It retains no QFBank pointer and does
// not certify current mounts, parsed geometry, materials or GPU resources.
// Source caps are per encoded/decoded buffer (128KiB each). Capture transiently
// re-decodes beside the ordinary read: up to four such buffers can coexist;
// the 64KiB pool bounds retained proof metadata, not total parser RSS/latency.
class ModelCompressedSourceBirth
{
    friend class QFBank;
    struct Ticket {
        uint64_t bytes=0;
        explicit Ticket(uint64_t b):bytes(b){}
        ~Ticket(){LiveBytes.fetch_sub(bytes);Live.fetch_sub(1);}
    };
    inline static std::atomic<uint32_t> Live{0};
    inline static std::atomic<uint64_t> LiveBytes{0},NextToken{1};
    inline static thread_local uint64_t CurrentToken=0;
    inline static thread_local unsigned Depth=0;
    std::shared_ptr<const Ticket> _ticket;
    ModelCompressedSourceBirth(std::shared_ptr<const BankCompressedReadRequest> source,
        std::array<uint8_t,32> digest,uint64_t token,std::shared_ptr<const Ticket> ticket,
        const RetailRigidAssetProfile::Profile& profile)
        :_ticket(std::move(ticket)),lease(std::move(source)),decodedSha256(digest),
         decodedBytes(lease->DecodedBytes()),logicalName(profile.modelLogicalPath),
         readScopeToken(token),profileId(profile.id){}
    static std::shared_ptr<const ModelCompressedSourceBirth> Capture(
        BankCompressedReadRequest source,std::span<const char> actual)
    {
        const auto* profile=RetailRigidAssetProfile::Selected();
        if(!profile||!ReadScope::Active()||!source.Encoded().HasArchiveIdentity()||actual.empty()||
           actual.size()!=source.DecodedBytes()||actual.size()>BankCompressedReadRequest::MaxDecodedBytes||
           source.Encoded().bytes>BankCompressedReadRequest::MaxEncodedBytes||
           !profile->MatchesModelMember(source.CanonicalMember())||
           !profile->MatchesModelArchive(source.Encoded().archive))return {};
        try {
            const uint64_t bytes=sizeof(ModelCompressedSourceBirth)+sizeof(BankCompressedReadRequest)+
                sizeof(Ticket)+source.Encoded().archive.capacity()+source.CanonicalMember().capacity()+
                source.Encoded().ArchiveIdentityBytes()+256;
            if(bytes>MaxMetadataBytes)return {};
            uint32_t count=Live.load();
            do{if(count>=MaxLive)return {};}while(!Live.compare_exchange_weak(count,count+1));
            uint64_t used=LiveBytes.load();
            do{if(used>MaxMetadataBytes||bytes>MaxMetadataBytes-used){Live.fetch_sub(1);return {};}}
            while(!LiveBytes.compare_exchange_weak(used,used+bytes));
            std::shared_ptr<const Ticket> ticket;
            try{ticket=std::make_shared<const Ticket>(bytes);}
            catch(...){LiveBytes.fetch_sub(bytes);Live.fetch_sub(1);return {};}
            std::vector<char> decoded;std::string digest;
            if(!source.ReadDecoded(decoded,digest)||decoded.size()!=actual.size()||
               std::memcmp(decoded.data(),actual.data(),actual.size())||
               digest!=Foundation::Sha256::Of(actual.data(),actual.size()))return {};
            std::array<uint8_t,32> raw{};
            if(digest.size()!=64)return {};
            const auto hex=[](char c)->int{return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};
            for(size_t i=0;i<raw.size();++i){const int a=hex(digest[i*2]),b=hex(digest[i*2+1]);
                if(a<0||b<0)return {};raw[i]=uint8_t((a<<4)|b);}
            auto lease=std::make_shared<const BankCompressedReadRequest>(std::move(source));
            return std::shared_ptr<const ModelCompressedSourceBirth>(
                new ModelCompressedSourceBirth(std::move(lease),raw,CurrentToken,std::move(ticket),*profile));
        }catch(...){return {};}
    }
public:
    static constexpr uint32_t MaxLive=8;
    static constexpr uint64_t MaxMetadataBytes=64*1024;
    const std::shared_ptr<const BankCompressedReadRequest> lease;
    const std::array<uint8_t,32> decodedSha256;
    const uint32_t decodedBytes;
    const std::string logicalName;
    const uint64_t readScopeToken;
    const RetailRigidAssetProfile::Id profileId;
    class ReadScope
    {
        uint64_t _previous=0;bool _entered=false;
    public:
        static bool Enabled(){const char* e=std::getenv("WGR_GEOMETRY_PAGE_RETAIL_COLD_SOURCE");
            return e&&std::strcmp(e,"1")==0&&RetailRigidAssetProfile::Selected()!=nullptr;}
        static bool MatchesLogicalName(const char* name)
        {
            if(!name)return false;
            const auto* candidate=RetailRigidAssetProfile::ByModelPath(name);
            return candidate&&candidate==RetailRigidAssetProfile::Selected();
        }
        explicit ReadScope(const char* exactLogicalName)
        {
            if(!exactLogicalName||!RetailRigidAssetProfile::ByModelPath(exactLogicalName)||
               !Enabled()||!MatchesLogicalName(exactLogicalName)||
               !Foundation::IsMainThread()||Depth>=8)return;
            uint64_t next=NextToken.load();
            do{if(!next||next==UINT64_MAX)return;}while(!NextToken.compare_exchange_weak(next,next+1));
            _previous=CurrentToken;CurrentToken=next;++Depth;_entered=true;
        }
        ~ReadScope(){if(_entered){CurrentToken=_previous;--Depth;}}
        ReadScope(const ReadScope&)=delete;ReadScope& operator=(const ReadScope&)=delete;
        static bool Active(){return Depth&&CurrentToken&&Foundation::IsMainThread()&&Enabled();}
        static uint64_t Token(){return Active()?CurrentToken:0;}
    };
    bool Valid() const
    {
        const auto* profile=RetailRigidAssetProfile::ById(profileId);
        return _ticket&&lease&&readScopeToken&&readScopeToken!=UINT64_MAX&&decodedBytes&&
            decodedBytes<=BankCompressedReadRequest::MaxDecodedBytes&&decodedBytes==lease->DecodedBytes()&&
            profile&&profile==RetailRigidAssetProfile::Selected()&&
            lease->Encoded().HasArchiveIdentity()&&
            profile->MatchesModelMember(lease->CanonicalMember())&&
            profile->MatchesModelArchive(lease->Encoded().archive)&&
            profile->MatchesModelPath(logicalName)&&
            std::any_of(decodedSha256.begin(),decodedSha256.end(),[](auto b){return b!=0;});
    }
    uint64_t KnownCppBytes() const{return _ticket?_ticket->bytes:0;}
};
}
