#include <catch2/catch_test_macros.hpp>
#include <Poseidon/IO/Streams/ArchiveSourceBinding.hpp>
#include <Poseidon/IO/Streams/FileAccessPolicy.hpp>
#include <Poseidon/IO/FileServerMT.hpp>
#include <Poseidon/Graphics/Rendering/Font/Pactext.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <vector>
#include <type_traits>
#include "../../test_fixtures.hpp"
#include <iterator>

using namespace Poseidon;
static_assert(!std::is_copy_constructible_v<ArchiveSourceBinding::RetailPacReadScope>);
static_assert(!std::is_move_constructible_v<ArchiveSourceBinding::RetailPacReadScope>);

namespace
{
constexpr const char* SelectedModelPath = R"(data3d\skala_new.p3d)";
constexpr const char* SelectedTexturePath = R"(data\skala_piskovec2.pac)";
constexpr const char* Member = "skala_piskovec2.pac";

// An actual legacy PAC (P8 palette + one 8x8 mip), not a PAA magic word
// hidden behind the .pac extension.
std::vector<char> PacBytes()
{
    std::vector<char> out;
    auto word = [&](unsigned value) { out.push_back(char(value)); out.push_back(char(value >> 8)); };
    word(1);                         // one RGB palette entry
    out.insert(out.end(), {char(0x20), char(0x40), char(0x80)});
    word(8); word(8);                // first mip dimensions
    out.insert(out.end(), {char(64), char(0), char(0)}); // 24-bit payload size
    out.insert(out.end(), 64, char(0));
    word(0); word(0);                // mip terminator
    return out;
}

struct RawPacBank
{
    std::filesystem::path directory = std::filesystem::temp_directory_path() /
        ("retail-pac-binding-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path bankBase;
    QFBank bank;
    explicit RawPacBank(const char* bankDirectory = "DTA", const char* prefix = R"(data\)",
                        const std::vector<char>& data = PacBytes())
    {
        Foundation::CaptureMainThread();
        REQUIRE(std::filesystem::create_directory(directory));
        REQUIRE(std::filesystem::create_directory(directory / bankDirectory));
        bankBase = directory / bankDirectory / "data";
        std::ofstream out(bankBase.string() + ".pbo", std::ios::binary);
        REQUIRE(out.good());
        auto entry = [&](const char* name) {
            out.write(name, std::strlen(name) + 1);
            for (int value : {0, 0, 0, 0, int(data.size())})
                out.write(reinterpret_cast<const char*>(&value), 4);
        };
        entry(Member); entry("other.pac");
        const char end = 0;
        out.write(&end, 1);
        for (int value : {0, 0, 0, 0, 0}) out.write(reinterpret_cast<const char*>(&value), 4);
        out.write(data.data(), data.size());
        out.write(data.data(), data.size());
        out.close();
        REQUIRE(bank.open(RString(bankBase.string().c_str())));
        bank.SetPrefix(prefix);
        bank.Lock();
    }
    ~RawPacBank()
    {
        if (bank.IsLocked()) bank.Unlock();
        bank.close();
        std::error_code error;
        std::filesystem::remove(bankBase.string() + ".pbo", error); error.clear();
        std::filesystem::remove(bankBase.parent_path(), error); error.clear();
        std::filesystem::remove(directory, error);
    }
};

class DirectPacServer final : public FileServer
{
public:
    QFBank& bank;
    const char* member;
    DirectPacServer(QFBank& b, const char* m) : bank(b), member(m) {}
    void Request(const char*, float, int, int) override {}
    void CancelRequest(const char*, int, int) override {}
    void Open(QIFStream& stream, const char* name) override
    {
        if (std::strcmp(name, SelectedTexturePath) == 0) stream.open(bank.Read(member));
    }
    void Start() override {} void Stop() override {}
    void FlushBank(QFBank*) override {}
};

struct ServerScope
{
    Ref<FileServer> previous = GFileServer;
    explicit ServerScope(QFBank& bank, const char* member = Member)
    { GFileServer = new DirectPacServer(bank, member); }
    ~ServerScope() { GFileServer = previous; }
};

std::unique_ptr<ITextureSource> ParsePac()
{
    std::array<PacLevelMem, 7> mips;
    auto* factory = SelectTextureSourceFactory(SelectedTexturePath);
    REQUIRE(factory);
    return std::unique_ptr<ITextureSource>(factory->Create(SelectedTexturePath, mips.data(), int(mips.size())));
}

TEST_CASE("Marker-bearing PAC uses PAA level decoding without ordinary PAA birth admission",
          "[archive-source-retail-pac][upstream]")
{
    std::ifstream in(GET_FIXTURE("paa/synthetic_ai88_lzss.pac"), std::ios::binary);
    REQUIRE(in.good());
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
    RawPacBank files("DTA", R"(data\)", bytes);
    ServerScope server(files.bank);
    std::array<PacLevelMem, 7> mips;
    auto* factory = SelectTextureSourceFactory(SelectedTexturePath);
    REQUIRE(factory);
    std::unique_ptr<ITextureSource> source(factory->Create(SelectedTexturePath, mips.data(), int(mips.size())));
    REQUIRE(source);
    REQUIRE(source->GetFormat() == PacAI88);
    REQUIRE(source->GetMipmapCount() > 0);
    BankReadMemberIdentity identity;
    CHECK_FALSE(source->CopyArchiveMemberIdentity(identity));
    CHECK_FALSE(source->GetArchiveSourceBinding());
    mips[0].SetDestFormat(PacAI88, 4);
    std::vector<unsigned char> output(mips[0].Size(), 0);
    REQUIRE(source->GetMipmapData(output.data(), mips[0], 0));
    for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 8; ++x)
    {
        const auto* pixel = output.data() + y * mips[0].Pitch() + x * 2;
        CHECK(int(pixel[0]) == x * 32 + 16);
        CHECK(int(pixel[1]) == y * 32 + 16);
    }
}
}

