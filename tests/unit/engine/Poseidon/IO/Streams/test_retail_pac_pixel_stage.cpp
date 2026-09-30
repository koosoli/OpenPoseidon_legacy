#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Textures/RetailPacPixelStage.hpp>
#include <Poseidon/IO/Streams/FileAccessPolicy.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/IO/Streams/RetailRigidAssetProfile.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace Poseidon;

namespace
{
std::vector<char> DxtPac()
{
    std::vector<char> bytes;
    auto word = [&](unsigned x) { bytes.push_back(char(x)); bytes.push_back(char(x >> 8)); };
    word(0xff01); word(0); // DXT1 marker and empty palette
    for (unsigned side : {16u, 8u, 4u})
    {
        const unsigned size = ((side + 3) / 4) * ((side + 3) / 4) * 8;
        word(side); word(side);
        bytes.push_back(char(size)); bytes.push_back(char(size >> 8)); bytes.push_back(char(size >> 16));
        for (unsigned i = 0; i < size; ++i) bytes.push_back(char(13 + i * 17));
    }
    word(0); word(0);
    return bytes;
}

struct RawBank
{
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("retail-pixel-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    QFBank bank;
    explicit RawBank(const RetailRigidAssetProfile::Profile& profile)
    {
        Foundation::CaptureMainThread();
        REQUIRE(std::filesystem::create_directory(root));
        REQUIRE(std::filesystem::create_directory(root / "DTA"));
        const auto bytes = DxtPac();
        std::ofstream out(root / "DTA" / "data.pbo", std::ios::binary);
        REQUIRE(out.good());
        const std::string name(profile.textureMember);
        out.write(name.c_str(), static_cast<std::streamsize>(name.size() + 1));
        for (int value : {0, 0, 0, 0, int(bytes.size())})
            out.write(reinterpret_cast<const char*>(&value), 4);
        const char zero = 0;
        out.write(&zero, 1);
        for (int value : {0, 0, 0, 0, 0}) out.write(reinterpret_cast<const char*>(&value), 4);
        out.write(bytes.data(), bytes.size());
        out.close();
        REQUIRE(bank.open(RString((root / "DTA" / "data").string().c_str())));
        bank.SetPrefix(R"(data\)");
        bank.Lock();
    }
    ~RawBank()
    {
        if (bank.IsLocked()) bank.Unlock();
        bank.close();
        std::error_code error;
        std::filesystem::remove(root / "DTA" / "data.pbo", error); error.clear();
        std::filesystem::remove(root / "DTA", error); error.clear();
        std::filesystem::remove(root, error);
    }
};
}

TEST_CASE("Exact retail PAC preparation uses leased BC1 bytes and rejects changed headers",
    "[retail-pac-pixels][archive-source-actual]")
{
#ifndef _WIN32
    SKIP("Physical PBO archive identity is Windows-only.");
#else
    if (!ArchiveSourceBinding::RetailPacReadScope::Enabled())
        SKIP("Requires fresh WGR_GEOMETRY_PAGE_RETAIL_SOURCE=1 process.");
    if (!QFileAccess::MappingSupported()) SKIP("Original mapped bank read unavailable.");
    const auto* profile = RetailRigidAssetProfile::Selected();
    REQUIRE(profile);
    RawBank files(*profile);
    render::ColdPaaRead header;
    {
        ArchiveSourceBinding::RetailPacReadScope purpose(profile->modelLogicalPath.data());
        const std::string member(profile->textureMember);
        auto raw = files.bank.Read(member.c_str()); REQUIRE(raw);
        header.source = raw->GetArchiveSourceBinding(); REQUIRE(header.source);
    }
    header.key = std::string(profile->primaryTexturePath);
    header.magic = 0xff01;
    header.count = 3;
    header.levels[0] = {16, 16, 128, 4};
    header.levels[1] = {8, 8, 32, 139};
    header.levels[2] = {4, 4, 8, 178};
    REQUIRE(header.Valid());
    auto prepared = render::RetailPacPrepared::Prepare(header); REQUIRE(prepared);
    REQUIRE(prepared->Valid());
    CHECK(prepared->BlockBytes() == 168);
    CHECK(prepared->RawSha256().size() == 64);
    std::vector<char> reread;
    REQUIRE(header.source->Request().Read(reread));
    CHECK(reread == DxtPac());
    CHECK(render::RetailPacPrepared::MatchesSelectedSource(header));
    auto staleHeader = header; ++staleHeader.levels[1].header;
    CHECK_FALSE(render::RetailPacPrepared::Prepare(staleHeader));
    const auto* otherProfile = RetailRigidAssetProfile::ById(
        profile->id == RetailRigidAssetProfile::Id::SkalaNew ?
        RetailRigidAssetProfile::Id::Skala2 : RetailRigidAssetProfile::Id::SkalaNew);
    REQUIRE(otherProfile);
    auto wrongKey = header; wrongKey.key = std::string(otherProfile->primaryTexturePath);
    CHECK_FALSE(render::RetailPacPrepared::MatchesSelectedSource(wrongKey));
    CHECK_FALSE(render::RetailPacPrepared::Prepare(wrongKey));
    auto wrongFormat = header; wrongFormat.magic = 0xff05;
    CHECK_FALSE(render::RetailPacPrepared::Prepare(wrongFormat));
    CHECK_FALSE(render::RetailPacPrepared::Prepare(header, [] { return true; }));
#endif
}
