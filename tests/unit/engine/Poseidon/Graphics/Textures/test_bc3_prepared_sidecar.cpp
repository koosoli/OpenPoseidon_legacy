#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Textures/Bc3Encoder.hpp>
#include <Poseidon/Graphics/Textures/DdsImport.hpp>
#include <Poseidon/Graphics/Textures/PreparedTextures.hpp>
#include <Poseidon/Foundation/Platform/FpEnvironment.hpp>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>
#include <thread>

// stb_dxt's public C ABI, supplied once by Bc3Encoder.cpp. The test target does
// not inherit Poseidon's private Stb include directory.
extern "C" void stb_compress_dxt_block(unsigned char*, const unsigned char*, int, int);

namespace
{
// Historical TextureWgpu encoder, frozen independently of the extracted helper.
// This pins RGB/alpha block bytes, box rounding, edge replication and mip stopping.
Poseidon::Bc3MipChain LegacyEncode(const std::vector<uint8_t>& rgba, int width, int height)
{
    Poseidon::Bc3MipChain result;
    result.width = width; result.height = height;
    std::vector<uint8_t> level = rgba;
    int w = width, h = height;
    uint8_t block[64], out[16];
    while (true)
    {
        result.offsets.push_back(static_cast<uint32_t>(result.blocks.size()));
        for (int by = 0; by < (h + 3) / 4; ++by)
            for (int bx = 0; bx < (w + 3) / 4; ++bx)
            {
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x)
                    {
                        const int sx = std::min(bx * 4 + x, w - 1), sy = std::min(by * 4 + y, h - 1);
                        std::memcpy(block + (y * 4 + x) * 4,
                                    level.data() + (static_cast<size_t>(sy) * w + sx) * 4, 4);
                    }
                stb_compress_dxt_block(out, block, 1, 0); // STB_DXT_NORMAL
                result.blocks.insert(result.blocks.end(), out, out + 16);
            }
        ++result.levels;
        if (w <= 4 && h <= 4) break;
        const int nw = std::max(w / 2, 1), nh = std::max(h / 2, 1);
        std::vector<uint8_t> next(static_cast<size_t>(nw) * nh * 4);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
                for (int c = 0; c < 4; ++c)
                {
                    const int x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
                    const int y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
                    const int sum = level[(static_cast<size_t>(y0) * w + x0) * 4 + c] +
                                    level[(static_cast<size_t>(y0) * w + x1) * 4 + c] +
                                    level[(static_cast<size_t>(y1) * w + x0) * 4 + c] +
                                    level[(static_cast<size_t>(y1) * w + x1) * 4 + c];
                    next[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
                }
        level.swap(next); w = nw; h = nh;
    }
    return result;
}
void Word(std::vector<uint8_t>& bytes, size_t offset, uint32_t word)
{ std::memcpy(bytes.data() + offset, &word, sizeof(word)); }
std::vector<uint8_t> Edds(int width, int height, bool coverage, int mips = 3)
{
    std::vector<uint8_t> bytes(128, 0);
    std::memcpy(bytes.data(), "DDS ", 4);
    Word(bytes, 4, 124); Word(bytes, 8, 0x20000);
    Word(bytes, 12, height); Word(bytes, 16, width); Word(bytes, 28, mips);
    std::memcpy(bytes.data() + 36, "ENF1", 4);
    Word(bytes, 76, 32); Word(bytes, 80, 0x41); Word(bytes, 88, 32);
    Word(bytes, 92, 0xff0000); Word(bytes, 96, 0xff00);
    Word(bytes, 100, 0xff); Word(bytes, 104, 0xff000000);
    std::vector<std::vector<uint8_t>> levels;
    for (int mip = 0; mip < mips; ++mip)
    {
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
        for (size_t i = 0; i < pixels.size() / 4; ++i)
        {
            pixels[i * 4] = coverage ? static_cast<uint8_t>(i % 3 ? 255 : 0) : static_cast<uint8_t>(i * 13 + mip * 47);
            pixels[i * 4 + 1] = static_cast<uint8_t>(i * 7 + mip * 11);
            pixels[i * 4 + 2] = static_cast<uint8_t>(i * 23 + mip * 19);
            pixels[i * 4 + 3] = static_cast<uint8_t>(i % 2 ? 255 : 127);
        }
        levels.push_back(std::move(pixels));
        width = std::max(width / 2, 1); height = std::max(height / 2, 1);
    }
    for (auto i = levels.rbegin(); i != levels.rend(); ++i)
    {
        const size_t at = bytes.size(); bytes.resize(at + 8);
        Word(bytes, at, 0x59504f43); Word(bytes, at + 4, static_cast<uint32_t>(i->size()));
    }
    for (auto i = levels.rbegin(); i != levels.rend(); ++i) bytes.insert(bytes.end(), i->begin(), i->end());
    return bytes;
}
bool InitComposite(Poseidon::TextureSourceDDS& source, Poseidon::PacLevelMem* mips,
                   int width = 16, int height = 16, const char* name = "enfa|coverage.edds|colour.edds")
{
    return source.InitFromReader(name, mips, 16,
        [&](const char* key, std::vector<uint8_t>& out) {
            out = Edds(width, height, std::strcmp(key, "coverage.edds") == 0,
                       width >= 16 && height >= 16 ? 3 : 1);
            return true;
        }, {true, true, 4096});
}
std::vector<uint8_t> Pixels(Poseidon::TextureSourceDDS& source, Poseidon::PacLevelMem mip, int level = 0)
{
    mip._dFormat = Poseidon::PacARGB8888; mip._pitch = mip._w * 4;
    std::vector<uint8_t> pixels(static_cast<size_t>(mip._w) * mip._h * 4);
    if (!source.GetMipmapData(pixels.data(), mip, level)) return {};
    return pixels;
}
}

