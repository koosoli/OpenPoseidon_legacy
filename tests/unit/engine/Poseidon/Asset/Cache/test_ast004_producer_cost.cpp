#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Foundation/Algorithms/Sha256.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/World/Model/ModelCache.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// AST-004 -- the measurement that CHOOSES the producer.
//
// AST-002 built a store and AST-003 built the key's content-hash half; neither
// picked a customer. This is the "measure before choosing" pass: it runs the
// four candidate derivations over real retail PBO members and prints, per
// candidate, the derivation cost and the size of what a cache would have to
// store. The point of measuring the SHA-256 of the member alongside them is
// that the hash is the cache's own floor -- a derivation cheaper than its own
// key is a cache that loses.
//
// Hidden: needs a real archive. Run with
//   PoseidonCoreTests.exe "[ast004cost]" and AST004_COST_PBO pointing at an
// archive without the .pbo extension, e.g.
//   ".../ARMA Cold War Assault/AddOns/O".

using namespace Poseidon;
namespace fs = std::filesystem;

namespace
{

using Clock = std::chrono::steady_clock;
double Ms(Clock::duration d)
{
    return std::chrono::duration<double, std::milli>(d).count();
}

bool EndsWith(const std::string& s, const char* suffix)
{
    const std::string t(suffix);
    return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

std::string Lower(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return s;
}

struct Acc
{
    int    n     = 0;
    double ms    = 0;
    uint64_t in  = 0; // input bytes consumed
    uint64_t out = 0; // bytes a cache would have to store
};

void Report(const char* what, const Acc& a)
{
    std::printf("[ast004cost] %-28s n=%-6d total=%9.1f ms  per-item=%8.4f ms  in=%8.2f MiB  "
                "cacheable-out=%10llu B\n",
                what, a.n, a.ms, a.n ? a.ms / a.n : 0.0, a.in / 1048576.0, (unsigned long long)a.out);
    std::fflush(stdout);
}

} // namespace

TEST_CASE("AST-004 candidate producer derivation costs", "[.][ast004cost]")
{
    const char* env = std::getenv("AST004_COST_PBO");
    if (!env || !*env)
    {
        WARN("AST004_COST_PBO not set; skipping");
        return;
    }

    QFBank bank;
    REQUIRE(bank.open(RString(env)));
    bank.Lock();

    std::vector<std::string> names;
    struct Collect
    {
        std::vector<std::string>* names;
    } ctx{&names};
    bank.ForEach([](const FileInfoO& fi, const FileBankType*, void* c)
                 { static_cast<Collect*>(c)->names->push_back((const char*)fi.name); }, &ctx);

    // Warm the mapping first: every candidate below must be measured against the
    // same page-cache state, or the first one measured pays for all of them.
    for (const std::string& n : names)
    {
        Ref<IFileBuffer> b = bank.Read(n.c_str());
        if (b && b->GetData() && b->GetSize() > 0)
            (void)static_cast<unsigned char>(b->GetData()[b->GetSize() - 1]);
    }

    Acc hash, chain, decode, classify, model;

    for (const std::string& raw : names)
    {
        const std::string name = Lower(raw);
        const bool isTexture   = EndsWith(name, ".paa") || EndsWith(name, ".pac");
        const bool isModel     = EndsWith(name, ".p3d");
        if (!isTexture && !isModel)
            continue;

        Ref<IFileBuffer> buf = bank.Read(raw.c_str());
        if (!buf || !buf->GetData() || buf->GetSize() <= 0)
            continue;
        const void*  data = buf->GetData();
        const size_t size = static_cast<size_t>(buf->GetSize());

        // (0) The cache's own floor: hashing the member to build the key.
        {
            const auto t0 = Clock::now();
            const std::string h = Foundation::Sha256::Of(data, size);
            hash.ms += Ms(Clock::now() - t0);
            hash.n++;
            hash.in += size;
            hash.out += h.size();
        }

        if (isTexture)
        {
            const bool isPaa = EndsWith(name, ".paa");

            // (A) GPU-ready block chain -- the streaming preparer's stage 2.
            {
                PAABlockChain c;
                const auto    t0 = Clock::now();
                const bool    ok = ReadPAABlockChainBuffer(data, size, c);
                chain.ms += Ms(Clock::now() - t0);
                if (ok)
                {
                    chain.n++;
                    chain.in += size;
                    chain.out += c.blocks.size();
                }
            }

            // (B) Whole-file RGBA8 decode -- "texture preparation".
            DecodedImage img;
            {
                const auto t0 = Clock::now();
                img              = DecodePAABuffer(data, size, isPaa);
                decode.ms += Ms(Clock::now() - t0);
                if (img.valid())
                {
                    decode.n++;
                    decode.in += size;
                    decode.out += img.rgba.size();
                }
            }

            // (C) Alpha classification -- decode + histogram, output ~48 bytes.
            //     Measured as decode+classify because that is what the caller
            //     pays; the decode is not otherwise wanted at this seam.
            if (img.valid())
            {
                const auto t0 = Clock::now();
                DecodedImage again = DecodePAABuffer(data, size, isPaa);
                AlphaStats   st    = ClassifyAlpha(again.rgba.data(),
                                             static_cast<size_t>(again.width) * static_cast<size_t>(again.height));
                classify.ms += Ms(Clock::now() - t0);
                classify.n++;
                classify.in += size;
                classify.out += sizeof(AlphaStats);
                (void)st;
            }
        }

        if (isModel)
        {
            // ModelCache::LoadLooseFile is the pure worker-safe P3D -> IR path, and
            // it only reads loose files, so the member is spilled to a temp first.
            // The spill is OUTSIDE the timer.
            const fs::path tmp = fs::temp_directory_path() / "ast004-model.p3d";
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
            }
            std::string error;
            bool        opened = false;
            const auto  t0     = Clock::now();
            auto        m      = ModelCache::LoadLooseFile(tmp.string(), &error, &opened);
            model.ms += Ms(Clock::now() - t0);
            if (m)
            {
                model.n++;
                model.in += size;
            }
            std::error_code ec;
            fs::remove(tmp, ec);
        }
    }

    std::printf("[ast004cost] archive=%s members=%zu\n", env, names.size());
    Report("sha256 of member (key)", hash);
    Report("A: PAA block chain", chain);
    Report("B: PAA full RGBA decode", decode);
    Report("C: alpha classification", classify);
    Report("D: P3D -> Model IR", model);
    bank.Unlock();
    SUCCEED();
}
