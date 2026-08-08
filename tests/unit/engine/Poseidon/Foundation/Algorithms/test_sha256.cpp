// test_sha256.cpp - AST-020: SHA-256 for derived-asset identity.
//
// Checked against the FIPS 180-4 published vectors. A transcription error in a
// hash produces stable-looking garbage that only shows up later as a cache serving
// the wrong asset, so the vectors are the whole point of this file.

#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Foundation/Algorithms/Sha256.hpp>
#include <string>

using Poseidon::Foundation::Sha256;

TEST_CASE("SHA-256 matches the published vectors", "[foundation][sha256][ast-020]")
{
    REQUIRE(Sha256::Of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    REQUIRE(Sha256::Of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    REQUIRE(Sha256::Of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("SHA-256 is incremental and order-sensitive", "[foundation][sha256][ast-020]")
{
    Sha256 split;
    split.Update("ab");
    split.Update("c");
    REQUIRE(split.Hex() == Sha256::Of("abc"));

    // Hex() must not destroy the state, or incremental use silently breaks.
    REQUIRE(split.Hex() == Sha256::Of("abc"));

    REQUIRE(Sha256::Of("ab") != Sha256::Of("ba"));
}

TEST_CASE("SHA-256 spans the block boundary correctly", "[foundation][sha256][ast-020]")
{
    // 55, 56 and 64 bytes are where padding and length encoding change behaviour.
    for (size_t length : {size_t(55), size_t(56), size_t(64), size_t(119), size_t(120)})
    {
        const std::string message(length, 'a');
        Sha256            incremental;
        for (char ch : message)
            incremental.Update(&ch, 1);
        REQUIRE(incremental.Hex() == Sha256::Of(message));
    }
    REQUIRE(Sha256::Of(std::string(64, 'a')) ==
            "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}