TEST_CASE("Extracted BC3 encoder preserves every legacy block and generated mip", "[graphics][bc3-worker][parity]")
{
    for (const auto [w, h] : {std::pair{4,4}, std::pair{5,7}, std::pair{17,5}, std::pair{4,32}, std::pair{33,9}})
        for (int alphaKind = 0; alphaKind < 3; ++alphaKind)
        {
            std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
            for (size_t i = 0; i < rgba.size() / 4; ++i)
            {
                rgba[i*4] = static_cast<uint8_t>(i*13); rgba[i*4+1] = static_cast<uint8_t>(i*7);
                rgba[i*4+2] = static_cast<uint8_t>(i*29);
                rgba[i*4+3] = alphaKind == 0 ? 255 : alphaKind == 1 ? (i % 3 ? 255 : 0) : static_cast<uint8_t>(i*17);
            }
            const auto expected = LegacyEncode(rgba, w, h);
            Poseidon::Bc3MipChain actual;
            REQUIRE(Poseidon::EncodeBc3ChainRGBA(rgba.data(), w, h, actual.blocks, actual.offsets, actual.levels));
            CHECK(actual.blocks == expected.blocks); CHECK(actual.offsets == expected.offsets);
            CHECK(actual.levels == expected.levels);
            Poseidon::Bc3EncodingFootprint footprint;
            REQUIRE(Poseidon::MeasureBc3EncodingFootprint(w, h, footprint));
            CHECK(footprint.blockBytes == actual.blocks.size()); CHECK(footprint.levels == actual.levels);
            CHECK(footprint.scratchBytes >= rgba.capacity() * 2 + actual.RetainedBytes());
        }
    Poseidon::Bc3EncodingFootprint footprint;
    CHECK_FALSE(Poseidon::MeasureBc3EncodingFootprint(3, 8, footprint));
    CHECK_FALSE(Poseidon::MeasureBc3EncodingFootprint(-1, 8, footprint));
    CHECK_FALSE(Poseidon::MeasureBc3EncodingFootprint(std::numeric_limits<int>::max(), 8, footprint));
    std::vector<uint8_t> blocks; std::vector<uint32_t> offsets; int levels = 0;
    CHECK_FALSE(Poseidon::EncodeBc3ChainRGBA(nullptr, 4, 4, blocks, offsets, levels));
}

TEST_CASE("Concurrent BC3 workers match the legacy owner encoder", "[graphics][bc3-worker][parity]")
{
    std::vector<uint8_t> rgba(64 * 32 * 4);
    for (size_t i = 0; i < rgba.size(); ++i) rgba[i] = static_cast<uint8_t>(i * 37 + i / 7);
    const auto expected = LegacyEncode(rgba, 64, 32);
    std::vector<Poseidon::Bc3MipChain> results(8);
    std::vector<std::thread> workers;
    for (size_t i = 0; i < results.size(); ++i)
        workers.emplace_back([&, i] {
            Poseidon::Foundation::ApplyMainFpEnvironment();
            auto& result = results[i];
            Poseidon::EncodeBc3ChainRGBA(rgba.data(), 64, 32, result.blocks, result.offsets, result.levels);
        });
    for (auto& worker : workers) worker.join();
    for (const auto& result : results)
    {
        CHECK(result.blocks == expected.blocks); CHECK(result.offsets == expected.offsets);
        CHECK(result.levels == expected.levels);
    }
}

