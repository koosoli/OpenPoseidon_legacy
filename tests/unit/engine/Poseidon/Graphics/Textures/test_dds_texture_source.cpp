// test_dds_texture_source.cpp - DZ-002: TextureSourceDDS, the ITextureSource over .edds.
//
// The container itself is tested in test_edds_reader.cpp. What is tested here is the part
// that hands decoded levels to the renderer, because that is where a texture that parsed
// perfectly can still arrive wrong: the wrong destination format, a row pitch ignored, or
// a block format quietly converted.
//
// The 32-bit path matters more than its share of the corpus suggests. DayZ's water
// environment map -- the texture its whole appearance depends on (DZ-003) -- is 32-bit
// BGRA, and until reflection is bound nothing in the game exercises it.
//
// Inputs are built byte by byte from the format: DayZ's data is proprietary and no fixture
// may be committed.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <Poseidon/Graphics/Textures/DdsImport.hpp>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/Graphics/Textures/PreparedTextures.hpp>
#include <Poseidon/Graphics/Textures/EnfusionTextureName.hpp>
#include <Poseidon/Graphics/Textures/NativeNormalChannels.hpp>
#include <Poseidon/Asset/Formats/Enfusion/PakArchive.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <chrono>
#include <thread>
#include <string>
#include <vector>

using Poseidon::PacARGB1555;
using Poseidon::PacARGB8888;
using Poseidon::PacDXT1;
using Poseidon::PacLevelMem;
using Poseidon::PacRGB565;
using Poseidon::TextureSourceDDS;

