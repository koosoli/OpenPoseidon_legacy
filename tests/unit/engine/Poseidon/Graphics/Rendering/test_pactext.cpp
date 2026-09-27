#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Graphics/Rendering/Font/Pactext.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>

#include <cstring>
#include <array>
#include <cstdint>
#include <Poseidon/Foundation/Algorithms/Lzo1x.hpp>
#include <vector>

TEST_CASE("pactext.hpp compiles", "[rendering][font]")
{
    SUCCEED("header included successfully");
}

// SelectTextureSourceFactory is the texture-load entry the briefing equipment
// screen hits per gear slot.  An empty optional slot resolves to a directory-
// only path ("dtaext\equip\") with no filename — the original engine skipped it
// silently.  Without the guard the selector logs an "Unrecognized texture type"
// ERROR for every empty slot, which strict mode turns fatal (exit 3) on the
// briefing / credits screens.
//
// Broken-state delta: without the empty-filename guard, the "dtaext\equip\"
// case bumps GetErrorCount() to 1 (the LOG_ERROR fires); with it, count stays 0.
TEST_CASE("SelectTextureSourceFactory: empty-filename path is no-texture, not an error", "[rendering][font]")
{
    using LS = Poseidon::Foundation::LoggingSystem;
    LS logSys;
    logSys.Initialize("trace"); // attaches the ErrorCountingSink to category loggers
    LS::SetStrictMode(false);   // count errors without latching the strict trip

    SECTION("directory-only path (empty equipment slot) is silent")
    {
        LS::ResetErrorCount();
        auto* f = Poseidon::SelectTextureSourceFactory("dtaext\\equip\\");
        CHECK(f == nullptr);
        CHECK(LS::GetErrorCount() == 0); // the fix: no ERROR for a fileless path
    }

    SECTION("a real filename with an unknown extension still errors")
    {
        LS::ResetErrorCount();
        auto* f = Poseidon::SelectTextureSourceFactory("equip\\w\\w_bad.xyz");
        CHECK(f == nullptr);
        CHECK(LS::GetErrorCount() >= 1); // genuinely unrecognized → still reported
    }

    SECTION("a .paa name resolves to the PAC factory")
    {
        LS::ResetErrorCount();
        auto* f = Poseidon::SelectTextureSourceFactory("equip\\w\\w_m16.paa");
        CHECK(f != nullptr);
        CHECK(LS::GetErrorCount() == 0);
    }

    SECTION("empty and null names are silent")
    {
        LS::ResetErrorCount();
        CHECK(Poseidon::SelectTextureSourceFactory("") == nullptr);
        CHECK(Poseidon::SelectTextureSourceFactory(nullptr) == nullptr);
        CHECK(LS::GetErrorCount() == 0);
    }

    LS::ResetErrorCount();
}

// A PAA 16b mipmap is an LZSS stream followed by a 32b checksum over the decoded bytes,
// each summed as a SIGNED char -- the semantics the CWR d7a89671 backport made explicit
// in DecodeLZW. The checksum below is only correct under signed summation (bytes >= 0x80
// contribute negatively); on an unsigned-plain-char platform the old code computed +911
// and rejected the stream.
TEST_CASE("PacLevelMem::LoadPaa accepts the signed LZW checksum", "[rendering][font]")
{
    const unsigned char pixels[8] = {0x80, 0xff, 0x7f, 0x01, 0x90, 0x00, 0xc0, 0x40};
    const int checksum = -113;

    std::vector<unsigned char> file;
    auto putWord = [&file](int v)
    {
        file.push_back(v & 0xff);
        file.push_back((v >> 8) & 0xff);
    };

    // mipmap header: 16b width, 16b height, 24b compressed size
    putWord(2);
    putWord(2);
    file.push_back(static_cast<unsigned char>(sizeof(pixels)));
    file.push_back(0);
    file.push_back(0);

    file.push_back(0xff); // LZSS flag byte: the next eight tokens are literals
    for (unsigned char b : pixels)
    {
        file.push_back(b);
    }
    for (int i = 0; i < 4; i++)
    {
        file.push_back((checksum >> (i * 8)) & 0xff);
    }

    Poseidon::PacLevelMem mip;
    mip._w = 2;
    mip._h = 2;
    mip._sFormat = Poseidon::PacARGB4444;
    mip.SetDestFormat(Poseidon::PacARGB4444, 1);

    Poseidon::PacPalette palette;
    Poseidon::QIStream in(file.data(), static_cast<int>(file.size()));

    unsigned char decoded[sizeof(pixels)] = {};
    REQUIRE(mip.LoadPaa(in, decoded, &palette) == 0);
    REQUIRE(std::memcmp(decoded, pixels, sizeof(pixels)) == 0);
}