TEST_CASE("Retail PAC source purpose is exact and owner-only", "[archive-source-retail-pac]")
{
    Foundation::CaptureMainThread();
    CHECK_FALSE(ArchiveSourceBinding::RetailPacReadScope::Active());
    {
        ArchiveSourceBinding::RetailPacReadScope wrong(R"(data3d\other.p3d)");
        CHECK_FALSE(ArchiveSourceBinding::RetailPacReadScope::Active());
    }
    ArchiveSourceBinding::RetailPacReadScope selected(SelectedModelPath);
    if (!ArchiveSourceBinding::RetailPacReadScope::Enabled())
    {
        CHECK_FALSE(ArchiveSourceBinding::RetailPacReadScope::Active());
        return;
    }
    REQUIRE(ArchiveSourceBinding::RetailPacReadScope::Active());
    CHECK(ArchiveSourceBinding::RetailPacReadScope::MatchesLogicalName(SelectedTexturePath));
    CHECK_FALSE(ArchiveSourceBinding::RetailPacReadScope::MatchesLogicalName(R"(data\skala_piskovec.pac)"));
    CHECK_FALSE(ArchiveSourceBinding::RetailPacReadScope::MatchesLogicalName(R"(data\piskovec.pac)"));
    CHECK(ArchiveSourceBinding::RetailPacReadScope::MatchesBankMember(
        R"(C:\game\DTA\data.pbo)", R"(data\)", Member));
    CHECK_FALSE(ArchiveSourceBinding::RetailPacReadScope::MatchesBankMember(
        R"(C:\game\Other\data.pbo)", R"(data\)", Member));
    CHECK_FALSE(ArchiveSourceBinding::RetailPacReadScope::MatchesBankMember(
        R"(C:\game\DTA\data.pbo)", R"(other\)", Member));
    CHECK_FALSE(ArchiveSourceBinding::RetailPacReadScope::MatchesBankMember(
        R"(C:\game\DTA\data.pbo)", R"(data\)", "other.pac"));
    CHECK_FALSE(ArchiveSourceBinding::RetailPacReadScope::MatchesBankMember(
        R"(C:\game\DTA\data.pbo)", R"(data\)", "piskovec.pac"));
    {
        ArchiveSourceBinding::RetailPacReadScope otherProfile(R"(data3d\skala2.p3d)");
        CHECK_FALSE(ArchiveSourceBinding::RetailPacReadScope::MatchesLogicalName(R"(data\piskovec.pac)"));
    }
    auto offOwner = std::async(std::launch::async, [] {
        ArchiveSourceBinding::RetailPacReadScope other(SelectedModelPath);
        return ArchiveSourceBinding::RetailPacReadScope::Active();
    });
    CHECK_FALSE(offOwner.get());
}

TEST_CASE("Retail PAC binding is born in the original mapped read and retained by Init",
    "[archive-source-retail-pac][archive-source-actual]")
{
#ifndef _WIN32
    SKIP("Physical PBO archive identity is Windows-only.");
#else
    if (!ArchiveSourceBinding::RetailPacReadScope::Enabled())
        SKIP("Requires fresh WGR_GEOMETRY_PAGE_RETAIL_SOURCE=1 process.");
    if (!QFileAccess::MappingSupported()) SKIP("Original mapped bank read unavailable.");
    RawPacBank files;
    ServerScope server(files.bank);
    {
        auto unscoped = ParsePac(); REQUIRE(unscoped);
        CHECK_FALSE(unscoped->GetArchiveSourceBinding());
    }
    std::unique_ptr<ITextureSource> source;
    {
        ArchiveSourceBinding::RetailPacReadScope selected(SelectedModelPath);
        REQUIRE(ArchiveSourceBinding::RetailPacReadScope::Active());
        source = ParsePac(); REQUIRE(source);
        REQUIRE(source->GetMipmapCount() > 0);
        REQUIRE(source->GetArchiveSourceBinding());
        auto request = files.bank.CaptureReadRequest(Member, true); REQUIRE(request);
        CHECK(source->GetArchiveSourceBinding()->Request().SameArchiveMember(*request));
        std::vector<char> reread;
        REQUIRE(source->GetArchiveSourceBinding()->Request().Read(reread));
        CHECK(reread == PacBytes());
        {
            ServerScope otherMember(files.bank, "other.pac");
            auto refused = ParsePac(); REQUIRE(refused);
            CHECK_FALSE(refused->GetArchiveSourceBinding());
        }
    }
    REQUIRE_FALSE(ArchiveSourceBinding::RetailPacReadScope::Active());
    REQUIRE(source->GetArchiveSourceBinding()); // source owns exact birth evidence past purpose exit
    {
        ArchiveSourceBinding::RetailPacReadScope wrongModel(R"(data3d\other.p3d)");
        auto refused = ParsePac(); REQUIRE(refused);
        CHECK_FALSE(refused->GetArchiveSourceBinding());
    }
    RawPacBank wrongBank("Other");
    ServerScope otherServer(wrongBank.bank);
    {
        ArchiveSourceBinding::RetailPacReadScope selected(SelectedModelPath);
        auto refused = ParsePac(); REQUIRE(refused);
        CHECK_FALSE(refused->GetArchiveSourceBinding());
    }
#endif
}
