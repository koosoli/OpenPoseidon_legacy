#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <Poseidon/Graphics/Textures/DDSConverter.hpp>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/Graphics/Textures/PAAEncoder.hpp>
#include <Poseidon/Graphics/Textures/Image.hpp>
#include "test_fixtures.hpp"
#include <cstring>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdint.h>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using namespace Poseidon;

// --- DDS header constants for verification ---
static constexpr uint32_t DDS_MAGIC = 0x20534444;
static constexpr uint32_t DDS_HEADER_SIZE = 124;
static constexpr uint32_t DDPF_FOURCC = 0x4;

static uint32_t readU32(const uint8_t* p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// RAII temp file -- uses exe-relative path to avoid directory-not-found issues
struct TmpFile
{
    std::string path;
    TmpFile(const char* name) : path(std::string(TestFixtures::GetExecutableDirectory()) + "/tmp/dds_test_" + name)
    {
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    }
    ~TmpFile() { std::remove(path.c_str()); }
};

// --- PAA->DDS: DXT1 raw block copy ---

TEST_CASE("DDSConverter: PAA DXT1 -> DDS preserves raw blocks", "[Graphics][DDS]")
{
    auto dds = DDSConverter::PAAFileToDDS(GET_FIXTURE("texture/paa/synthetic_dxt1.paa"));
    REQUIRE(dds.valid());
    REQUIRE(dds.format == PixelFormat::DXT1);
    REQUIRE(dds.width == 64);
    REQUIRE(dds.height == 64);
    REQUIRE(!dds.mipmaps.empty());

    // DXT1 block size: ((64+3)/4) * ((64+3)/4) * 8 = 2048 bytes
    REQUIRE(dds.mipmaps[0].data.size() == 2048);
    REQUIRE(dds.mipmaps[0].width == 64);
    REQUIRE(dds.mipmaps[0].height == 64);
}

TEST_CASE("DDSConverter: PAA DXT5 -> DDS preserves raw blocks", "[Graphics][DDS]")
{
    auto dds = DDSConverter::PAAFileToDDS(GET_FIXTURE("paa/synthetic_dxt5.paa"));
    REQUIRE(dds.valid());
    REQUIRE(dds.format == PixelFormat::DXT5);
    REQUIRE(dds.width == 32);
    REQUIRE(dds.height == 32);
    REQUIRE(!dds.mipmaps.empty());

    // DXT5: 8 * 8 blocks * 16 bytes = 1024
    REQUIRE(dds.mipmaps[0].data.size() == 1024);
}

TEST_CASE("DDSConverter: PAA DXT3 -> DDS", "[Graphics][DDS]")
{
    auto dds = DDSConverter::PAAFileToDDS(GET_FIXTURE("texture/paa/synthetic_dxt3.paa"));
    REQUIRE(dds.valid());
    REQUIRE(dds.format == PixelFormat::DXT3);
    REQUIRE(!dds.mipmaps.empty());
}

TEST_CASE("DDSConverter: PAA DXT5 fixture remains DXT5", "[Graphics][DDS]")
{
    auto dds = DDSConverter::PAAFileToDDS(GET_FIXTURE("texture/paa/synthetic_dxt5.paa"));
    REQUIRE(dds.valid());
    REQUIRE(dds.format == PixelFormat::DXT5);
    REQUIRE(dds.width == 32);
    REQUIRE(dds.height == 32);
    REQUIRE(dds.mipmaps[0].data.size() == 1024);
}

// --- WriteDDS buffer: verify DDS header structure ---

TEST_CASE("DDSConverter: WriteDDS buffer has correct header", "[Graphics][DDS]")
{
    auto dds = DDSConverter::PAAFileToDDS(GET_FIXTURE("texture/paa/synthetic_dxt1.paa"));
    REQUIRE(dds.valid());

    auto buf = DDSConverter::WriteDDSBuffer(dds);
    REQUIRE(buf.size() > 128);

    // Magic
    REQUIRE(readU32(buf.data()) == DDS_MAGIC);
    // Header size
    REQUIRE(readU32(buf.data() + 4) == DDS_HEADER_SIZE);
    // Dimensions
    REQUIRE(readU32(buf.data() + 12) == 64); // height
    REQUIRE(readU32(buf.data() + 16) == 64); // width
    // PixelFormat offset at 76: pf.size at +76
    REQUIRE(readU32(buf.data() + 4 + 72) == 32); // sizeof(DDS_PIXELFORMAT)
    // FourCC flags
    REQUIRE((readU32(buf.data() + 4 + 76) & DDPF_FOURCC) != 0);
    // FourCC value: "DXT1"
    uint32_t fourCC = readU32(buf.data() + 4 + 80);
    char cc[5] = {};
    std::memcpy(cc, &fourCC, 4);
    REQUIRE(std::string(cc) == "DXT1");
}

TEST_CASE("DDSConverter: WriteDDS DXT5 header fourCC", "[Graphics][DDS]")
{
    auto dds = DDSConverter::PAAFileToDDS(GET_FIXTURE("paa/synthetic_dxt5.paa"));
    auto buf = DDSConverter::WriteDDSBuffer(dds);
    REQUIRE(buf.size() > 128);

    uint32_t fourCC = readU32(buf.data() + 4 + 80);
    char cc[5] = {};
    std::memcpy(cc, &fourCC, 4);
    REQUIRE(std::string(cc) == "DXT5");
    REQUIRE(readU32(buf.data() + 12) == 32); // height
    REQUIRE(readU32(buf.data() + 16) == 32); // width
}

// --- Write to file + read back ---

TEST_CASE("DDSConverter: write DDS file and read back", "[Graphics][DDS]")
{
    TmpFile tmp("roundtrip.dds");

    auto dds = DDSConverter::PAAFileToDDS(GET_FIXTURE("texture/paa/synthetic_dxt1.paa"));
    REQUIRE(dds.valid());
    REQUIRE(DDSConverter::WriteDDS(tmp.path, dds));

    auto dds2 = DDSConverter::ReadDDS(tmp.path);
    REQUIRE(dds2.valid());
    REQUIRE(dds2.format == PixelFormat::DXT1);
    REQUIRE(dds2.width == 64);
    REQUIRE(dds2.height == 64);
    REQUIRE(dds2.mipmaps.size() == dds.mipmaps.size());

    // Verify mip0 data is identical (raw block copy)
    REQUIRE(dds2.mipmaps[0].data.size() == dds.mipmaps[0].data.size());
    REQUIRE(dds2.mipmaps[0].data == dds.mipmaps[0].data);
}

TEST_CASE("DDSConverter: DXT5 write+read roundtrip preserves all mipmaps", "[Graphics][DDS]")
{
    TmpFile tmp("roundtrip_dxt5.dds");

    auto dds = DDSConverter::PAAFileToDDS(GET_FIXTURE("paa/synthetic_dxt5.paa"));
    REQUIRE(dds.valid());
    REQUIRE(DDSConverter::WriteDDS(tmp.path, dds));

    auto dds2 = DDSConverter::ReadDDS(tmp.path);
    REQUIRE(dds2.valid());
    REQUIRE(dds2.format == PixelFormat::DXT5);
    REQUIRE(dds2.mipmaps.size() == dds.mipmaps.size());

    for (size_t i = 0; i < dds.mipmaps.size(); ++i)
    {
        REQUIRE(dds2.mipmaps[i].width == dds.mipmaps[i].width);
        REQUIRE(dds2.mipmaps[i].height == dds.mipmaps[i].height);
        REQUIRE(dds2.mipmaps[i].data == dds.mipmaps[i].data);
    }
}

// --- Mipmaps preserved through DDS ---

TEST_CASE("DDSConverter: DXT1 PAA mipmaps survive DDS roundtrip", "[Graphics][DDS]")
{
    PAAInfo info;
    REQUIRE(ReadPAAInfo(GET_FIXTURE("texture/paa/synthetic_dxt1.paa"), info));

    auto dds = DDSConverter::PAAFileToDDS(GET_FIXTURE("texture/paa/synthetic_dxt1.paa"));
    REQUIRE(dds.valid());
    REQUIRE(static_cast<int>(dds.mipmaps.size()) == info.mipmapCount);

    // Verify mip dimensions halve correctly
    int w = dds.width, h = dds.height;
    for (auto& m : dds.mipmaps)
    {
        REQUIRE(m.width == w);
        REQUIRE(m.height == h);
        w = (std::max)(w / 2, 1);
        h = (std::max)(h / 2, 1);
    }
}

// --- DDS -> PAA roundtrip (full pixel roundtrip) ---

static double computePSNR(const uint8_t* a, const uint8_t* b, int n)
{
    double mse = 0;
    for (int i = 0; i < n; ++i)
    {
        double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        mse += d * d;
    }
    mse /= n;
    if (mse == 0.0)
        return 100.0;
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}

TEST_CASE("DDSConverter: PAA->DDS->decode produces same pixels as PAA->decode", "[Graphics][DDS]")
{
    // Decode original PAA
    auto origDecoded = DecodePAAFile(GET_FIXTURE("texture/paa/synthetic_dxt1.paa"));
    REQUIRE(origDecoded.valid());

    // Convert PAA->DDS (raw block copy)
    auto dds = DDSConverter::PAAFileToDDS(GET_FIXTURE("texture/paa/synthetic_dxt1.paa"));
    REQUIRE(dds.valid());

    // Write DDS, load as Image, decode to RGBA
    TmpFile tmp("pixel_compare.dds");
    REQUIRE(DDSConverter::WriteDDS(tmp.path, dds));

    auto img = Image::FromFile(tmp.path);
    REQUIRE(img.valid());
    auto rgba = img.ToRGBA();
    REQUIRE(rgba.valid());

    // DXT1 raw blocks are identical, so decoded pixels must match exactly
    REQUIRE(rgba.width() == origDecoded.width);
    REQUIRE(rgba.height() == origDecoded.height);

    double psnr = computePSNR(rgba.data().data(), origDecoded.rgba.data(), origDecoded.width * origDecoded.height * 4);
    REQUIRE(psnr == 100.0); // Lossless: same raw DXT blocks
}

// --- Image::FromFile DDS support ---

TEST_CASE("Image::FromFile loads DDS", "[Graphics][DDS][Image]")
{
    TmpFile tmp("image_load.dds");

    auto dds = DDSConverter::PAAFileToDDS(GET_FIXTURE("texture/paa/synthetic_dxt1.paa"));
    REQUIRE(DDSConverter::WriteDDS(tmp.path, dds));

    auto img = Image::FromFile(tmp.path);
    REQUIRE(img.valid());
    REQUIRE(img.width() == 64);
    REQUIRE(img.height() == 64);
    REQUIRE(img.format() == PixelFormat::DXT1);
}

// --- Image::Save to DDS ---

TEST_CASE("Image::Save writes DDS from RGBA", "[Graphics][DDS][Image]")
{
    TmpFile tmp("image_save.dds");

    // Create gradient RGBA image
    int w = 16, h = 16;
    std::vector<uint8_t> rgba(w * h * 4);
    for (int i = 0; i < w * h; ++i)
    {
        rgba[i * 4 + 0] = static_cast<uint8_t>(i);
        rgba[i * 4 + 1] = static_cast<uint8_t>(255 - i);
        rgba[i * 4 + 2] = 128;
        rgba[i * 4 + 3] = 255;
    }
    auto img = Image::FromRGBA(w, h, std::move(rgba));
    REQUIRE(img.Save(tmp.path));

    auto dds = DDSConverter::ReadDDS(tmp.path);
    REQUIRE(dds.valid());
    REQUIRE(dds.format == PixelFormat::RGBA8888);
    REQUIRE(dds.width == 16);
    REQUIRE(dds.height == 16);
    REQUIRE(dds.mipmaps[0].data.size() == 16 * 16 * 4);
    // Verify first pixel
    REQUIRE(dds.mipmaps[0].data[0] == 0);   // R
    REQUIRE(dds.mipmaps[0].data[1] == 255); // G
    REQUIRE(dds.mipmaps[0].data[2] == 128); // B
    REQUIRE(dds.mipmaps[0].data[3] == 255); // A
}

// --- Invalid inputs ---

TEST_CASE("DDSConverter: ReadDDS rejects empty file", "[Graphics][DDS]")
{
    auto dds = DDSConverter::ReadDDSBuffer(nullptr, 0);
    REQUIRE_FALSE(dds.valid());
}

TEST_CASE("DDSConverter: ReadDDS rejects invalid magic", "[Graphics][DDS]")
{
    uint8_t fake[256] = {};
    fake[0] = 'B';
    fake[1] = 'A';
    fake[2] = 'D';
    fake[3] = '!';
    auto dds = DDSConverter::ReadDDSBuffer(fake, sizeof(fake));
    REQUIRE_FALSE(dds.valid());
}

TEST_CASE("DDSConverter: WriteDDS rejects empty DDSFile", "[Graphics][DDS]")
{
    DDSFile empty;
    auto buf = DDSConverter::WriteDDSBuffer(empty);
    REQUIRE(buf.empty());
}

// --- Fixture generation: create DDS fixture from PAA ---

TEST_CASE("DDSConverter: generate DDS fixture from synthetic DXT1", "[Graphics][DDS][.fixture]")
{
    auto dds = DDSConverter::PAAFileToDDS(GET_FIXTURE("texture/paa/synthetic_dxt1.paa"));
    REQUIRE(dds.valid());
    REQUIRE(dds.format == PixelFormat::DXT1);
    REQUIRE(dds.width == 64);
    REQUIRE(dds.height == 64);

    TmpFile tmp("synthetic_dxt1.dds");
    REQUIRE(DDSConverter::WriteDDS(tmp.path, dds));

    auto dds2 = DDSConverter::ReadDDS(tmp.path);
    REQUIRE(dds2.valid());
    REQUIRE(dds2.format == PixelFormat::DXT1);
    REQUIRE(dds2.width == 64);
    REQUIRE(dds2.height == 64);
    REQUIRE(dds2.mipmaps[0].data == dds.mipmaps[0].data);
}

TEST_CASE("DDSConverter: synthetic DXT5 dimensions and blocks survive roundtrip", "[Graphics][DDS]")
{
    // The source is 32x32 DXT5, not the 64x64 DXT1 fixture above. This writes only
    // a temporary file, so it belongs in normal tests, not the hidden generators.
    PAAInfo info;
    REQUIRE(ReadPAAInfo(GET_FIXTURE("paa/synthetic_dxt5.paa"), info));
    REQUIRE(info.width == 32);
    REQUIRE(info.height == 32);
    auto dds = DDSConverter::PAAFileToDDS(GET_FIXTURE("paa/synthetic_dxt5.paa"));
    REQUIRE(dds.valid());
    REQUIRE(dds.width == info.width);
    REQUIRE(dds.height == info.height);
    REQUIRE(dds.mipmaps[0].data.size() == 1024);

    TmpFile tmp("synthetic_dxt5.dds");
    REQUIRE(DDSConverter::WriteDDS(tmp.path, dds));

    auto dds2 = DDSConverter::ReadDDS(tmp.path);
    REQUIRE(dds2.valid());
    REQUIRE(dds2.format == PixelFormat::DXT5);
    REQUIRE(dds2.width == info.width);
    REQUIRE(dds2.height == info.height);
    REQUIRE(dds2.mipmaps.size() == dds.mipmaps.size());
    for (size_t i = 0; i < dds.mipmaps.size(); ++i)
    {
        REQUIRE(dds2.mipmaps[i].width == dds.mipmaps[i].width);
        REQUIRE(dds2.mipmaps[i].height == dds.mipmaps[i].height);
        REQUIRE(dds2.mipmaps[i].data == dds.mipmaps[i].data);
    }
}
