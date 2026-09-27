#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace Poseidon::GeometryPages
{
// Explicit offline original-byte subset, not a general MLOD/retail eligibility
// certificate. Unlike the normal loader, no unknown TAGG/reserved flags are lost.
// Exactly two visual P3DM LODs, untextured, canonical autocenter=0, no helpers.
enum class ControlledMlodStatus { Exported, Eligible, Unsupported, Invalid, Capacity, WrongOwner, AllocationFailed };
struct ControlledMlodRawFacts { std::array<float,2> resolutions{}; };
namespace ControlledMlodRawDetail
{
struct Refusal { ControlledMlodStatus status; };
struct Reader
{
    std::span<const uint8_t> data; size_t at=0;
    uint8_t Byte() { if(at==data.size()) throw Refusal{ControlledMlodStatus::Invalid}; return data[at++]; }
    uint32_t U32() { uint32_t v=0;for(unsigned i=0;i<4;++i)v|=uint32_t(Byte())<<(i*8);return v; }
    float Float() { const float v=std::bit_cast<float>(U32()); if(!std::isfinite(v))throw Refusal{ControlledMlodStatus::Invalid};return v; }
    void Literal(std::string_view s) { for(char c:s)if(Byte()!=uint8_t(c))throw Refusal{ControlledMlodStatus::Unsupported}; }
    std::string_view Name() {
        const size_t start=at;
        for(size_t n=0;n<32;++n) if(Byte()==0)
            return {reinterpret_cast<const char*>(data.data()+start),at-start-1};
        throw Refusal{ControlledMlodStatus::Unsupported};
    }
    void Fixed(std::string_view s) {
        for(size_t i=0;i<64;++i) if(Byte()!=(i<s.size()?uint8_t(s[i]):0))
            throw Refusal{ControlledMlodStatus::Unsupported};
    }
    void Zero(uint32_t v) { if(v) throw Refusal{ControlledMlodStatus::Unsupported}; }
};
}
inline ControlledMlodStatus PreflightControlledMlod(std::span<const uint8_t> bytes,ControlledMlodRawFacts& destination)
{
    using namespace ControlledMlodRawDetail;
    if(bytes.size()>16*1024*1024) return ControlledMlodStatus::Capacity;
    try {
        Reader r{bytes}; ControlledMlodRawFacts result;
        r.Literal("MLOD"); if(r.U32()!=0x00000101) return ControlledMlodStatus::Unsupported;
        if(r.U32()!=2) return ControlledMlodStatus::Unsupported;
        for(size_t lod=0;lod<2;++lod) {
            r.Literal("P3DM"); if(r.U32()!=28||r.U32()!=256)return ControlledMlodStatus::Unsupported;
            const uint32_t positions=r.U32(),normals=r.U32(),faces=r.U32();r.Zero(r.U32());
            if(!positions||!normals||!faces)return ControlledMlodStatus::Invalid;
            if(positions>4096||normals>4096||faces>8192)return ControlledMlodStatus::Capacity;
            for(uint32_t i=0;i<positions;++i) {
                for(int axis=0;axis<3;++axis)if(std::abs(r.Float())>10000)return ControlledMlodStatus::Unsupported;
                r.Zero(r.U32());
            }
            for(uint32_t i=0;i<normals;++i) {
                float length2=0;for(int axis=0;axis<3;++axis) {const float f=r.Float();length2+=f*f;}
                if(!std::isfinite(length2)||length2<1e-12f||length2>1e8f)return ControlledMlodStatus::Unsupported;
            }
            uint32_t corners=0;
            for(uint32_t f=0;f<faces;++f) {
                const uint32_t n=r.U32();if(n!=3&&n!=4)return ControlledMlodStatus::Unsupported;
                corners+=n;if(corners>4096)return ControlledMlodStatus::Capacity; // bounds seam-expanded loader vertices
                std::array<uint32_t,4> ids{};
                for(uint32_t v=0;v<4;++v) {
                    const uint32_t p=r.U32(),normal=r.U32();
                    if(v<n&&(p>=positions||normal>=normals))return ControlledMlodStatus::Invalid;
                    if(v>=n&&(p||normal))return ControlledMlodStatus::Unsupported;
                    ids[v]=p;
                    for(int uv=0;uv<2;++uv)if(std::abs(r.Float())>10000)return ControlledMlodStatus::Unsupported;
                }
                for(uint32_t a=0;a<n;++a)for(uint32_t b=a+1;b<n;++b)
                    if(ids[a]==ids[b])return ControlledMlodStatus::Unsupported;
                r.Zero(r.U32()); if(r.Byte()!=0||r.Byte()!=0)return ControlledMlodStatus::Unsupported;
            }
            r.Literal("TAGG");
            if(r.Byte()!=1||r.Name()!="#Property#"||r.U32()!=128)return ControlledMlodStatus::Unsupported;
            r.Fixed("autocenter");r.Fixed("0");
            if(r.Byte()!=1||r.Name()!="#EndOfFile#"||r.U32()!=0)return ControlledMlodStatus::Unsupported;
            result.resolutions[lod]=r.Float();
            if(result.resolutions[lod]<0||result.resolutions[lod]>=900)return ControlledMlodStatus::Unsupported;
        }
        if(r.at!=bytes.size()||result.resolutions[0]==result.resolutions[1])return ControlledMlodStatus::Invalid;
        destination=result;return ControlledMlodStatus::Eligible;
    } catch(const Refusal& e) { return e.status; }
}
} // namespace Poseidon::GeometryPages