namespace
{
std::vector<char> OriginalLzoMip(int w, int h, int blockBytes)
{
    const int outputBytes = ((w + 3) / 4) * ((h + 3) / 4) * blockBytes;
    std::vector<char> packed{char(17 + outputBytes)};
    for (int i = 0; i < outputBytes; ++i) packed.push_back(char((i * 37 + w * 11 + h) & 255));
    packed.insert(packed.end(), {char(0x11), 0, 0});
    std::vector<char> mip;
    const auto u16 = [&](int v) { mip.push_back(char(v)); mip.push_back(char(v >> 8)); };
    u16(w | 0x8000); u16(h);
    const int stored = int(packed.size());
    mip.push_back(char(stored)); mip.push_back(char(stored >> 8)); mip.push_back(char(stored >> 16));
    mip.insert(mip.end(), packed.begin(), packed.end());
    return mip;
}
// Independent copied-input reference of the original ordinary LZO mip branch.
int CopiedLzoMipReference(Poseidon::QIStream& in, void* output, int blockBytes)
{
    const int w = Poseidon::fgetiw(in) & 0x7fff;
    const int h = Poseidon::fgetiw(in);
    const int stored = Poseidon::fgeti24(in);
    const size_t expected = size_t((w + 3) / 4) * ((h + 3) / 4) * blockBytes;
    std::vector<char> packed(size_t(stored), 0);
    in.read(packed.data(), stored);
    if (in.fail()) return -1;
    const size_t got = Poseidon::Foundation::Lzo1x::Decompress(
        reinterpret_cast<const uint8_t*>(packed.data()), packed.size(), static_cast<uint8_t*>(output), expected);
    return got == expected ? 0 : -1;
}
Poseidon::PacLevelMem LzoLevel(Poseidon::PacFormat format, int w, int h)
{
    Poseidon::PacLevelMem level;
    level._sFormat = format; level._dFormat = format;
    level._w = short(w); level._h = short(h);
    return level;
}
}

TEST_CASE("PAA LZO mip disjoint input matches original copying reader", "[rendering][font][lzo-disjoint]")
{
    for (const auto format : {Poseidon::PacDXT1, Poseidon::PacDXT3, Poseidon::PacDXT5})
    for (const auto dims : std::array<std::array<int,2>,4>{{{4,4}, {8,4}, {4,8}, {8,8}}})
    {
        const int blockBytes = format == Poseidon::PacDXT1 ? 8 : 16;
        const size_t bytes = size_t(dims[0] / 4) * (dims[1] / 4) * blockBytes;
        const auto original = OriginalLzoMip(dims[0], dims[1], blockBytes);
        const auto level = LzoLevel(format, dims[0], dims[1]);
        Poseidon::PacPalette palette;
        for (int variant = 0; variant < 3; ++variant)
        {
            auto input = original;
            if (variant == 1) input.resize(input.size() - 2); // declared payload truncated
            if (variant == 2) input.back() = 1; // malformed end-of-stream
            Poseidon::QIStream actual(input.data(), int(input.size())), reference(input.data(), int(input.size()));
            std::vector<uint8_t> a(bytes + 8, 0xA7), b = a;
            CHECK(level.LoadPaa(actual, a.data(), &palette) == CopiedLzoMipReference(reference, b.data(), blockBytes));
            CHECK(a == b); CHECK(actual.tellg() == reference.tellg());
            CHECK(actual.fail() == reference.fail()); CHECK(actual.eof() == reference.eof());
        }
    }
}

TEST_CASE("PAA LZO mip overlapping output retains copied-input semantics", "[rendering][font][lzo-disjoint]")
{
    const auto mip = OriginalLzoMip(4, 4, 16);
    const auto level = LzoLevel(Poseidon::PacDXT3, 4, 4);
    Poseidon::PacPalette palette;
    for (const size_t outputOffset : {size_t(0), size_t(7), mip.size()})
    {
        auto a = mip; a.resize(a.size() + 64, char(0x5A));
        auto b = a;
        Poseidon::QIStream actual(a.data(), int(a.size())), reference(b.data(), int(b.size()));
        REQUIRE(level.LoadPaa(actual, a.data() + outputOffset, &palette) == 0);
        REQUIRE(CopiedLzoMipReference(reference, b.data() + outputOffset, 16) == 0);
        CHECK(a == b); CHECK(actual.tellg() == reference.tellg());
        CHECK(actual.fail() == reference.fail()); CHECK(actual.eof() == reference.eof());
    }
}
