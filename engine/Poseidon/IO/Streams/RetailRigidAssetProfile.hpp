#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace Poseidon::RetailRigidAssetProfile
{
// This is a two-name opt-in selector, not source or material authority. A
// successful lookup never substitutes for an actual parser-born buffer/lease,
// current mounted member, initialized PAC binding, final Shape comparison,
// or the ordinary world's instance/coverage checks.
enum class Id : uint8_t { SkalaNew=1, Skala2=2 };

namespace Detail
{
constexpr char Fold(char c)
{
    if(c=='/')return '\\';
    return c>='A'&&c<='Z'?char(c-'A'+'a'):c;
}
constexpr bool Exact(std::string_view observed,std::string_view expected)
{
    if(observed.empty()||observed.size()!=expected.size())return false;
    for(std::size_t i=0;i<expected.size();++i)
        if(Fold(observed[i])!=Fold(expected[i]))return false;
    return true;
}
constexpr bool ArchiveSuffix(std::string_view observed,std::string_view suffix)
{
    if(observed.size()<suffix.size()||observed.size()>=1024||suffix.empty())return false;
    const std::size_t start=observed.size()-suffix.size();
    if(start&&Fold(observed[start-1])!='\\')return false;
    return Exact(observed.substr(start),suffix);
}
constexpr std::string_view Bounded(const char* observed)
{
    if(!observed)return {};
    std::size_t length=0;
    while(length<8192&&observed[length])++length;
    return length<8192?std::string_view(observed,length):std::string_view{};
}
}

struct Profile
{
    Id id;
    std::string_view modelLogicalPath,modelMember,modelArchiveSuffix;
    std::string_view primaryTexturePath,textureMember,textureArchiveSuffix;
    uint32_t visualLevels=0,fineSourceLod=0,coarseSourceLod=0;
    bool selectedNoShadow=false;

    constexpr bool MatchesModelPath(std::string_view path) const
    {return Detail::Exact(path,modelLogicalPath);}
    constexpr bool MatchesModelPath(const char* path) const
    {return MatchesModelPath(Detail::Bounded(path));}
    constexpr bool MatchesModelMember(std::string_view member) const
    {return Detail::Exact(member,modelMember);}
    constexpr bool MatchesModelMember(const char* member) const
    {return MatchesModelMember(Detail::Bounded(member));}
    constexpr bool MatchesModelArchive(std::string_view path) const
    {return Detail::ArchiveSuffix(path,modelArchiveSuffix);}
    constexpr bool MatchesModelArchive(const char* path) const
    {return MatchesModelArchive(Detail::Bounded(path));}
    constexpr bool MatchesTexturePath(std::string_view path) const
    {return Detail::Exact(path,primaryTexturePath);}
    constexpr bool MatchesTexturePath(const char* path) const
    {return MatchesTexturePath(Detail::Bounded(path));}
    constexpr bool MatchesTextureMember(std::string_view member) const
    {return Detail::Exact(member,textureMember);}
    constexpr bool MatchesTextureMember(const char* member) const
    {return MatchesTextureMember(Detail::Bounded(member));}
    constexpr bool MatchesTextureArchive(std::string_view path) const
    {return Detail::ArchiveSuffix(path,textureArchiveSuffix);}
    constexpr bool MatchesTextureArchive(const char* path) const
    {return MatchesTextureArchive(Detail::Bounded(path));}
};

inline constexpr std::array<Profile,2> Entries{{
    {Id::SkalaNew, R"(data3d\skala_new.p3d)","skala_new.p3d",R"(DTA\data3d.pbo)",
        R"(data\skala_piskovec2.pac)","skala_piskovec2.pac",R"(DTA\data.pbo)",4,0,1,true},
    {Id::Skala2, R"(data3d\skala2.p3d)","skala2.p3d",R"(DTA\data3d.pbo)",
        R"(data\piskovec.pac)","piskovec.pac",R"(DTA\data.pbo)",3,0,1,false}
}};

constexpr const Profile* ById(Id id)
{
    for(const auto& entry:Entries)if(entry.id==id)return &entry;
    return nullptr;
}
constexpr const Profile* ByModelPath(std::string_view path)
{
    for(const auto& entry:Entries)if(entry.MatchesModelPath(path))return &entry;
    return nullptr;
}
constexpr const Profile* ByModelPath(const char* path)
{return ByModelPath(Detail::Bounded(path));}
constexpr const Profile* ByModelMember(std::string_view member)
{
    for(const auto& entry:Entries)if(entry.MatchesModelMember(member))return &entry;
    return nullptr;
}
constexpr const Profile* ByModelMember(const char* member)
{return ByModelMember(Detail::Bounded(member));}

// Process-start selector. An absent variable preserves the original Skala
// experiment; any explicit unknown/empty value disables profile capture.
// Selection is metadata only. It cannot authenticate bytes or a live mount.
inline const Profile* Selected()
{
    static const Profile* selected=[]() -> const Profile* {
        const char* value=std::getenv("WGR_GEOMETRY_PAGE_RETAIL_ASSET");
        if(!value)return ById(Id::SkalaNew);
        if(std::strcmp(value,"skala_new")==0)return ById(Id::SkalaNew);
        if(std::strcmp(value,"skala2")==0)return ById(Id::Skala2);
        return nullptr;
    }();
    return selected;
}
// Non-authoritative literal fallback for owner paths that must remain valid
// strings even while an invalid selector causes their admission gate to refuse.
inline const Profile& SelectedOrDefaultMetadata()
{const auto* value=Selected();return value?*value:*ById(Id::SkalaNew);}
inline const char* ModelPath(){return SelectedOrDefaultMetadata().modelLogicalPath.data();}
inline const char* ModelMember(){return SelectedOrDefaultMetadata().modelMember.data();}
inline const char* PrimaryPac(){return SelectedOrDefaultMetadata().primaryTexturePath.data();}

static_assert(ByModelPath(R"(DATA3D/skala_new.p3d)")==ById(Id::SkalaNew));
static_assert(ByModelPath(R"(data3d\skala2.p3d)")==ById(Id::Skala2));
static_assert(!ByModelPath(R"(data3d\skala2.p3d.bak)"));
static_assert(!ById(static_cast<Id>(0)));
} // namespace Poseidon::RetailRigidAssetProfile