namespace
{

void PutU32(std::vector<uint8_t>& buffer, size_t offset, uint32_t value)
{
    std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

void AppendU32(std::vector<uint8_t>& buffer, uint32_t value)
{
    const size_t at = buffer.size();
    buffer.resize(at + 4);
    PutU32(buffer, at, value);
}

// 128-byte DDS header carrying Enfusion's marker. `fourCC` empty means a 32-bit BGRA
// layout; otherwise a block format.
std::vector<uint8_t> Header(int width, int height, int mips, const char* fourCC = nullptr)
{
    std::vector<uint8_t> h(128, 0);
    std::memcpy(h.data(), "DDS ", 4);
    PutU32(h, 0x04, 124);
    PutU32(h, 0x08, 0x00020000); // DDSD_MIPMAPCOUNT
    PutU32(h, 0x0C, static_cast<uint32_t>(height));
    PutU32(h, 0x10, static_cast<uint32_t>(width));
    PutU32(h, 0x1C, static_cast<uint32_t>(mips));
    std::memcpy(h.data() + 0x24, "ENF1", 4);
    PutU32(h, 0x4C, 32);
    if (fourCC)
    {
        PutU32(h, 0x50, 0x04); // DDPF_FOURCC
        std::memcpy(h.data() + 0x54, fourCC, 4);
    }
    else
    {
        PutU32(h, 0x50, 0x41); // DDPF_RGB | DDPF_ALPHAPIXELS
        PutU32(h, 0x58, 32);
        PutU32(h, 0x5C, 0x00FF0000);
        PutU32(h, 0x60, 0x0000FF00);
        PutU32(h, 0x64, 0x000000FF);
        PutU32(h, 0x68, 0xFF000000);
    }
    return h;
}

// A single-level 2x2 BGRA image whose four texels are distinguishable.
std::vector<uint8_t> Bgra2x2()
{
    std::vector<uint8_t> file = Header(2, 2, 1);
    AppendU32(file, 0x59504F43); // 'COPY'
    AppendU32(file, 16);
    const uint8_t texels[16] = {
        0x10, 0x20, 0x30, 0xFF, // B,G,R,A
        0x11, 0x21, 0x31, 0xFE, //
        0x12, 0x22, 0x32, 0xFD, //
        0x13, 0x23, 0x33, 0xFC,
    };
    file.insert(file.end(), texels, texels + 16);
    return file;
}

struct ScopedLayerTintMode
{
    int previous;
    explicit ScopedLayerTintMode(bool multiply)
        : previous(Poseidon::Enfusion::GLayerTintLinearMultiply.exchange(multiply ? 1 : 0)) {}
    ~ScopedLayerTintMode() { Poseidon::Enfusion::GLayerTintLinearMultiply.store(previous); }
};

std::vector<uint8_t> BcrBc7Chain()
{
    auto file = Header(32, 32, 5, "DX10");
    for (uint32_t word : {98u, 3u, 0u, 1u, 0u}) AppendU32(file, word);
    // EDDS has one complete chunk table, followed by smallest-first payloads.
    std::vector<uint8_t> payload;
    for (int edge = 2; edge <= 32; edge *= 2)
    {
        const int blocks = ((edge + 3) / 4) * ((edge + 3) / 4);
        AppendU32(file, 0x59504F43);
        AppendU32(file, 16 * blocks);
        for (int b = 0; b < blocks; ++b)
        {
            uint8_t block[16]{};
            unsigned bit = 0;
            auto write = [&](unsigned value, unsigned bits) {
                for (unsigned i = 0; i < bits; ++i, ++bit)
                    block[bit / 8] |= ((value >> i) & 1u) << (bit % 8);
            };
            write(1u << 6, 7);
            for (unsigned value : {20u + unsigned(b % 20), 40u, 60u, 80u})
            { write(value, 7); write(value + 30, 7); }
            write(0, 1); write(1, 1); write(b % 8, 3);
            for (int i = 1; i < 16; ++i) write((i + b) % 16, 4);
            payload.insert(payload.end(), block, block + 16);
        }
    }
    file.insert(file.end(), payload.begin(), payload.end());
    return file;
}

std::vector<uint8_t> BcrBc7Alpha200()
{
    // Synthetic BC7 mode 6, all texels RGBA=(128,128,40,200).
    uint8_t block[16]{};
    unsigned bit = 0;
    auto write = [&](unsigned value, unsigned bits) {
        for (unsigned i = 0; i < bits; ++i, ++bit)
            block[bit / 8] |= ((value >> i) & 1u) << (bit % 8);
    };
    write(1u << 6, 7);
    for (unsigned value : {64u, 64u, 20u, 100u}) { write(value, 7); write(value, 7); }
    write(0, 1); write(0, 1); write(0, 3);
    for (int i = 1; i < 16; ++i) write(0, 4);
    auto file = Header(4, 4, 1, "DX10");
    for (uint32_t word : {98u, 3u, 0u, 1u, 0u, 0x59504F43u, 16u}) AppendU32(file, word);
    file.insert(file.end(), block, block + 16);
    return file;
}
} // namespace

TEST_CASE("Borrowed BC7 BCR tint matches baked pixels and averages without a decoded chain", "[graphics][edds][lazy-tint]")
{
    const int maxMips = GENERATE(2, 7, 16);
    const auto bytes = BcrBc7Chain();
    int reads = 0;
    const auto reader = [&](const char*, std::vector<uint8_t>& out) { ++reads; out = bytes; return true; };
    TextureSourceDDS base;
    PacLevelMem baseMips[16];
    const Poseidon::DdsPreparationOptions options{true, true, 1024};
    REQUIRE(base.InitFromReader("fixture_bcr.edds", baseMips, maxMips, reader, options));
    REQUIRE(base.GetFormat() == Poseidon::PacBC7);
    const size_t baseBytes = base.PreparedByteSize();
    const float tint[3] = {GENERATE(0.0f, 0.25f, 1.0f), 0.7f, 0.15f};
    const auto name = Poseidon::Enfusion::MakeLayerTintName(tint, "fixture_bcr.edds");
    TextureSourceDDS baked;
    PacLevelMem expectedMips[16], viewMips[16];
    REQUIRE(baked.InitFromReader(name.c_str(), expectedMips, maxMips, reader, options));
    const int beforeView = reads;
    auto view = base.CreateLinearTintView(tint, viewMips, 16);
    REQUIRE(view);
    CHECK(reads == beforeView);
    CHECK(view->GetAverageColor() == baked.GetAverageColor());
    CHECK(view->GetFormat() == baked.GetFormat());
    CHECK(view->IsAlpha() == baked.IsAlpha());
    CHECK(view->IsTransparent() == baked.IsTransparent());
    CHECK(view->GetMipmapCount() == baked.GetMipmapCount());
    view->ForceAlpha();
    CHECK(view->IsAlpha());
    CHECK_FALSE(base.IsAlpha());
    for (int i = 0; i < view->GetMipmapCount(); ++i)
    {
        REQUIRE(viewMips[i]._w == expectedMips[i]._w);
        REQUIRE(viewMips[i]._h == expectedMips[i]._h);
        for (const auto format : {PacARGB8888, PacARGB1555, PacRGB565})
        {
            PacLevelMem mip = viewMips[i];
            mip.SetDestFormat(format, 8);
            std::vector<uint8_t> actual(mip._pitch * mip._h, 0x77), expected(actual);
            REQUIRE(view->GetMipmapData(actual.data(), mip, i));
            REQUIRE(baked.GetMipmapData(expected.data(), mip, i));
            CHECK(actual == expected);
        }
    }
    CHECK(base.PreparedByteSize() == baseBytes);
    CHECK(base.GetFormat() == Poseidon::PacBC7);
    CHECK_FALSE(view->GetMipmapData(nullptr, viewMips[0], 0));
    uint8_t unused[4]{};
    CHECK_FALSE(view->GetMipmapData(unused, viewMips[0], -1));
    CHECK_FALSE(view->GetMipmapData(unused, viewMips[0], view->GetMipmapCount()));
    auto capped = base.CreateLinearTintView(tint, viewMips, 1);
    REQUIRE(capped);
    CHECK(capped->GetMipmapCount() == 1);
    CHECK(capped->GetAverageColor() == baked.GetAverageColor());
}

TEST_CASE("Installed Reforger BCR tint views match the original CPU chain", "[.][lazy-tint-corpus]")
{
    namespace fs = std::filesystem;
    using Poseidon::Asset::Formats::Enfusion::PakArchive;
    const char* corpus = std::getenv("POSEIDON_TEST_REFORGER_ADDONS");
    REQUIRE(corpus);
    REQUIRE(fs::is_directory(corpus));
    std::vector<fs::path> paths;
    for (const auto& entry : fs::recursive_directory_iterator(corpus))
        if (entry.is_regular_file() && entry.path().extension() == ".pak") paths.push_back(entry.path());
    std::sort(paths.begin(), paths.end());
    REQUIRE_FALSE(paths.empty());
    std::vector<PakArchive> archives(paths.size());
    for (size_t i = 0; i < paths.size(); ++i) REQUIRE(archives[i].Open(paths[i].string()));
    const auto reader = [&](const char* name, std::vector<uint8_t>& bytes) {
        for (const auto& archive : archives)
            if (const auto* entry = archive.Find(name)) return archive.Read(*entry, bytes);
        return false;
    };
    // Actual synthetic names observed in the native Everon route; no asset copies.
    const char* samples[] = {
        "enft|0.1723,0.1038,0.0944|assets/_shareddata/metal/st_metalpaint_mate_1m_bcr.edds",
        "enft|0.4770,0.4570,0.4238|assets/_shareddata/metal/st_metalpaint_mate_1m_bcr.edds",
        "enft|0.2196,0.1938,0.1509|assets/_shareddata/plaster/st_plaster_bare_03_bcr.edds",
        "enft|0.2993,0.2694,0.2402|assets/_shareddata/wood/st_woodplanks_01_bcr.edds",
        "enft|0.3432,0.4112,0.4612|assets/structures/signs/directions/data/sign_direction_15_2_bcr.edds"
    };
    for (const int cap : {256, 1024})
        for (const char* name : samples)
        {
            CAPTURE(name, cap);
            float rgb[3];
            const char* inner = nullptr;
            REQUIRE(Poseidon::Enfusion::SplitLayerTintName(name, rgb, &inner));
            TextureSourceDDS base, baked;
            PacLevelMem baseMips[7], expectedMips[7], viewMips[7];
            const Poseidon::DdsPreparationOptions options{true, true, cap};
            REQUIRE(base.InitFromReader(inner, baseMips, 7, reader, options));
            REQUIRE(baked.InitFromReader(name, expectedMips, 7, reader, options));
            auto view = base.CreateLinearTintView(rgb, viewMips, 7);
            REQUIRE(view);
            CHECK(view->GetAverageColor() == baked.GetAverageColor());
            REQUIRE(view->GetMipmapCount() == baked.GetMipmapCount());
            CHECK(view->IsAlpha() == baked.IsAlpha());
            CHECK(view->IsTransparent() == baked.IsTransparent());
            for (int level = 0; level < view->GetMipmapCount(); ++level)
            {
                REQUIRE(viewMips[level]._w == expectedMips[level]._w);
                REQUIRE(viewMips[level]._h == expectedMips[level]._h);
                auto mip = viewMips[level];
                mip.SetDestFormat(PacARGB8888, 8);
                std::vector<uint8_t> actual(mip._pitch * mip._h, 0x77), expected(actual);
                REQUIRE(view->GetMipmapData(actual.data(), mip, level));
                REQUIRE(baked.GetMipmapData(expected.data(), mip, level));
                CHECK(actual == expected);
            }
        }
}

TEST_CASE("Lazy tint declines unsupported data and invalid inputs", "[graphics][edds][lazy-tint]")
{
    TextureSourceDDS base;
    PacLevelMem mips[16];
    const float tint[3] = {.2f, .3f, .4f};
    const auto bytes = BcrBc7Chain();
    const auto reader = [&](const char*, std::vector<uint8_t>& out) { out = bytes; return true; };
    const char* name = GENERATE("fixture_bcr.edds", "fixture_ntc.edds", "fixture.edds");
    REQUIRE(base.InitFromReader(name, mips, 16, reader, {true, true, 1024}));
    CHECK_FALSE(base.CreateLinearTintView(nullptr, mips, 16));
    CHECK_FALSE(base.CreateLinearTintView(tint, nullptr, 16));
    CHECK_FALSE(base.CreateLinearTintView(tint, mips, 0));
    if (std::strcmp(name, "fixture_bcr.edds") != 0)
        CHECK_FALSE(base.CreateLinearTintView(tint, mips, 16));
    const float invalid[3] = {std::numeric_limits<float>::quiet_NaN(), .2f, .3f};
    CHECK_FALSE(base.CreateLinearTintView(invalid, mips, 16));
    const float negative[3] = {-.2f, .2f, .3f};
    CHECK_FALSE(base.CreateLinearTintView(negative, mips, 16));
    TextureSourceDDS decoded;
    REQUIRE(decoded.InitFromReader("enft|0.2,0.3,0.4|fixture_bcr.edds", mips, 16, reader, {true, true, 1024}));
    CHECK_FALSE(decoded.CreateLinearTintView(tint, mips, 16));
}

TEST_CASE("Enfusion RG normals use red rather than cavity or occlusion for X", "[graphics][edds][ntc][nho]")
{
    REQUIRE(Poseidon::IsEnfusionRgNormalName("assets/polyplane_picea_NTC.edds"));
    REQUIRE(Poseidon::IsEnfusionRgNormalName("assets/bark_picea_ntc.edds"));
    REQUIRE(Poseidon::IsEnfusionRgNormalName("assets/wall_nmo.edds"));
    REQUIRE(Poseidon::IsEnfusionRgNormalName("assets/decals/beach_naturedebris_01_nho.edds"));
    REQUIRE(Poseidon::IsEnfusionRgNormalName("assets\\decals\\BEACH_NHO.EDDS"));
    REQUIRE(Poseidon::IsEnfusionRgNormalName("assets.v2/beach_nho"));
    REQUIRE_FALSE(Poseidon::IsEnfusionRgNormalName("assets/old_nho/beach_bcr.edds"));
    REQUIRE_FALSE(Poseidon::IsEnfusionRgNormalName("assets/beach_nho_bcr.edds"));
    REQUIRE_FALSE(Poseidon::IsEnfusionRgNormalName("assets/beach_nh.edds"));
    REQUIRE_FALSE(Poseidon::IsEnfusionRgNormalName(""));
    REQUIRE_FALSE(Poseidon::IsEnfusionRgNormalName("assets/wall_nohq.paa"));
    REQUIRE_FALSE(Poseidon::IsEnfusionRgNormalName("assets/leaf_bcr.edds"));
    REQUIRE_FALSE(Poseidon::IsEnfusionRgNormalName("assets/leaf_a.edds"));
    REQUIRE_FALSE(Poseidon::IsEnfusionRgNormalName("assets/old_ntc/leaf_bcr.edds"));
    REQUIRE_FALSE(Poseidon::IsEnfusionRgNormalName("assets/leaf_ntc_bcr.edds"));
    REQUIRE_FALSE(Poseidon::IsEnfusionRgNormalName(nullptr));

    // Synthetic BC7 mode 6, uniform RGBA=(128,128,40,200). No game assets.
    uint8_t block[16] = {};
    unsigned bit = 0;
    auto write = [&](unsigned value, unsigned bits) {
        for (unsigned i = 0; i < bits; ++i, ++bit)
            block[bit / 8] |= ((value >> i) & 1u) << (bit % 8);
    };
    write(1u << 6, 7);
    for (unsigned value : {64u, 64u, 20u, 100u}) { write(value, 7); write(value, 7); }
    write(0, 1); write(0, 1); write(0, 3);
    for (int i = 1; i < 16; ++i) write(0, 4);
    REQUIRE(bit == 128);
    auto file = Header(4, 4, 1, "DX10");
    for (uint32_t word : {98u, 3u, 0u, 1u, 0u, 0x59504F43u, 16u}) AppendU32(file, word);
    file.insert(file.end(), block, block + 16);
    for (const char* name : {"test_ntc.edds", "test_nmo.edds", "test_nho.edds", "test_bcr.edds", "test_coverage.edds"})
    {
        INFO(name);
        TextureSourceDDS source;
        REQUIRE(source.InitFromMemory(file.data(), file.size(), name, true));
        PacLevelMem mip;
        mip._w = mip._h = 4; mip._pitch = 16; mip._dFormat = PacARGB8888;
        uint8_t pixels[64] = {};
        REQUIRE(source.GetMipmapData(pixels, mip, 0));
        for (int i = 0; i < 64; i += 4)
        {
            REQUIRE(pixels[i + 2] == 128);
            REQUIRE(pixels[i + 1] == 128);
            // BCR roughness survives as data, while IsAlpha stays false so it
            // cannot become coverage. Native RG decoding likewise retains
            // independent height/transmission B and AO/cavity A.
            REQUIRE(pixels[i] == 40);
            REQUIRE(static_cast<unsigned>(pixels[i + 3]) == 200);
        }
        if (std::strcmp(name, "test_bcr.edds") == 0)
            REQUIRE_FALSE(source.IsAlpha());
    }
}

TEST_CASE("Native NTC cavity survives compressed and decoded DDS upload classification",
          "[graphics][edds][ntc-cavity]")
{
    const auto file = BcrBc7Alpha200(); // Synthetic RGBA=(128,128,40,200), no game asset.
    const char* name = "Assets/Trees/Leaf_NTC.edds";
    const auto reader = [&](const char*, std::vector<uint8_t>& out) { out = file; return true; };
    REQUIRE(Poseidon::IsEnfusionRgNormalName(name));
    for (bool compressed : {false, true})
    {
        Poseidon::DdsPreparationOptions options{compressed, true, 1024};
        TextureSourceDDS source;
        PacLevelMem mips[16]{};
        REQUIRE(source.InitFromReader(name, mips, 16, reader, options));
        const auto format = source.GetFormat();
        REQUIRE(format == (compressed ? Poseidon::PacBC7 : PacARGB8888));
        REQUIRE(Poseidon::NativeNtcCavityChannelAvailable(format, !compressed, true, name));
        if (!compressed)
        {
            mips[0].SetDestFormat(PacARGB8888, 8);
            uint8_t pixels[64]{};
            REQUIRE(source.GetMipmapData(pixels, mips[0], 0));
            for (int i = 0; i < 64; i += 4)
                CHECK(pixels[i + 3] == 200); // Authored cavity, not repacked normal X.
        }
    }
    CHECK_FALSE(Poseidon::NativeNtcCavityChannelAvailable(PacARGB8888, false, true, name));
    CHECK_FALSE(Poseidon::NativeNtcCavityChannelAvailable(Poseidon::PacBC5, true, true, name));
    CHECK_FALSE(Poseidon::NativeNtcCavityChannelAvailable(Poseidon::PacBC7, false, false, name));
    CHECK_FALSE(Poseidon::NativeNtcCavityChannelAvailable(Poseidon::PacBC7, false, true,
                                                           "Assets/Trees/Leaf_NMO.edds"));
}

TEST_CASE("Native NMO auxiliary material channels require an original four-channel DDS",
          "[graphics][edds][nmo-pbr]")
{
    const auto file = BcrBc7Alpha200(); // RGBA=(128,128,40,200): independent B/A lanes.
    const char* name = "Assets/Materials/Metal_NMO.edds";
    const auto reader = [&](const char*, std::vector<uint8_t>& out) { out = file; return true; };
    for (bool compressed : {false, true})
    {
        Poseidon::DdsPreparationOptions options{compressed, true, 1024};
        TextureSourceDDS source;
        PacLevelMem mips[16]{};
        REQUIRE(source.InitFromReader(name, mips, 16, reader, options));
        const auto format = source.GetFormat();
        REQUIRE(Poseidon::NativeNmoMaterialChannelsAvailable(format, !compressed, true, name));
        if (!compressed)
        {
            mips[0].SetDestFormat(PacARGB8888, 8);
            uint8_t pixels[64]{};
            REQUIRE(source.GetMipmapData(pixels, mips[0], 0));
            for (int i = 0; i < 64; i += 4)
            {
                CHECK(pixels[i] == 40);     // B: metalness
                CHECK(pixels[i + 3] == 200); // A: ambient occlusion
            }
        }
    }
    CHECK_FALSE(Poseidon::NativeNmoMaterialChannelsAvailable(PacARGB8888, false, true, name));
    CHECK_FALSE(Poseidon::NativeNmoMaterialChannelsAvailable(Poseidon::PacBC5, true, true, name));
    CHECK_FALSE(Poseidon::NativeNmoMaterialChannelsAvailable(Poseidon::PacBC7, false, false, name));
    CHECK_FALSE(Poseidon::NativeNmoMaterialChannelsAvailable(Poseidon::PacBC7, false, true,
                                                              "Assets/Materials/Metal_NTC.edds"));
    CHECK_FALSE(Poseidon::NativeNmoMaterialChannelsAvailable(Poseidon::PacBC7, false, true,
                                                              "Assets/Materials/Metal_NMO.paa"));
}

TEST_CASE("Original BCR roughness survives compressed, decoded and tinted routes without opacity",
          "[graphics][edds][bcr-roughness]")
{
    const auto file = BcrBc7Alpha200();
    const auto reader = [&](const char*, std::vector<uint8_t>& out) { out = file; return true; };
    REQUIRE(Poseidon::Enfusion::IsOriginalBcrRoughnessName("Assets/WaterTank_01_MLOD_BCR.edds"));
    REQUIRE(Poseidon::Enfusion::IsOriginalBcrRoughnessName(
        "enft|0.3,0.4,0.5|Assets/WaterTank_01_MLOD_BCR.edds"));
    REQUIRE_FALSE(Poseidon::Enfusion::IsOriginalBcrRoughnessName("Assets/WaterTank_01_MLOD_BCR.paa"));
    REQUIRE_FALSE(Poseidon::Enfusion::IsOriginalBcrRoughnessName(
        "enfa|coverage.edds|Assets/WaterTank_01_MLOD_BCR.edds"));
    REQUIRE(Poseidon::Enfusion::IsOpacityCompositeName(
        "enft|0.3,0.4,0.5|enfa|coverage.edds|Assets/WaterTank_01_MLOD_BCR.edds"));
    for (bool compressed : {false, true})
    {
        auto options = Poseidon::CaptureDdsPreparationOptions();
        options.compressedPassthrough = compressed;
        const char* name = "Assets/WaterTank_01_MLOD_BCR.edds";
        TextureSourceDDS source;
        PacLevelMem mips[16]{};
        REQUIRE(source.InitFromReader(name, mips, 16, reader, options));
        REQUIRE_FALSE(source.IsAlpha());
        REQUIRE(Poseidon::ClassifyTextureAlpha(source.IsAlpha(), source.IsTransparent(), false, nullptr) ==
                Poseidon::AlphaStats::Opaque);
        if (compressed)
        {
            REQUIRE(source.GetFormat() == Poseidon::PacBC7);
            mips[0].SetDestFormat(Poseidon::PacBC7, 8);
            uint8_t block[16]{};
            REQUIRE(source.GetMipmapData(block, mips[0], 0));
            REQUIRE(std::memcmp(block, file.data() + file.size() - 16, 16) == 0);
        }
        else
        {
            REQUIRE(source.GetFormat() == PacARGB8888);
            mips[0].SetDestFormat(PacARGB8888, 8);
            uint8_t pixels[64]{};
            REQUIRE(source.GetMipmapData(pixels, mips[0], 0));
            for (int i = 0; i < 64; i += 4)
                REQUIRE(pixels[i + 3] == 200);
        }
    }
    const float tint[3] = {0.3f, 0.4f, 0.5f};
    const auto tinted = Poseidon::Enfusion::MakeLayerTintName(tint, "Assets/WaterTank_01_MLOD_BCR.edds");
    TextureSourceDDS tintedSource;
    PacLevelMem tintedMips[16]{};
    REQUIRE(tintedSource.InitFromReader(tinted.c_str(), tintedMips, 16, reader,
                                        {true, true, 1024}));
    REQUIRE_FALSE(tintedSource.IsAlpha());
    tintedMips[0].SetDestFormat(PacARGB8888, 8);
    uint8_t tintedPixels[64]{};
    REQUIRE(tintedSource.GetMipmapData(tintedPixels, tintedMips[0], 0));
    for (int i = 0; i < 64; i += 4)
        REQUIRE(tintedPixels[i + 3] == 200);
    TextureSourceDDS compressedBase;
    PacLevelMem baseMips[16]{}, viewMips[16]{};
    REQUIRE(compressedBase.InitFromReader("Assets/WaterTank_01_MLOD_BCR.edds", baseMips, 16, reader,
                                          {true, true, 1024}));
    auto view = compressedBase.CreateLinearTintView(tint, viewMips, 16);
    REQUIRE(view);
    REQUIRE_FALSE(view->IsAlpha());
    viewMips[0].SetDestFormat(PacARGB8888, 8);
    uint8_t viewPixels[64]{};
    REQUIRE(view->GetMipmapData(viewPixels, viewMips[0], 0));
    for (int i = 0; i < 64; i += 4)
        REQUIRE(viewPixels[i + 3] == 200);
    // The lazy tint factory also accepts legacy *_BCR.dds names. Their alpha
    // semantics are not certified as Enfusion roughness, so keep the old
    // opaque-alpha output instead of accidentally turning them into coverage.
    TextureSourceDDS legacyBase;
    PacLevelMem legacyMips[16]{}, legacyViewMips[16]{};
    REQUIRE(legacyBase.InitFromReader("Assets/WaterTank_01_MLOD_BCR.dds", legacyMips, 16, reader,
                                      {true, true, 1024}));
    auto legacyView = legacyBase.CreateLinearTintView(tint, legacyViewMips, 16);
    REQUIRE(legacyView);
    legacyViewMips[0].SetDestFormat(PacARGB8888, 8);
    uint8_t legacyPixels[64]{};
    REQUIRE(legacyView->GetMipmapData(legacyPixels, legacyViewMips[0], 0));
    for (int i = 0; i < 64; i += 4)
        REQUIRE(legacyPixels[i + 3] == 255);
}

TEST_CASE("Enfusion opacity composite replaces BCR roughness with coverage",
          "[graphics][edds][bcr-roughness]")
{
    const auto bcr = BcrBc7Alpha200();
    const auto coverage = Bgra2x2();
    const auto reader = [&](const char* name, std::vector<uint8_t>& out) {
        if (std::strcmp(name, "coverage.edds") == 0) out = coverage;
        else if (std::strcmp(name, "colour_BCR.edds") == 0) out = bcr;
        else return false;
        return true;
    };
    TextureSourceDDS source;
    PacLevelMem mips[16]{};
    REQUIRE(source.InitFromReader("enfa|coverage.edds|colour_BCR.edds", mips, 16, reader,
                                  {true, true, 1024}));
    REQUIRE(source.IsAlpha());
    REQUIRE(Poseidon::Enfusion::IsOpacityCompositeName("enfa|coverage.edds|colour_BCR.edds"));
    REQUIRE(source.GetFormat() == PacARGB8888);
    mips[0].SetDestFormat(PacARGB8888, 8);
    uint8_t pixels[64]{};
    REQUIRE(source.GetMipmapData(pixels, mips[0], 0));
    REQUIRE(pixels[3] == 0x30);
    REQUIRE(pixels[2 * 4 + 3] == 0x31);
    REQUIRE(pixels[2 * 4 * 4 + 3] == 0x32);
    REQUIRE(pixels[(2 * 4 + 2) * 4 + 3] == 0x33);
}

TEST_CASE("DDS composition uses only its supplied reader", "[graphics][edds][streaming]")
{
    const auto file = Bgra2x2();
    std::vector<std::string> reads;
    const TextureSourceDDS::Reader reader = [&](const char* name, std::vector<uint8_t>& out) {
        reads.emplace_back(name);
        if (reads.back() != "colour.edds" && reads.back() != "coverage.edds")
            return false;
        out = file;
        return true;
    };
    TextureSourceDDS source;
    PacLevelMem mips[16];
    SECTION("plain data matches the buffer decoder")
    {
        REQUIRE(source.InitFromReader("colour.edds", mips, 16, reader));
        REQUIRE(reads == std::vector<std::string>{"colour.edds"});
        TextureSourceDDS reference;
        REQUIRE(reference.InitFromMemory(file.data(), file.size(), "colour.edds"));
        CHECK(source.GetFormat() == reference.GetFormat());
        CHECK(source.GetAverageColor() == reference.GetAverageColor());
        mips[0]._dFormat = PacARGB8888;
        mips[0]._pitch = 8;
        uint8_t actual[16]{}, expected[16]{};
        REQUIRE(source.GetMipmapData(actual, mips[0], 0));
        REQUIRE(reference.GetMipmapData(expected, mips[0], 0));
        CHECK(std::memcmp(actual, expected, sizeof(actual)) == 0);
    }
    SECTION("both halves of a composite use the reader")
    {
        REQUIRE(source.InitFromReader("enfa|coverage.edds|colour.edds", mips, 16, reader));
        const std::vector<std::string> expectedReads{"colour.edds", "coverage.edds"};
        CHECK(reads == expectedReads);
        mips[0]._dFormat = PacARGB8888;
        mips[0]._pitch = 8;
        uint8_t pixels[16]{};
        REQUIRE(source.GetMipmapData(pixels, mips[0], 0));
        for (int i = 0; i < 4; ++i)
            CHECK(pixels[i * 4 + 3] == 0x30 + i);
    }
    SECTION("failed read has no implicit file-server fallback")
    {
        CHECK_FALSE(source.InitFromReader("missing.edds", mips, 16, reader));
        CHECK(reads.size() == 1);
    }
    SECTION("solid colours require no read")
    {
        REQUIRE(source.InitFromReader("enfc|0.2,0.3,0.4", mips, 16, reader));
        CHECK(reads.empty());
    }
    SECTION("invalid arguments are refused")
    {
        CHECK_FALSE(source.InitFromReader(nullptr, mips, 16, reader));
        CHECK_FALSE(source.InitFromReader("colour.edds", nullptr, 16, reader));
        CHECK_FALSE(source.InitFromReader("colour.edds", mips, -1, reader));
        CHECK_FALSE(source.InitFromReader("colour.edds", mips, 16, {}));
        CHECK(reads.empty());
    }
}

TEST_CASE("DDS composition freezes tint settings before its reader runs", "[graphics][edds][streaming]")
{
    const bool multiply = GENERATE(false, true);
    const ScopedLayerTintMode mode(multiply);
    const auto file = Bgra2x2();
    std::vector<Poseidon::DDSMipLevel> expected(1);
    expected[0].width = expected[0].height = 2;
    expected[0].data.assign(file.end() - 16, file.end());
    const float tint[3] = {0.5f, 0.25f, 0.125f};
    Poseidon::ApplyEnfusionLayerTint(expected, tint);
    const auto name = Poseidon::Enfusion::MakeLayerTintName(tint, "colour.edds");
    TextureSourceDDS source;
    PacLevelMem mips[16];
    REQUIRE(source.InitFromReader(name.c_str(), mips, 16,
        [&](const char*, std::vector<uint8_t>& out) {
            Poseidon::Enfusion::GLayerTintLinearMultiply.store(multiply ? 0 : 1);
            out = file;
            return true;
        }));
    mips[0]._dFormat = PacARGB8888;
    mips[0]._pitch = 8;
    std::vector<uint8_t> pixels(16);
    REQUIRE(source.GetMipmapData(pixels.data(), mips[0], 0));
    CHECK(pixels == expected[0].data);
}

TEST_CASE("DDS composition uses supplied compression settings and rejects invalid limits", "[graphics][edds][streaming]")
{
    auto file = Header(4, 4, 1, "DX10");
    for (uint32_t word : {80u, 3u, 0u, 1u, 0u, 0x59504F43u, 8u}) AppendU32(file, word);
    file.insert(file.end(), 8, 0); // BC4 zero block
    int reads = 0;
    const TextureSourceDDS::Reader reader = [&](const char*, std::vector<uint8_t>& out) {
        ++reads;
        out = file;
        return true;
    };
    for (bool compressed : {false, true}) {
        auto options = Poseidon::CaptureDdsPreparationOptions();
        options.compressedPassthrough = compressed;
        TextureSourceDDS source;
        PacLevelMem mips[16];
        REQUIRE(source.InitFromReader("mask.edds", mips, 16, reader, options));
        CHECK((source.GetFormat() == PacARGB8888) == !compressed);
        for (int invalid : {0, 63, 8193}) {
            options.decodedMaxEdge = invalid;
            const int previousReads = reads;
            CHECK_FALSE(source.InitFromReader("mask.edds", mips, 16, reader, options));
            CHECK(reads == previousReads);
        }
    }
}

TEST_CASE("Prepared DDS ownership uses the existing store lifecycle", "[graphics][edds][streaming][dds-handoff]")
{
    if (!Poseidon::render::PreparedTextureStore::Enabled()) SKIP("Requires texture preparation");
    auto& store = Poseidon::render::PreparedTextureStore::Instance();
    store.Clear();
    const auto options = Poseidon::CaptureDdsPreparationOptions();
    const auto file = Bgra2x2();
    const auto make = [&] {
        auto source = std::make_unique<TextureSourceDDS>();
        PacLevelMem mips[16];
        REQUIRE(source->InitFromReader("colour.edds", mips, 16,
            [&](const char*, std::vector<uint8_t>& out) { out = file; return true; }, options));
        return source;
    };
    SECTION("ownership moves without copying pixels") {
        auto source = make();
        auto* identity = source.get();
        REQUIRE(store.PutDdsPrepared("colour.edds", std::move(source), options, store.Generation()));
        CHECK(store.SnapshotStats().bytes >= 16);
        CHECK_FALSE(store.ShouldPrepare("colour.edds"));
        auto result = store.TakeDdsPrepared("colour.edds", options);
        REQUIRE(result.get() == identity);
        PacLevelMem mip;
        mip._w = mip._h = 2; mip._pitch = 8; mip._dFormat = PacARGB8888;
        std::vector<uint8_t> actual(16);
        REQUIRE(result->GetMipmapData(actual.data(), mip, 0));
        CHECK(actual == std::vector<uint8_t>(file.end() - 16, file.end()));
        CHECK(store.SnapshotStats().bytes == 0);
        CHECK_FALSE(store.TakeDdsPrepared("colour.edds", options));
    }
    SECTION("clear invalidates publication without replacing a fresh entry") {
        const auto oldGeneration = store.Generation();
        auto stale = make();
        store.Clear();
        auto fresh = make();
        auto* identity = fresh.get();
        REQUIRE(store.PutDdsPrepared("colour.edds", std::move(fresh), options, store.Generation()));
        CHECK_FALSE(store.PutDdsPrepared("colour.edds", std::move(stale), options, oldGeneration));
        CHECK(store.TakeDdsPrepared("colour.edds", options).get() == identity);
    }
    SECTION("each configuration mismatch retires the old result") {
        for (int field = 0; field < 3; ++field) {
            REQUIRE(store.PutDdsPrepared("colour.edds", make(), options, store.Generation()));
            auto changed = options;
            if (field == 0) changed.compressedPassthrough = !changed.compressedPassthrough;
            if (field == 1) changed.linearTintMultiply = !changed.linearTintMultiply;
            if (field == 2) changed.decodedMaxEdge = options.decodedMaxEdge == 64 ? 128 : 64;
            const auto misses = store.SnapshotStats().ddsConfigurationMisses;
            CHECK_FALSE(store.TakeDdsPrepared("colour.edds", changed));
            CHECK(store.SnapshotStats().ddsConfigurationMisses == misses + 1);
            CHECK(store.SnapshotStats().bytes == 0);
            CHECK(store.ShouldPrepare("colour.edds"));
        }
    }
    SECTION("upload attempt and eviction share the existing marks") {
        REQUIRE(store.PutDdsPrepared("colour.edds", make(), options, store.Generation()));
        store.MarkUploaded("colour.edds");
        CHECK(store.SnapshotStats().bytes == 0);
        CHECK_FALSE(store.TakeDdsPrepared("colour.edds", options));
        CHECK_FALSE(store.PutDdsPrepared("colour.edds", make(), options, store.Generation()));
        store.MarkEvicted("colour.edds");
        REQUIRE(store.PutDdsPrepared("colour.edds", make(), options, store.Generation()));
    }
    SECTION("invalid payload and duplicate cannot overwrite a valid result") {
        CHECK_FALSE(store.PutDdsPrepared("colour.edds", nullptr, options, store.Generation()));
        CHECK_FALSE(store.PutDdsPrepared("wrong.edds", make(), options, store.Generation()));
        CHECK_FALSE(store.PutDdsPrepared("colour.edds", std::make_unique<TextureSourceDDS>(), options, store.Generation()));
        auto first = make();
        auto* identity = first.get();
        REQUIRE(store.PutDdsPrepared("colour.edds", std::move(first), options, store.Generation()));
        CHECK_FALSE(store.PutDdsPrepared("colour.edds", make(), options, store.Generation()));
        CHECK(store.TakeDdsPrepared("colour.edds", options).get() == identity);
    }
    store.Clear();
}

TEST_CASE("Prepared DDS respects shared byte limits and expiry", "[graphics][edds][streaming][dds-handoff]")
{
    auto& store = Poseidon::render::PreparedTextureStore::Instance();
    store.Clear();
    auto options = Poseidon::CaptureDdsPreparationOptions();
    options.decodedMaxEdge = 1024;
    auto file = Header(1024, 1024, 1);
    AppendU32(file, 0x59504F43);
    AppendU32(file, 4 * 1024 * 1024);
    file.insert(file.end(), 4 * 1024 * 1024, 128);
    auto source = std::make_unique<TextureSourceDDS>();
    PacLevelMem mips[16];
    REQUIRE(source->InitFromReader("large.edds", mips, 16,
        [&](const char*, std::vector<uint8_t>& out) { out = file; return true; }, options));
    const bool fits = store.Enabled() && store.ByteBudget() > 4 * 1024 * 1024 + 4096;
    REQUIRE(store.PutDdsPrepared("large.edds", std::move(source), options, store.Generation()) == fits);
    CHECK(store.SnapshotStats().bytes <= store.ByteBudget());
    if (!fits) {
        CHECK(store.SnapshotStats().entries == 0);
    } else if (store.TtlSeconds() > 0 && store.TtlSeconds() < 0.1) {
        // Dedicated short-TTL test process, never wait for the production 30 s TTL.
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK_FALSE(store.TakeDdsPrepared("large.edds", options));
        CHECK(store.SnapshotStats().bytes == 0);
    } else {
        REQUIRE(store.TakeDdsPrepared("large.edds", options));
        CHECK(store.SnapshotStats().bytes == 0);
    }
    store.Clear();
}

TEST_CASE("DDS factory consumes a prepared source without reopening its file", "[graphics][edds][streaming][dds-factory]")
{
    auto& store = Poseidon::render::PreparedTextureStore::Instance();
    if (!store.NativeDdsEnabled()) SKIP("Run with WGR_NATIVE_DDS_PREPARE=1");
    store.Clear();
    const auto options = Poseidon::CaptureDdsPreparationOptions();
    const auto bytes = Bgra2x2();
    auto source = std::make_unique<TextureSourceDDS>();
    PacLevelMem preparedMips[16], usedMips[1];
    const char* name = "nonexistent-prepared-fixture.edds";
    REQUIRE(source->InitFromReader(name, preparedMips, 16,
        [&](const char*, std::vector<uint8_t>& out) { out = bytes; return true; }, options));
    auto* identity = source.get();
    REQUIRE(store.PutDdsPrepared(name, std::move(source), options, store.Generation()));
    Poseidon::TextureSourceDDSFactory factory;
    std::unique_ptr<Poseidon::ITextureSource> result(factory.Create(name, usedMips, 1));
    REQUIRE(result.get() == identity);
    CHECK(result->GetMipmapCount() == 1);
    CHECK(usedMips[0]._w == 2);
    CHECK(usedMips[0]._h == 2);
    CHECK(store.SnapshotStats().bytes == 0);
    store.Clear();
}

TEST_CASE("Reusable DDS payloads preserve source identity and independent pixels", "[graphics][edds][streaming]")
{
    auto& store = Poseidon::render::PreparedTextureStore::Instance();
    store.Clear();
    const auto file = Bgra2x2();
    TextureSourceDDS source;
    REQUIRE(source.InitFromMemory(file.data(), file.size(), "test.edds", true));
    REQUIRE(store.PutDdsDecode("dds-decode|test.edds", file, source));
    CHECK_FALSE(store.PutDdsDecode("dds-decode|test.edds", file, source));
    CHECK(store.SnapshotStats().ddsReuseBytes > file.size());
    TextureSourceDDS copy;
    REQUIRE(store.CopyDdsDecode("dds-decode|test.edds", file, copy));
    CHECK(copy.GetFormat() == source.GetFormat());
    CHECK(copy.GetMipmapCount() == source.GetMipmapCount());
    CHECK(copy.IsAlpha() == source.IsAlpha());
    CHECK(copy.IsTransparent() == source.IsTransparent());
    PacLevelMem mip;
    mip._w = mip._h = 2; mip._pitch = 8; mip._dFormat = PacARGB8888;
    std::vector<uint8_t> expected(16), actual(16);
    REQUIRE(source.GetMipmapData(expected.data(), mip, 0));
    REQUIRE(copy.GetMipmapData(actual.data(), mip, 0));
    CHECK(actual == expected);
    auto changed = file;
    changed.back() ^= 0x40;
    CHECK_FALSE(store.CopyDdsDecode("dds-decode|test.edds", changed, copy));
    REQUIRE(copy.InitFromMemory(changed.data(), changed.size(), "changed.edds", true));
    Poseidon::PAABlockChain chain;
    CHECK_FALSE(store.Take("dds-decode|test.edds", chain));
    store.MarkUploaded("test.edds"); // A derived GPU texture is not its shared source.
    REQUIRE(store.CopyDdsDecode("dds-decode|test.edds", file, copy));
    REQUIRE(copy.GetMipmapData(actual.data(), mip, 0));
    CHECK(actual == expected);
    store.Clear();
    CHECK(store.SnapshotStats().ddsReuseBytes == 0);
    CHECK(store.SnapshotStats().bytes == 0);
    CHECK_FALSE(store.CopyDdsDecode("dds-decode|test.edds", file, source));
    REQUIRE(copy.GetMipmapData(actual.data(), mip, 0));
    CHECK(actual == expected); // Clearing the store cannot invalidate a consumer.
    const size_t cap = std::min<size_t>(64u * 1024u * 1024u, store.ByteBudget() / 4);
    const std::vector<uint8_t> oversized(cap, 0);
    CHECK_FALSE(store.PutDdsDecode("dds-decode|oversized.edds", oversized, copy));
    CHECK(store.SnapshotStats().bytes == 0);
    REQUIRE(store.PutDdsDecode("dds-decode|test.edds", file, copy));
    store.MarkUploaded("dds-decode|test.edds");
    CHECK(store.SnapshotStats().ddsReuseBytes == 0);
    CHECK(store.SnapshotStats().bytes == 0);
    store.Clear();
}

TEST_CASE("DDS source: a 32-bit level is delivered as BGRA, honouring pitch", "[graphics][edds][dz-002]")
{
    const std::vector<uint8_t> file = Bgra2x2();
    TextureSourceDDS source;
    REQUIRE(source.InitFromMemory(file.data(), file.size(), "test.edds"));
    REQUIRE(source.GetMipmapCount() == 1);
    REQUIRE(source.GetFormat() == PacARGB8888);

    PacLevelMem mip;
    mip._w = 2;
    mip._h = 2;
    mip._dFormat = PacARGB8888;

    SECTION("tight rows")
    {
        mip._pitch = 8; // 2 texels x 4 bytes
        uint8_t out[16] = {};
        REQUIRE(source.GetMipmapData(out, mip, 0));
        // Byte order is preserved: the renderer's ARGB8888 is B,G,R,A in memory.
        REQUIRE(out[0] == 0x10);
        REQUIRE(out[3] == 0xFF);
        REQUIRE(out[12] == 0x13);
    }

    SECTION("padded rows")
    {
        // A destination row wider than the source is the case that a plain memcpy of the
        // whole level gets wrong: row 1 would land 8 bytes early and the image would
        // shear. 0xCC marks the padding so an overrun is visible.
        mip._pitch = 12;
        uint8_t out[24];
        std::memset(out, 0xCC, sizeof(out));
        REQUIRE(source.GetMipmapData(out, mip, 0));

        REQUIRE(out[0] == 0x10); // row 0, texel 0
        REQUIRE(out[4] == 0x11); // row 0, texel 1
        REQUIRE(out[8] == 0xCC); // row 0 padding, untouched
        REQUIRE(out[9] == 0xCC);
        REQUIRE(out[12] == 0x12); // row 1 starts at the pitch, not at 8
        REQUIRE(out[16] == 0x13);
        REQUIRE(out[20] == 0xCC); // row 1 padding, untouched
    }
}

TEST_CASE("DDS source: 32-bit converts down to the 16-bit destinations", "[graphics][edds][dz-002]")
{
    const std::vector<uint8_t> file = Bgra2x2();
    TextureSourceDDS source;
    REQUIRE(source.InitFromMemory(file.data(), file.size(), "test.edds"));

    PacLevelMem mip;
    mip._w = 2;
    mip._h = 2;
    mip._pitch = 4;

    SECTION("ARGB1555 keeps the high bits and thresholds alpha")
    {
        mip._dFormat = PacARGB1555;
        uint16_t out[4] = {};
        REQUIRE(source.GetMipmapData(out, mip, 0));
        // Texel 0 is B=0x10 G=0x20 R=0x30 A=0xFF -> (1, 0b00110, 0b00100, 0b00010).
        REQUIRE((out[0] & 0x8000) != 0);          // alpha 0xFF is above the threshold
        REQUIRE(((out[0] >> 10) & 0x1F) == 0x06); // R 0x30 >> 3
        REQUIRE(((out[0] >> 5) & 0x1F) == 0x04);  // G 0x20 >> 3
        REQUIRE((out[0] & 0x1F) == 0x02);         // B 0x10 >> 3
    }

    SECTION("RGB565 gives green its extra bit")
    {
        mip._dFormat = PacRGB565;
        uint16_t out[4] = {};
        REQUIRE(source.GetMipmapData(out, mip, 0));
        REQUIRE(((out[0] >> 11) & 0x1F) == 0x06); // R
        REQUIRE(((out[0] >> 5) & 0x3F) == 0x08);  // G 0x20 >> 2
        REQUIRE((out[0] & 0x1F) == 0x02);         // B
    }
}

TEST_CASE("DDS source: block levels transfer verbatim and only to their own format", "[graphics][edds][dz-002]")
{
    // Blocks are deliberately NOT decompressed on this path: DayZ worlds hold ~28,000
    // textures at 1k-4k and inflating each one 8x on the way in is the one thing this
    // must not do (DZ-002). So a DXT level is a memcpy, and a mismatched destination is
    // an error rather than a silent conversion.
    std::vector<uint8_t> file = Header(4, 4, 1, "DXT1");
    AppendU32(file, 0x59504F43); // 'COPY'
    AppendU32(file, 8);          // one 4x4 DXT1 block
    const uint8_t block[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
    file.insert(file.end(), block, block + 8);

    TextureSourceDDS source;
    REQUIRE(source.InitFromMemory(file.data(), file.size(), "block.edds"));
    REQUIRE(source.GetFormat() == PacDXT1);

    PacLevelMem mip;
    mip._w = 4;
    mip._h = 4;
    mip._dFormat = PacDXT1;
    uint8_t out[8] = {};
    REQUIRE(source.GetMipmapData(out, mip, 0));
    REQUIRE(std::memcmp(out, block, 8) == 0);

    // Asking for it as anything else must fail rather than produce rubbish.
    mip._dFormat = PacARGB8888;
    uint8_t wide[64] = {};
    REQUIRE_FALSE(source.GetMipmapData(wide, mip, 0));
}

TEST_CASE("DDS source: a level whose extents disagree with the caller is refused", "[graphics][edds][dz-002]")
{
    // The mip table and the caller's PacLevelMem are two independent descriptions of the
    // same level. If they disagree, writing anyway would overrun or shear.
    const std::vector<uint8_t> file = Bgra2x2();
    TextureSourceDDS source;
    REQUIRE(source.InitFromMemory(file.data(), file.size(), "test.edds"));

    PacLevelMem mip;
    mip._w = 4; // the file says 2
    mip._h = 2;
    mip._pitch = 16;
    mip._dFormat = PacARGB8888;
    uint8_t out[32] = {};
    REQUIRE_FALSE(source.GetMipmapData(out, mip, 0));

    // And a level index past the end is refused too.
    mip._w = 2;
    mip._pitch = 8;
    REQUIRE_FALSE(source.GetMipmapData(out, mip, 1));
    REQUIRE_FALSE(source.GetMipmapData(out, mip, -1));
}

TEST_CASE("DDS source: mip 0 is the largest level", "[graphics][edds][dz-002]")
{
    // The on-disk chunk table runs smallest-first; a source that forwarded that order
    // would hand the renderer a 1x1 smear as its base level.
    std::vector<uint8_t> file = Header(2, 2, 2);
    AppendU32(file, 0x59504F43);
    AppendU32(file, 4); // 1x1
    AppendU32(file, 0x59504F43);
    AppendU32(file, 16); // 2x2
    file.insert(file.end(), 4, 0x77);
    file.insert(file.end(), 16, 0x22);

    TextureSourceDDS source;
    REQUIRE(source.InitFromMemory(file.data(), file.size(), "mips.edds"));
    REQUIRE(source.GetMipmapCount() == 2);

    PacLevelMem mip;
    mip._w = 2;
    mip._h = 2;
    mip._pitch = 8;
    mip._dFormat = PacARGB8888;
    uint8_t out[16] = {};
    REQUIRE(source.GetMipmapData(out, mip, 0));
    REQUIRE(out[0] == 0x22); // the 2x2 fill, not the 1x1 one
}

// ---------------------------------------------------------------------------------
// RFG-070: the `enft|r,g,b|` layer tint.
//
// Everon's buildings read as one pale grey because a `MatPBRMulti` names no albedo of its
// own -- it names a `BCR_N`, which is a SHARED one-metre library tile, and 436 of them
// serve 2,889 of the island's structure materials. The per-layer `Color_N` beside the tile
// is what makes a wall its own colour, and the native path threw it away.
//
// Two things are pinned here and they are the two that fail silently.

TEST_CASE("Enfusion layer tint: r,g,b lands in R,G,B and not in B,G,R", "[graphics][edds][rfg-070]")
{
    const bool multiply = GENERATE(false, true);
    const ScopedLayerTintMode mode(multiply);
    // The trap: decoded levels are ARGB8888, which in this engine means B,G,R,A in memory.
    // A tint applied in index order paints a beige wall blue -- and it still LOOKS like a
    // working tint, because the image does change colour. Only a per-channel assertion
    // catches it, so the target here is deliberately lopsided: strongly red, barely blue.
    std::vector<Poseidon::DDSMipLevel> levels(1);
    levels[0].width = 2;
    levels[0].height = 2;
    levels[0].data.assign(2 * 2 * 4, 0);
    for (size_t t = 0; t < 4; ++t)
    {
        levels[0].data[t * 4 + 0] = 128; // B
        levels[0].data[t * 4 + 1] = 128; // G
        levels[0].data[t * 4 + 2] = 128; // R
        levels[0].data[t * 4 + 3] = 200; // A -- roughness on a _BCR; must survive untouched
    }
    const float rgb[3] = {0.9f, 0.5f, 0.05f}; // linear
    Poseidon::ApplyEnfusionLayerTint(levels, rgb);

    const uint8_t b = levels[0].data[0];
    const uint8_t g = levels[0].data[1];
    const uint8_t r = levels[0].data[2];
    REQUIRE(r > g);
    REQUIRE(g > b);
    // Independent sRGB reference values: multiply the decoded 128/255 texel,
    // or, only in the legacy arm, rescale its mean to the authored colour.
    REQUIRE(static_cast<int>(r) == (multiply ? 122 : 243));
    REQUIRE(static_cast<int>(g) == (multiply ? 92 : 188));
    REQUIRE(static_cast<int>(b) == (multiply ? 27 : 63));
    // Alpha is coverage or roughness and is never the tint's business.
    REQUIRE(levels[0].data[3] == 200);
}

TEST_CASE("Enfusion layer tint: one gain for the whole mip chain", "[graphics][edds][rfg-070]")
{
    const bool multiply = GENERATE(false, true);
    const ScopedLayerTintMode mode(multiply);
    // A per-level mean would give each mip its own gain, and the same wall would change
    // colour as you walked away from it. The gain comes from level 0 and is applied to all.
    std::vector<Poseidon::DDSMipLevel> levels(2);
    levels[0].width = 2;
    levels[0].height = 2;
    levels[0].data.assign(2 * 2 * 4, 100);
    levels[1].width = 1;
    levels[1].height = 1;
    levels[1].data.assign(4, 100);
    const float rgb[3] = {0.25f, 0.25f, 0.25f};
    Poseidon::ApplyEnfusionLayerTint(levels, rgb);
    REQUIRE(levels[0].data[0] == levels[1].data[0]);
    REQUIRE(levels[0].data[2] == levels[1].data[2]);
}

TEST_CASE("Enfusion layer tint: a zero channel is left alone", "[graphics][edds][rfg-070]")
{
    const bool multiply = GENERATE(false, true);
    const ScopedLayerTintMode mode(multiply);
    // Zero remains zero in both modes, including the legacy target/mean path.
    std::vector<Poseidon::DDSMipLevel> levels(1);
    levels[0].width = 1;
    levels[0].height = 1;
    levels[0].data = {0, 60, 60, 255}; // B dead
    const float rgb[3] = {0.5f, 0.5f, 0.5f};
    Poseidon::ApplyEnfusionLayerTint(levels, rgb);
    REQUIRE(levels[0].data[0] == 0);
    REQUIRE(static_cast<int>(levels[0].data[1]) == (multiply ? 41 : 188));
    REQUIRE(static_cast<int>(levels[0].data[2]) == (multiply ? 41 : 188));
    REQUIRE(levels[0].data[3] == 255);
}

TEST_CASE("Enfusion composite names nest: tint outside coverage", "[graphics][edds][rfg-070]")
{
    // The claim this pins is COMPOSABILITY. A leaf card on a tinted shared tile is both
    // forms at once, and three separate places in the renderer test `strncmp(name,
    // "enfa|", 5)` by hand to decide whether a section is a cutout, whether the material's
    // Stage0 may override the face texture, and which pass it draws in. Wrapping a tint
    // around the name must not change any of those three answers -- so every one of them
    // reads through SkipLayerTint, and this is the test that says what that has to do.
    const float rgb[3] = {0.385f, 0.356f, 0.322f}; // the corpus median Color_N
    const std::string inner = "enfa|assets/x_A.edds|assets/x_BCR.edds";
    const std::string name = Poseidon::Enfusion::MakeLayerTintName(rgb, inner);

    REQUIRE(name.rfind("enft|", 0) == 0);
    REQUIRE(std::string(Poseidon::Enfusion::SkipLayerTint(name.c_str())) == inner);
    REQUIRE(std::strncmp(Poseidon::Enfusion::SkipLayerTint(name.c_str()), "enfa|", 5) == 0);

    float back[3] = {0.0f, 0.0f, 0.0f};
    const char* rest = nullptr;
    REQUIRE(Poseidon::Enfusion::SplitLayerTintName(name.c_str(), back, &rest));
    REQUIRE(std::string(rest) == inner);
    for (int c = 0; c < 3; ++c)
        REQUIRE(std::abs(back[c] - rgb[c]) < 1e-3f);

    // An untinted name passes through untouched, which is what keeps every non-Enfusion
    // world inert: nothing else in the engine ever builds one of these.
    const char* plain = "data\\wall.paa";
    REQUIRE(Poseidon::Enfusion::SkipLayerTint(plain) == plain);
    REQUIRE(!Poseidon::Enfusion::SplitLayerTintName(plain, back, &rest));
    REQUIRE(!Poseidon::Enfusion::SplitLayerTintName("enft|0.5,0.5,0.5", back, &rest)); // no inner
}