TEST_CASE("Prepared composite sidecar preserves original pixels metadata and exact owner upload chain", "[graphics][bc3-worker][edds]")
{
    Poseidon::TextureSourceDDS source;
    Poseidon::PacLevelMem mips[16];
    REQUIRE(InitComposite(source, mips));
    const auto beforePixels = Pixels(source, mips[0]);
    REQUIRE_FALSE(beforePixels.empty());
    REQUIRE(source.GetMipmapCount() == 3);
    const auto beforeLower = Pixels(source, mips[1], 1);
    const auto beforeLast = Pixels(source, mips[2], 2);
    REQUIRE_FALSE(beforeLower.empty()); REQUIRE_FALSE(beforeLast.empty());
    const auto beforeHeader = mips[0];
    const auto beforeAverage = source.GetAverageColor();
    const auto beforeAlpha = source.IsAlpha(), beforeTransparent = source.IsTransparent();
    const auto beforeFormat = source.GetFormat(); const auto beforeMips = source.GetMipmapCount();
    const size_t beforeBytes = source.PreparedByteSize(), required = source.CompositeBc3Reservation();
    REQUIRE(required > beforeBytes);
    CHECK_FALSE(source.PrepareCompositeBc3(required - 1));
    CHECK(source.PreparedByteSize() == beforeBytes);
    REQUIRE(source.PrepareCompositeBc3(required));
    CHECK(Pixels(source, mips[0]) == beforePixels);
    CHECK(Pixels(source, mips[1], 1) == beforeLower);
    CHECK(Pixels(source, mips[2], 2) == beforeLast);
    CHECK(mips[0]._w == beforeHeader._w); CHECK(mips[0]._h == beforeHeader._h);
    CHECK(mips[0]._sFormat == beforeHeader._sFormat);
    CHECK(source.GetAverageColor() == beforeAverage); CHECK(source.IsAlpha() == beforeAlpha);
    CHECK(source.IsTransparent() == beforeTransparent); CHECK(source.GetFormat() == beforeFormat);
    CHECK(source.GetMipmapCount() == beforeMips);
    auto rgba = beforePixels;
    for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i+2]);
    const auto expected = LegacyEncode(rgba, mips[0]._w, mips[0]._h);
    Poseidon::Bc3MipChain prepared;
    CHECK_FALSE(source.TakeCompositeBc3("enfa|wrong.edds|colour.edds", 16, 16, prepared));
    CHECK_FALSE(source.TakeCompositeBc3("enfa|coverage.edds|colour.edds", 8, 16, prepared));
    const size_t withSidecar = source.PreparedByteSize();
    REQUIRE(source.TakeCompositeBc3("enfa|coverage.edds|colour.edds", 16, 16, prepared));
    CHECK(prepared.blocks == expected.blocks); CHECK(prepared.offsets == expected.offsets);
    CHECK(prepared.levels == expected.levels);
    CHECK(withSidecar - source.PreparedByteSize() == prepared.RetainedBytes());
    CHECK_FALSE(source.TakeCompositeBc3("enfa|coverage.edds|colour.edds", 16, 16, prepared));
    CHECK(Pixels(source, mips[0]) == beforePixels); // still available for failed BC3 GPU upload
    REQUIRE(source.PrepareCompositeBc3(source.CompositeBc3Reservation()));
    REQUIRE(InitComposite(source, mips, 16, 16, "colour.edds")); // reinitialization retires sidecar
    CHECK_FALSE(source.TakeCompositeBc3("enfa|coverage.edds|colour.edds", 16, 16, prepared));
    CHECK(source.CompositeBc3Reservation() == 0);
}

TEST_CASE("Worker sidecar excludes oversized and nested tinted sources without changing their data", "[graphics][bc3-worker][edds]")
{
    for (const auto [width, height] : {std::pair{2,2}, std::pair{2049,4}})
    {
        Poseidon::TextureSourceDDS source; Poseidon::PacLevelMem mips[16];
        REQUIRE(InitComposite(source, mips, width, height));
        const auto before = Pixels(source, mips[0]);
        CHECK(source.CompositeBc3Reservation() == 0);
        CHECK_FALSE(source.PrepareCompositeBc3(SIZE_MAX));
        CHECK(Pixels(source, mips[0]) == before);
    }
    Poseidon::TextureSourceDDS source; Poseidon::PacLevelMem mips[16];
    REQUIRE(InitComposite(source, mips, 16, 16, "enft|1,1,1|enfa|coverage.edds|colour.edds"));
    CHECK(source.CompositeBc3Reservation() == 0);
}

