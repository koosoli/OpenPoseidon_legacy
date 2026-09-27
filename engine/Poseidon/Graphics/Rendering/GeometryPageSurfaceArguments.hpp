#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace Poseidon::GeometryPages::SurfaceAdmission
{
struct Arguments {
    std::filesystem::path manifestPath,certificatePath;
    std::array<uint8_t,32> manifestSha256{},certificateSha256{};
};
namespace ArgumentDetail {
inline bool Hex(std::string_view s,std::span<uint8_t> output) {
    if(s.size()!=output.size()*2)return false;
    auto digit=[](char c)->int {return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;};
    for(size_t i=0;i<output.size();++i){const int a=digit(s[i*2]),b=digit(s[i*2+1]);if(a<0||b<0)return false;output[i]=uint8_t(a*16+b);}return true;
}
}
inline bool ParseArguments(const std::vector<std::string>& manifest,const std::vector<std::string>& certificate,Arguments& out) {
    if(manifest.size()!=2||certificate.size()!=2)return false;Arguments a;
    for(const auto* pair:{&manifest,&certificate}){if((*pair)[0].empty()||(*pair)[0].size()>1023)return false;
        for(unsigned char c:(*pair)[0])if(c<32||c>126)return false;}
    a.manifestPath=manifest[0];a.certificatePath=certificate[0];
    if(!ArgumentDetail::Hex(manifest[1],a.manifestSha256)||!ArgumentDetail::Hex(certificate[1],a.certificateSha256))return false;
    out=std::move(a);return true;
}
}
