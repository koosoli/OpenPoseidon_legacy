#include "Reskin.hpp"

#include "Renderer.hpp"

#include <Poseidon/Graphics/Textures/Image.hpp>
#include <Poseidon/Graphics/Textures/PAAEncoder.hpp>
#include <Poseidon/IO/PackFiles.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

namespace island
{
namespace fs = std::filesystem;

namespace
{
enum Style
{
    Snow,
    Desert,
    DrySeason,
    Autumn,
    Lush,
    Mud,
    Scorched,
    Volcanic,
    Mars,
    Moon,
    NStyles
};

float Clamp01(float v)
{
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

void Mix(float c[3], const float target[3], float k)
{
    for (int i = 0; i < 3; i++)
        c[i] += (target[i] - c[i]) * k;
}

void Ramp(const float a[3], const float b[3], float t, float out[3])
{
    for (int i = 0; i < 3; i++)
        out[i] = a[i] + (b[i] - a[i]) * t;
}

unsigned Hash(unsigned v)
{
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return v;
}
} // namespace

const std::vector<ReskinStyle>& ReskinStyles()
{
    static const std::vector<ReskinStyle> styles = {
        {"Snow", "snow", "Winter: the land under snow, roads and rock showing through.", 0.0f, 0.0f},
        {"Desert", "desert", "Sand and bare rock; most of the trees gone.", 0.85f, 0.6f},
        {"Dry season", "dry", "Grass burnt to straw by the sun; the rest as it was.", 0.1f, 0.1f},
        {"Autumn", "autumn", "Grass and leaf litter in browns, oranges and yellows.", 0.0f, 0.0f},
        {"Lush", "lush", "Wet-season green: richer, deeper colours.", 0.0f, 0.0f},
        {"Mud", "mud", "After the rains: dark, brown and soaked.", 0.0f, 0.15f},
        {"Scorched", "burnt", "After the fire: charred ground, few trees left standing.", 0.7f, 0.9f},
        {"Volcanic", "ash", "Dark ash and basalt, nothing growing.", 0.95f, 0.95f},
        {"Mars", "mars", "Red dust. No vegetation.", 1.0f, 1.0f},
        {"Moon", "moon", "Grey regolith. No vegetation.", 1.0f, 1.0f},
    };
    return styles;
}

void ReskinPixels(int style, float strength, uint8_t* rgba, size_t count)
{
    if (!rgba || count == 0 || style < 0 || style >= NStyles || strength <= 0)
        return;
    double sum = 0;
    for (size_t i = 0; i < count; i++)
        sum += rgba[i * 4] * 0.30 + rgba[i * 4 + 1] * 0.59 + rgba[i * 4 + 2] * 0.11;
    const float mean = static_cast<float>(sum / count / 255.0);
    strength = Clamp01(strength);

    for (size_t i = 0; i < count; i++)
    {
        uint8_t* p = rgba + i * 4;
        float c[3] = {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f};
        const float lum = c[0] * 0.30f + c[1] * 0.59f + c[2] * 0.11f;
        const float mx = std::max(c[0], std::max(c[1], c[2])), mn = std::min(c[0], std::min(c[1], c[2]));
        const float sat = mx - mn;
        const float detail = lum - mean; // what makes it a texture and not a flat colour
        // asphalt, concrete, painted lines: grey things keep more of themselves
        const float hard = Clamp01(1.0f - sat / 0.07f) * Clamp01((0.62f - std::fabs(lum - 0.35f)) / 0.3f);
        // things that grow: green wins over red and blue (olive counts a little)
        const float veg = Clamp01((c[1] - std::max(c[0] * 0.92f, c[2])) * 7.0f + (c[1] > c[2] * 1.15f ? 0.35f : 0.0f));
        float t[3];
        float k = 0;
        switch (style)
        {
            case Snow:
            {
                const float v = Clamp01(0.92f + detail * 1.1f);
                t[0] = v * 0.97f, t[1] = v * 0.985f, t[2] = v * 1.02f;
                k = 0.93f - 0.6f * hard;
                break;
            }
            case Desert:
            {
                const float lo[3] = {0.60f, 0.45f, 0.27f}, hi[3] = {0.96f, 0.85f, 0.62f};
                Ramp(lo, hi, Clamp01(0.55f + detail * 2.2f), t);
                k = 0.9f - 0.65f * hard;
                break;
            }
            case DrySeason:
                t[0] = Clamp01(lum * 1.45f), t[1] = Clamp01(lum * 1.22f), t[2] = lum * 0.62f;
                k = veg * 0.92f;
                break;
            case Autumn:
            {
                const float lo[3] = {0.55f, 0.24f, 0.07f}, hi[3] = {0.86f, 0.66f, 0.18f};
                Ramp(lo, hi, Clamp01(0.5f + detail * 3.0f), t);
                for (float& v : t)
                    v *= 0.55f + lum * 1.1f;
                k = veg * 0.88f;
                break;
            }
            case Lush:
                for (int j = 0; j < 3; j++)
                    t[j] = Clamp01(lum * 0.92f + (c[j] - lum) * 1.7f);
                t[1] = Clamp01(t[1] * 1.10f);
                k = 1.0f - 0.8f * hard;
                break;
            case Mud:
                t[0] = lum * 0.74f, t[1] = lum * 0.60f, t[2] = lum * 0.42f;
                k = 0.72f - 0.5f * hard;
                break;
            case Scorched:
                t[0] = lum * 0.44f + 0.015f, t[1] = lum * 0.37f + 0.012f, t[2] = lum * 0.32f + 0.01f;
                k = 0.92f - 0.45f * hard;
                break;
            case Volcanic:
            {
                const float v = Clamp01(0.21f + detail * 1.6f);
                t[0] = v * 1.05f, t[1] = v * 0.98f, t[2] = v;
                k = 0.93f - 0.5f * hard;
                break;
            }
            case Mars:
            {
                const float lo[3] = {0.42f, 0.17f, 0.09f}, hi[3] = {0.86f, 0.50f, 0.29f};
                Ramp(lo, hi, Clamp01(0.5f + detail * 2.2f), t);
                k = 0.93f - 0.5f * hard;
                break;
            }
            default: // Moon
            {
                const float v = Clamp01(0.47f + detail * 1.5f);
                t[0] = t[1] = t[2] = v;
                k = 0.95f - 0.35f * hard;
                break;
            }
        }
        Mix(c, t, Clamp01(k) * strength);
        p[0] = static_cast<uint8_t>(Clamp01(c[0]) * 255.0f + 0.5f);
        p[1] = static_cast<uint8_t>(Clamp01(c[1]) * 255.0f + 0.5f);
        p[2] = static_cast<uint8_t>(Clamp01(c[2]) * 255.0f + 0.5f);
    }
}

std::string GuessBaseWorld(const std::string& islandFile)
{
    const std::string stem = LowerPath(fs::path(islandFile).stem().string());
    if (stem == "eden")
        return "Eden";
    if (stem == "abel")
        return "Abel";
    if (stem == "cain")
        return "Cain";
    if (stem == "noe")
        return "Noe";
    return "Intro";
}

namespace
{
bool ValidAddonName(const std::string& s)
{
    if (s.empty() || s.size() > 12)
        return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    return !(s[0] >= '0' && s[0] <= '9');
}

bool PackFolder(const fs::path& folder, const fs::path& pbo, std::string& error)
{
    std::error_code ec;
    fs::remove(pbo, ec);
    Poseidon::FileBankManager mgr;
    if (mgr.Create(pbo.string().c_str(), folder.string().c_str(), false) != Poseidon::LSOK || !fs::exists(pbo, ec))
    {
        error = "the archive " + pbo.string() + " could not be made";
        return false;
    }
    return true;
}
} // namespace

bool ReskinIsland(Island& is, GameData& game, const ReskinOptions& opt, ReskinReport& report, std::string& error)
{
    report = ReskinReport();
    const std::vector<ReskinStyle>& styles = ReskinStyles();
    if (opt.style < 0 || opt.style >= static_cast<int>(styles.size()))
    {
        error = "no such style";
        return false;
    }
    if (!ValidAddonName(opt.addon))
    {
        error = "the addon name has to be 1 to 12 small letters, digits or _ and start with a letter";
        return false;
    }
    if (is.size <= 0 || opt.outFolder.empty())
    {
        error = "nothing to reskin, or nowhere to write it";
        return false;
    }
    std::error_code ec;
    const fs::path folder = fs::path(opt.outFolder) / opt.addon;
    fs::create_directories(folder, ec);
    if (!fs::is_directory(folder, ec))
    {
        error = "cannot make the folder " + folder.string();
        return false;
    }

    // first every texture as a file; the island is only touched when that has gone well
    struct Renamed
    {
        int slot;
        std::string name;
    };
    std::vector<Renamed> renamed;
    std::set<std::string> stems;
    for (int slot = 1; slot < kMaxTextures; slot++)
    {
        const std::string& old = is.textures[slot];
        if (old.empty())
            continue;
        Image img = game.LoadTexture(old);
        if (!img.Valid())
        {
            report.failed++; // not found: left pointing at the old name
            continue;
        }
        ReskinPixels(opt.style, opt.strength, img.rgba.data(), static_cast<size_t>(img.width) * img.height);
        // "eden\tntvtntn.paa" -> "tntvtntn"; two folders may have a file of the same name
        std::string stem = LowerPath(fs::path(LowerPath(old)).stem().string());
        if (stem.size() > 8)
            stem.resize(8);
        for (int n = 2; stems.count(stem); n++)
        {
            const std::string tail = std::to_string(n);
            stem = stem.substr(0, 8 - tail.size()) + tail;
        }
        stems.insert(stem);
        const fs::path file = folder / (stem + ".paa");
        const Poseidon::Image out = Poseidon::Image::FromRGBA(img.width, img.height, std::move(img.rgba));
        if (!Poseidon::PAAEncoder::WritePAA(file.string(), out, Poseidon::PixelFormat::DXT1))
        {
            error = "cannot write " + file.string();
            return false;
        }
        renamed.push_back({slot, opt.addon + "\\" + stem + ".paa"});
        report.textures++;
    }
    if (renamed.empty())
    {
        error = "none of the island's textures could be read (is the game folder set?)";
        return false;
    }

    is.BeginEdit((std::string("Reskin: ") + styles[opt.style].name).c_str());
    for (const Renamed& r : renamed)
        is.RenameTexture(r.slot, r.name);
    if (opt.removeTrees > 0 || opt.removeBushes > 0)
    {
        ObjectList kept;
        kept.reserve(is.objects.size());
        for (const Object& o : is.objects)
        {
            const ObjectKind kind = KindOf(o.model);
            const float share = kind == ObjectKind::Tree ? opt.removeTrees : (kind == ObjectKind::Bush ? opt.removeBushes : 0.0f);
            const float dice = (Hash(static_cast<unsigned>(o.id) * 2654435761u + opt.seed) & 0xFFFF) / 65536.0f;
            if (share > 0 && dice < share)
                report.objectsRemoved++;
            else
                kept.push_back(o);
        }
        if (report.objectsRemoved)
            is.ReplaceObjects(std::move(kept));
    }
    is.EndEdit();
    game.AddLooseFolder(opt.outFolder);
    report.folder = folder.string();

    if (opt.worldConfig)
    {
        const fs::path wrp = folder / (opt.addon + ".wrp");
        if (!is.Save(wrp.string(), error))
            return false;
        fs::remove(wrp.string() + ".bak", ec);
        report.wrp = wrp.string();
        const std::string base = opt.baseWorld.empty() ? "Intro" : opt.baseWorld;
        std::string title = opt.title.empty() ? opt.addon : opt.title;
        for (char& c : title)
            if (c == '"')
                c = '\'';
        std::ofstream cfg(folder / "config.cpp", std::ios::trunc);
        cfg << "// Made by Island Editor: " << styles[opt.style].name << " reskin.\n";
        cfg << "class CfgPatches\n{\n\tclass " << opt.addon << "\n\t{\n\t\tunits[] = {};\n\t\tweapons[] = {};\n\t\tworlds[] = {"
            << opt.addon << "};\n\t\trequiredVersion = 1.75;\n\t};\n};\n\n";
        cfg << "class CfgWorlds\n{\n\tclass DefaultWorld {};\n";
        if (base != "DefaultWorld")
            cfg << "\tclass " << base << ": DefaultWorld {};\n";
        cfg << "\tclass " << opt.addon << ": " << base << "\n\t{\n\t\tdescription = \"" << title << "\";\n\t\tworldName = \"\\"
            << opt.addon << "\\" << opt.addon << ".wrp\";\n\t};\n};\n\n";
        cfg << "class CfgWorldList\n{\n\tclass " << opt.addon << " {};\n};\n";
    }
    if (opt.pack)
    {
        const fs::path pbo = fs::path(opt.outFolder) / (opt.addon + ".pbo");
        if (!PackFolder(folder, pbo, error))
            return false;
        report.pbo = pbo.string();
        if (opt.install && !game.Folder().empty())
        {
            const fs::path dest = fs::path(game.Folder()) / "AddOns" / pbo.filename();
            fs::create_directories(dest.parent_path(), ec);
            fs::copy_file(pbo, dest, fs::copy_options::overwrite_existing, ec);
            if (ec)
            {
                error = "the archive was made, but could not be copied to " + dest.string() + ": " + ec.message();
                return false;
            }
            report.installed = dest.string();
        }
    }
    return true;
}

bool IsReskinAddon(const Island& is)
{
    if (is.path.empty())
        return false;
    std::error_code ec;
    const fs::path p(is.path);
    return fs::exists(p.parent_path() / "config.cpp", ec) &&
           LowerPath(p.stem().string()) == LowerPath(p.parent_path().filename().string());
}

bool RepackAddon(Island& is, std::string& pbo, std::string& error)
{
    if (!IsReskinAddon(is))
    {
        error = "this island is not inside an addon folder";
        return false;
    }
    if (!is.Save(is.path, error))
        return false;
    std::error_code ec;
    fs::remove(is.path + ".bak", ec);
    const fs::path folder = fs::path(is.path).parent_path();
    const fs::path out = folder.parent_path() / (folder.filename().string() + ".pbo");
    if (!PackFolder(folder, out, error))
        return false;
    pbo = out.string();
    return true;
}
} // namespace island
