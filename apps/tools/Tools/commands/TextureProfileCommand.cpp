#include "TextureProfileCommand.hpp"

#include <Poseidon/Graphics/Textures/Image.hpp>
#include <Poseidon/Graphics/Textures/PAAEncoder.hpp>
#include <Poseidon/Graphics/Textures/PixelFormat.hpp>
#include <Poseidon/IO/PackFiles.hpp>

#include <CLI/App.hpp>
#include <CLI/Option.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace PoseidonTools
{
namespace
{
namespace fs = std::filesystem;

constexpr const char* DefaultGameDir = R"(D:\SteamLibrary\steamapps\common\ARMA Cold War Assault)";
constexpr const char* ProfilesDirName = "TextureProfiles";

fs::path ProfilesRoot(const std::string& gameDir)
{
    return fs::path(gameDir) / ProfilesDirName;
}

bool IsSafeProfileName(const std::string& name)
{
    return !name.empty() && std::all_of(name.begin(), name.end(),
                                        [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; });
}

fs::path ProfileDir(const std::string& gameDir, const std::string& name)
{
    return ProfilesRoot(gameDir) / ("@" + name);
}

fs::path SourceDir(const std::string& gameDir, const std::string& name)
{
    return ProfileDir(gameDir, name) / "source";
}

bool NormalizedTarget(const std::string& input, fs::path& target)
{
    fs::path raw(input);
    if (raw.empty() || raw.is_absolute())
        return false;
    target = raw.lexically_normal();
    for (const fs::path& component : target)
    {
        if (component == "..")
            return false;
    }
    return target != ".";
}

bool WritePaaFromImage(const fs::path& input, const fs::path& output)
{
    Poseidon::Image image = Poseidon::Image::FromFile(input.string());
    if (!image.valid())
    {
        std::cerr << "Cannot read texture: " << input << "\n";
        return false;
    }
    std::error_code ec;
    fs::create_directories(output.parent_path(), ec);
    if (ec)
    {
        std::cerr << "Cannot create profile folder: " << output.parent_path() << "\n";
        return false;
    }
    if (!Poseidon::PAAEncoder::WritePAA(output.string(), image, Poseidon::PixelFormat::DXT5))
    {
        std::cerr << "Cannot write PAA: " << output << "\n";
        return false;
    }
    return true;
}

uint8_t SampleLuma(const std::vector<uint8_t>& rgba, int width, int height, int x, int y)
{
    x = std::clamp(x, 0, width - 1);
    y = std::clamp(y, 0, height - 1);
    const size_t pixel = static_cast<size_t>(y * width + x) * 4;
    return static_cast<uint8_t>((rgba[pixel] * 77u + rgba[pixel + 1] * 150u + rgba[pixel + 2] * 29u) >> 8u);
}

bool SaveNormalMap(const fs::path& input, const fs::path& output, float strength)
{
    Poseidon::Image source = Poseidon::Image::FromFile(input.string()).ToRGBA();
    if (!source.valid())
    {
        std::cerr << "Cannot read texture for normal map: " << input << "\n";
        return false;
    }
    const int width = source.width();
    const int height = source.height();
    const std::vector<uint8_t>& rgba = source.data();
    std::vector<uint8_t> normal(rgba.size());
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const float left = static_cast<float>(SampleLuma(rgba, width, height, x - 1, y));
            const float right = static_cast<float>(SampleLuma(rgba, width, height, x + 1, y));
            const float above = static_cast<float>(SampleLuma(rgba, width, height, x, y - 1));
            const float below = static_cast<float>(SampleLuma(rgba, width, height, x, y + 1));
            float nx = (left - right) * strength / 255.0f;
            float ny = (above - below) * strength / 255.0f;
            float nz = 1.0f;
            const float invLength = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
            nx *= invLength;
            ny *= invLength;
            nz *= invLength;
            // NOHQ channel layout, which is what the engine decodes: X in ALPHA, Y in GREEN, Z
            // reconstructed, R unused. DXT5 is chosen for normal maps precisely because its alpha
            // block is stored separately at higher quality than the colour endpoints, so the two
            // channels that survive are A and G -- putting X in R throws it into the worst channel
            // of the format and then the renderer reads alpha and finds nothing there.
            //
            // Writing X/Y/Z to R/G/B produced a constant-255 alpha, so every enhancement authored
            // by this tool decoded with X pinned at 1.0 -- a normal leaning hard sideways, which
            // reads as no relief at all. Measured against a shipped Arma NOHQ, which has alpha
            // varying 0..255 with mean 118 while this wrote 255 everywhere.
            const size_t pixel = static_cast<size_t>(y * width + x) * 4;
            const uint8_t ex = static_cast<uint8_t>(std::clamp((nx * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
            const uint8_t ey = static_cast<uint8_t>(std::clamp((ny * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
            const uint8_t ez = static_cast<uint8_t>(std::clamp((nz * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
            // R carries HEIGHT for the parallax path. Shipped Arma NOHQ has R constant 0 in 40 of
            // 40 measured textures, so writing height there cannot disturb stock content: those
            // read 0 and displace by nothing. Height is the source luma -- the same signal the
            // gradient above is derived from, so bright reads as proud and dark as recessed, which
            // is what mortar-versus-brick already looks like in an OFP albedo.
            normal[pixel] = SampleLuma(rgba, width, height, x, y);
            normal[pixel + 1] = ey; // G: Y
            normal[pixel + 2] = ez; // B: carries Z for readability; the decode reconstructs it
            normal[pixel + 3] = ex; // A: X
        }
    }
    Poseidon::Image generated = Poseidon::Image::FromRGBA(width, height, std::move(normal));
    std::error_code ec;
    fs::create_directories(output.parent_path(), ec);
    if (ec || !Poseidon::PAAEncoder::WritePAA(output.string(), generated, Poseidon::PixelFormat::DXT5))
    {
        std::cerr << "Cannot write normal-map PAA: " << output << "\n";
        return false;
    }
    return true;
}

bool RequireProfile(const std::string& gameDir, const std::string& name)
{
    if (!IsSafeProfileName(name))
    {
        std::cerr << "Profile names may contain only letters, numbers, '-' and '_'.\n";
        return false;
    }
    if (!fs::is_directory(SourceDir(gameDir, name)))
    {
        std::cerr << "Profile does not exist: " << name << "\n";
        return false;
    }
    return true;
}

void WriteLauncher(const std::string& gameDir, const std::string& name)
{
    const fs::path launcher = ProfilesRoot(gameDir) / ("Launch-" + name + ".cmd");
    std::ofstream out(launcher, std::ios::trunc);
    out << "@echo off\r\n"
           "setlocal\r\n"
        << "pushd \"" << gameDir << "\"\r\n"
        << "start \"Arma CWA - " << name << "\" \"" << (fs::path(gameDir) / "OpenPoseidon.exe").string()
        << "\" --mods-dir \"" << ProfilesRoot(gameDir).string() << "\" --mod \"@" << name
        << "\" %*\r\n"
           "popd\r\n";
}

bool BuildProfile(const std::string& gameDir, const std::string& name)
{
    if (!RequireProfile(gameDir, name))
        return false;
    const fs::path source = SourceDir(gameDir, name);
    const fs::path addons = ProfileDir(gameDir, name) / "addons";
    const fs::path output = addons / ("zz_texture_profile_" + name + ".pbo");
    std::error_code ec;
    fs::create_directories(addons, ec);
    fs::remove(output, ec);
    FileBankManager packer;
    if (packer.Create(output.string().c_str(), source.string().c_str(), true) != LSOK)
    {
        std::cerr << "Failed to package profile: " << output << "\n";
        return false;
    }
    WriteLauncher(gameDir, name);
    std::cout << "Built profile '@" << name << "' without changing any original PBO.\n"
              << "Double-click: " << (ProfilesRoot(gameDir) / ("Launch-" + name + ".cmd")) << "\n";
    return true;
}
} // namespace

void TextureProfileCommand::Setup(CLI::App& app)
{
    auto* texture =
        app.add_subcommand("texture", "Non-destructive texture profiles, PAA conversion, and normal-map authoring");
    texture->require_subcommand(1);

    static std::string gameDir = DefaultGameDir;
    texture->add_option("--game-dir", gameDir, "Arma CWA install directory");

    auto* profile = texture->add_subcommand("profile", "Create, build, list, and launch isolated texture profiles");
    profile->require_subcommand(1);

    static std::string createName;
    auto* create = profile->add_subcommand("create", "Create an empty @profile; originals are never touched");
    create->add_option("name", createName, "Profile name")->required();
    create->callback(
        []()
        {
            if (!IsSafeProfileName(createName))
                throw CLI::ValidationError("name", "Use only letters, numbers, '-' and '_'.");
            const fs::path source = SourceDir(gameDir, createName);
            if (fs::exists(source))
            {
                std::cerr << "Profile already exists: " << createName << "\n";
                throw CLI::RuntimeError(1);
            }
            fs::create_directories(source);
            std::ofstream(source / "README.txt")
                << "Place replacement files here using their original in-game PAA paths.\n"
                   "Then run: PoseidonTools texture profile build "
                << createName << "\n";
            std::cout << "Created: " << source << "\n";
        });

    static std::string listGameDir = DefaultGameDir;
    auto* list = profile->add_subcommand("list", "List available texture profiles and whether they are built");
    list->add_option("--game-dir", listGameDir, "Arma CWA install directory");
    list->callback(
        []()
        {
            const fs::path root = ProfilesRoot(listGameDir);
            if (!fs::is_directory(root))
            {
                std::cout << "No texture profiles yet. Run: texture profile create <name>\n";
                return;
            }
            for (const fs::directory_entry& entry : fs::directory_iterator(root))
            {
                const std::string name = entry.path().filename().string();
                if (!entry.is_directory() || name.empty() || name[0] != '@')
                    continue;
                const fs::path addons = entry.path() / "addons";
                const bool built =
                    fs::is_directory(addons) &&
                    std::any_of(fs::directory_iterator(addons), fs::directory_iterator(),
                                [](const fs::directory_entry& item) { return item.path().extension() == ".pbo"; });
                std::cout << name << (built ? "  [built]" : "  [source only]") << "\n";
            }
        });

    static std::string addName;
    static std::string addInput;
    static std::string addTarget;
    auto* add = profile->add_subcommand("add", "Add an albedo replacement and convert it to a profile-local DXT5 PAA");
    add->add_option("profile", addName, "Profile name")->required();
    add->add_option("input", addInput, "PNG, TGA, JPG, PAA, PAC, or DDS source")->required()->check(CLI::ExistingFile);
    add->add_option("target", addTarget, "Original in-game texture path, e.g. landtext\\trava1.paa")->required();
    add->add_option("--game-dir", gameDir, "Arma CWA install directory");
    add->callback(
        []()
        {
            fs::path target;
            if (!RequireProfile(gameDir, addName) || !NormalizedTarget(addTarget, target))
            {
                std::cerr << "Target must be a safe relative in-game texture path.\n";
                throw CLI::RuntimeError(1);
            }
            if (!WritePaaFromImage(addInput, SourceDir(gameDir, addName) / target))
                throw CLI::RuntimeError(1);
            std::cout << "Added replacement: " << target << "\n";
        });

    static std::string normalInput;
    static std::string normalOutput;
    static float normalStrength = 3.0f;
    auto* normal = texture->add_subcommand(
        "normal", "Create a tangent-space DXT5 PAA normal map from an albedo or height texture");
    normal->add_option("input", normalInput, "Texture to convert to a normal map")
        ->required()
        ->check(CLI::ExistingFile);
    normal->add_option("output", normalOutput, "Output .paa path")->required();
    normal->add_option("--strength", normalStrength, "Surface relief strength (default: 3.0)")
        ->check(CLI::Range(0.1f, 16.0f));
    normal->callback(
        []()
        {
            if (!SaveNormalMap(normalInput, normalOutput, normalStrength))
                throw CLI::RuntimeError(1);
            std::cout << "Created normal map: " << normalOutput << "\n";
        });

    static std::string normalProfile;
    static std::string normalProfileInput;
    static std::string normalProfileTarget;
    static float normalProfileStrength = 3.0f;
    auto* addNormal =
        profile->add_subcommand("add-normal", "Generate and add a normal-map replacement directly to a profile");
    addNormal->add_option("profile", normalProfile, "Profile name")->required();
    addNormal->add_option("input", normalProfileInput, "Albedo or height texture")
        ->required()
        ->check(CLI::ExistingFile);
    addNormal->add_option("target", normalProfileTarget, "Normal-map in-game path, commonly ending in _nohq.paa")
        ->required();
    addNormal->add_option("--strength", normalProfileStrength, "Surface relief strength (default: 3.0)")
        ->check(CLI::Range(0.1f, 16.0f));
    addNormal->add_option("--game-dir", gameDir, "Arma CWA install directory");
    addNormal->callback(
        []()
        {
            fs::path target;
            if (!RequireProfile(gameDir, normalProfile) || !NormalizedTarget(normalProfileTarget, target))
            {
                std::cerr << "Target must be a safe relative in-game texture path.\n";
                throw CLI::RuntimeError(1);
            }
            if (!SaveNormalMap(normalProfileInput, SourceDir(gameDir, normalProfile) / target, normalProfileStrength))
                throw CLI::RuntimeError(1);
            std::cout << "Added normal map: " << target << "\n";
        });

    static std::string buildName;
    auto* build =
        profile->add_subcommand("build", "Pack a profile to its own addon PBO and make a double-click launcher");
    build->add_option("profile", buildName, "Profile name")->required();
    build->add_option("--game-dir", gameDir, "Arma CWA install directory");
    build->callback(
        []()
        {
            if (!BuildProfile(gameDir, buildName))
                throw CLI::RuntimeError(1);
        });
}

} // namespace PoseidonTools
