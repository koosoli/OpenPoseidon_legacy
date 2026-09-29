#include <catch2/catch_test_macros.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/Asset/Formats/P3D/ODOLLoader.hpp>
#include <Poseidon/Foundation/Algorithms/Sha256.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace Poseidon;

namespace
{
std::vector<char> Compress(const std::vector<char>& source)
{
    QOStream stream;
    SSCompress codec;
    codec.Encode(stream, source.data(), static_cast<long>(source.size()));
    return {stream.str(), stream.str() + stream.pcount()};
}

struct Archive
{
    std::filesystem::path directory;
    std::filesystem::path file;
    QFBank bank;

    Archive(const char* name, const std::vector<char>& encoded, int codec, int decodedBytes)
    {
        Foundation::CaptureMainThread();
        directory = std::filesystem::temp_directory_path() /
            ("compressed-model-lease-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + name);
        REQUIRE(std::filesystem::create_directory(directory));
        file = directory / "models.pbo";
        {
            std::ofstream out(file, std::ios::binary);
            REQUIRE(out.good());
            const char member[] = "model.p3d";
            out.write(member, sizeof(member));
            for (int field : {codec, decodedBytes, 0, 0, static_cast<int>(encoded.size())})
                out.write(reinterpret_cast<const char*>(&field), sizeof(field));
            const char end = 0;
            out.write(&end, 1);
            const int zero = 0;
            for (int i = 0; i < 5; ++i) out.write(reinterpret_cast<const char*>(&zero), sizeof(zero));
            out.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
            REQUIRE(out.good());
        }
        REQUIRE(bank.open(RString((directory / "models").string().c_str())));
        bank.Lock();
    }
    ~Archive()
    {
        if (bank.IsLocked()) bank.Unlock();
        bank.close();
        std::error_code error;
        std::filesystem::remove(file, error);
        error.clear();
        std::filesystem::remove(directory, error);
    }
};
}

TEST_CASE("Compressed model member lease decodes one exact mounted source", "[archive][model][compressed-lease]")
{
    const std::vector<char> original{'O','D','O','L',7,0,0,0,'m','o','d','e','l'};
    const auto encoded = Compress(original);
    Archive first("first", encoded, CompMagic, static_cast<int>(original.size()));
    auto source = first.bank.CaptureCompressedReadRequest("MODEL.P3D");
#ifdef _WIN32
    REQUIRE(source);
    CHECK(source->CanonicalMember() == "model.p3d");
    CHECK(source->Codec() == CompMagic);
    CHECK(source->DecodedBytes() == original.size());
    CHECK(source->Encoded().HasArchiveIdentity());
    CHECK(first.bank.MatchesMountedCompressedMember("model.p3d", *source));
    CHECK(first.bank.MatchesMountedCompressedMember("MODEL.P3D", *source));
    CHECK_FALSE(first.bank.CaptureReadRequest("model.p3d", true));
    std::vector<char> decoded;
    std::string hash;
    REQUIRE(source->ReadDecoded(decoded, hash));
    CHECK(decoded == original);
    CHECK(hash == Foundation::Sha256::Of(original.data(), original.size()));

    auto different = original;
    different.back() ^= 1;
    Archive second("second", Compress(different), CompMagic, static_cast<int>(different.size()));
    CHECK_FALSE(second.bank.MatchesMountedCompressedMember("model.p3d", *source));
#else
    CHECK_FALSE(source); // Physical member leasing is deliberately Windows-only.
#endif
}

TEST_CASE("Compressed model member lease refuses malformed streams and unsupported metadata", "[archive][model][compressed-lease]")
{
    const std::vector<char> original(256, 'Q');
    const auto encoded = Compress(original);
    Archive valid("caps", encoded, CompMagic, static_cast<int>(original.size()));
    CHECK_FALSE(valid.bank.CaptureCompressedReadRequest("model.p3d", static_cast<uint32_t>(encoded.size() - 1)));
    CHECK_FALSE(valid.bank.CaptureCompressedReadRequest("model.p3d", BankCompressedReadRequest::MaxEncodedBytes, 255));
    CHECK_FALSE(valid.bank.CaptureCompressedReadRequest(std::string(128, 'a').c_str()));
    Archive raw("raw", original, 0, static_cast<int>(original.size()));
    CHECK_FALSE(raw.bank.CaptureCompressedReadRequest("model.p3d"));
    Archive encrypted("encrypted", encoded, EncrMagic, static_cast<int>(original.size()));
    CHECK_FALSE(encrypted.bank.CaptureCompressedReadRequest("model.p3d"));
    Archive oversized("oversized", encoded, CompMagic, BankCompressedReadRequest::MaxDecodedBytes + 1);
    CHECK_FALSE(oversized.bank.CaptureCompressedReadRequest("model.p3d"));

#ifdef _WIN32
    auto checksumBad = encoded;
    checksumBad.back() ^= 1;
    Archive checksum("checksum", checksumBad, CompMagic, static_cast<int>(original.size()));
    auto checksumSource = checksum.bank.CaptureCompressedReadRequest("model.p3d");
    REQUIRE(checksumSource);
    std::vector<char> untouched{'x'};
    std::string oldHash = "old";
    CHECK_FALSE(checksumSource->ReadDecoded(untouched, oldHash));
    CHECK(untouched == std::vector<char>{'x'});
    CHECK(oldHash == "old");

    auto trailing = encoded;
    trailing.push_back('!');
    Archive extra("trailing", trailing, CompMagic, static_cast<int>(original.size()));
    auto extraSource = extra.bank.CaptureCompressedReadRequest("model.p3d");
    REQUIRE(extraSource);
    CHECK_FALSE(extraSource->ReadDecoded(untouched, oldHash));
    CHECK(untouched == std::vector<char>{'x'});
    CHECK(oldHash == "old");
#endif
}

TEST_CASE("Retail data3d compressed model lease matches decoded ODOL7", "[.][archive][model][compressed-lease][retail]")
{
    // Opt-in because retail DTA is external to the source tree. Once supplied,
    // absence or mismatch is a test failure, never a skipped retail assertion.
    const char* configured = std::getenv("WGR_TEST_COMPRESSED_MODEL_PBO");
    if (!configured) return;
    const std::filesystem::path archive(configured);
    REQUIRE(std::filesystem::is_regular_file(archive));

    Foundation::CaptureMainThread();
    QFBank bank;
    auto stem = archive;
    stem.replace_extension();
    REQUIRE(bank.open(RString(stem.string().c_str())));
    bank.Lock();
    struct CloseBank
    {
        QFBank& bank;
        ~CloseBank() { if (bank.IsLocked()) bank.Unlock(); bank.close(); }
    } close{bank};

    auto source = bank.CaptureCompressedReadRequest("skala_new.p3d");
    REQUIRE(source);
    CHECK_FALSE(bank.CaptureReadRequest("skala_new.p3d", true));
    CHECK(source->Codec() == CompMagic);
    CHECK(source->CanonicalMember() == "skala_new.p3d");
    CHECK(source->Encoded().offset == 19310756);
    CHECK(source->Encoded().bytes == 33097);
    CHECK(source->DecodedBytes() == 53786);
    BankReadMemberIdentity identity;
    REQUIRE(source->Encoded().CopyMemberIdentity(identity));
    CHECK(identity.archiveBytes == 28029598);
    REQUIRE(bank.MatchesMountedCompressedMember("skala_new.p3d", *source));

    std::vector<char> decoded;
    std::string hash;
    REQUIRE(source->ReadDecoded(decoded, hash));
    CHECK(decoded.size() == 53786);
    CHECK(hash == "1048fad6987e65533c17146ab8b9d4ef70cd475e3ed88b2f7c76670504ecb78b");
    CHECK(hash == Foundation::Sha256::Of(decoded.data(), decoded.size()));
    const auto model = Asset::Formats::ODOLLoader::loadFromBuffer(
        decoded.data(), static_cast<int>(decoded.size()), "data3d/skala_new.p3d");
    CHECK(model.sourceFormat == "ODOL");
    CHECK(model.sourceVersion == 7);
    CHECK(model.lodLevels.size() == 7);
}