TEST_CASE("Prepared DDS store charges sidecar capacities and rejects stale or incompatible results", "[graphics][bc3-worker][streaming]")
{
    auto& store = Poseidon::render::PreparedTextureStore::Instance();
    if (!store.Enabled()) SKIP("Requires texture preparation store");
    store.Clear();
    const std::string name = "enfa|coverage.edds|colour.edds";
    const Poseidon::DdsPreparationOptions options{true, true, 4096};
    auto make = [&] {
        auto source = std::make_unique<Poseidon::TextureSourceDDS>(); Poseidon::PacLevelMem mips[16];
        if (!InitComposite(*source, mips) || !source->PrepareCompositeBc3(source->CompositeBc3Reservation()))
            return std::unique_ptr<Poseidon::TextureSourceDDS>{};
        return source;
    };
    auto source = make(); REQUIRE(source);
    const size_t bytes = source->PreparedByteSize();
    const auto generation = store.Generation();
    REQUIRE(store.PutDdsPrepared(name, std::move(source), options, generation));
    CHECK(store.SnapshotStats().bytes == bytes);
    auto taken = store.TakeDdsPrepared(name, options); REQUIRE(taken);
    CHECK(store.SnapshotStats().bytes == 0);
    Poseidon::Bc3MipChain sidecar;
    REQUIRE(taken->TakeCompositeBc3(name.c_str(), 16, 16, sidecar));
    store.Clear();
    source = make(); REQUIRE(source);
    CHECK_FALSE(store.PutDdsPrepared(name, std::move(source), options, generation));
    CHECK(store.SnapshotStats().bytes == 0);
    source = make(); REQUIRE(source);
    REQUIRE(store.PutDdsPrepared(name, std::move(source), options, store.Generation()));
    auto different = options; different.decodedMaxEdge = 1024;
    CHECK_FALSE(store.TakeDdsPrepared(name, different));
    CHECK(store.SnapshotStats().bytes == 0);
    store.Clear();
}

TEST_CASE("DDS factory adopts exact consumer identity for a normalized prepared BC3 source", "[graphics][bc3-worker][dds-factory]")
{
    // Run this tag in a fresh process with WGR_NATIVE_DDS_PREPARE=1 and
    // WGR_NATIVE_DDS_PREPARE_VERIFY=0. NativeDdsEnabled is cached process-wide.
    auto& store = Poseidon::render::PreparedTextureStore::Instance();
    if (!store.NativeDdsEnabled()) SKIP("Requires fresh process with WGR_NATIVE_DDS_PREPARE=1");
    if (const char* verify = std::getenv("WGR_NATIVE_DDS_PREPARE_VERIFY"); verify && std::strcmp(verify, "1") == 0)
        SKIP("Synthetic prepared-source test requires verification disabled");
    store.Clear();
    const char* consumer = "enfa|Coverage.edds|Colour.edds";
    const std::string workerKey = store.Key(consumer);
    const auto options = Poseidon::CaptureDdsPreparationOptions();
    auto source = std::make_unique<Poseidon::TextureSourceDDS>();
    Poseidon::PacLevelMem preparedMips[16], consumedMips[16];
    REQUIRE(source->InitFromReader(workerKey.c_str(), preparedMips, 16,
        [](const char* key, std::vector<uint8_t>& out) {
            out = Edds(16, 16, std::strcmp(key, "coverage.edds") == 0);
            return true;
        }, options));
    const auto beforePixels = Pixels(*source, preparedMips[0]);
    REQUIRE(source->PrepareCompositeBc3(source->CompositeBc3Reservation()));
    auto* identity = source.get();
    REQUIRE(store.PutDdsPrepared(workerKey, std::move(source), options, store.Generation()));
    Poseidon::TextureSourceDDSFactory factory;
    std::unique_ptr<Poseidon::ITextureSource> consumed(factory.Create(consumer, consumedMips, 16));
    REQUIRE(consumed.get() == identity);
    auto* preparedSource = dynamic_cast<Poseidon::TextureSourceDDS*>(consumed.get());
    REQUIRE(preparedSource);
    CHECK(Pixels(*preparedSource, consumedMips[0]) == beforePixels);
    Poseidon::Bc3MipChain sidecar;
    CHECK_FALSE(preparedSource->TakeCompositeBc3(workerKey.c_str(), 16, 16, sidecar));
    REQUIRE(preparedSource->TakeCompositeBc3(consumer, 16, 16, sidecar));
    auto rgba = beforePixels;
    for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i+2]);
    CHECK(sidecar.blocks == LegacyEncode(rgba, 16, 16).blocks);
    CHECK(store.SnapshotStats().bytes == 0);
    store.Clear();
}
