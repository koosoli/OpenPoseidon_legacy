#include "ModelCommand.hpp"
#include "../SDLPreview.hpp"
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/Asset/Probes/AssetInfo.hpp>
#include <Poseidon/Asset/Probes/AssetPreview.hpp>
#include <Poseidon/Asset/Formats/Common/FormatDetector.hpp>
#include <Poseidon/Asset/Formats/P3D/Odol40.hpp>
#include <Poseidon/Asset/Formats/P3D/Odol49.hpp>
#include <Poseidon/Asset/Formats/P3D/Odol73.hpp>
#include <Poseidon/Asset/Formats/P3D/OdolRevision.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <map>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <cmath>
#include <set>
#include <stdint.h>
#include <CLI/App.hpp>
#include <CLI/Error.hpp>
#include <CLI/Option.hpp>
#include <CLI/TypeTools.hpp>
#include <CLI/Validators.hpp>
#include <cstdio>
#include <functional>
#include <string>
#include <system_error>
#include <utility>

namespace PoseidonTools
{

// Resolve a model's texture reference (e.g. "data\foo.pac") to a file on disk:
// try the reference relative to each root, then a recursive match on its basename.
// `<default>` / `#default#` placeholders resolve to nothing.
static std::filesystem::path resolveTexture(const std::string& texName, const std::filesystem::path& texRoot,
                                            const std::filesystem::path& modelDir)
{
    namespace fs = std::filesystem;
    if (texName.empty() || texName.front() == '<' || texName.front() == '#')
        return {};

    std::string norm = texName;
    std::replace(norm.begin(), norm.end(), '\\', '/');
    fs::path rel(norm);
    fs::path base = rel.filename();

    std::vector<fs::path> roots;
    if (!texRoot.empty())
        roots.push_back(texRoot);
    if (!modelDir.empty())
        roots.push_back(modelDir);

    for (const auto& root : roots)
    {
        std::error_code ec;
        fs::path direct = root / rel;
        if (fs::exists(direct, ec))
            return direct;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec))
        {
            if (ec)
                break;
            if (it->is_regular_file(ec) && it->path().filename() == base)
                return it->path();
        }
    }
    if (fs::exists(norm))
        return fs::path(norm);
    return {};
}

static void setupModelInspect(CLI::App& model)
{
    auto* cmd = model.add_subcommand("inspect", "Inspect P3D model details");
    static std::string inputPath;
    static bool showTextures = false;
    static bool showSelections = false;
    static bool showSections = false;
    static bool showAll = false;
    static bool classify = false;
    static std::string texRoot;

    cmd->add_option("input", inputPath, "Input P3D file path")->required()->check(CLI::ExistingFile);
    cmd->add_flag("-t,--textures", showTextures, "Show texture details");
    cmd->add_flag("-s,--selections", showSelections, "Show named selections");
    cmd->add_flag("--sections", showSections, "Show section details with render hints");
    cmd->add_flag("-a,--all", showAll, "Show all details");
    cmd->add_flag("--classify", classify, "Classify each texture's alpha (opaque/cutout/blend) + render route");
    cmd->add_option("--texroot", texRoot,
                    "Directory to resolve texture paths from (recursive basename match; defaults to model dir)");

    cmd->callback(
        [&]()
        {
            auto info = Poseidon::InspectModel(inputPath);

            std::cout << "File: " << inputPath << std::endl;
            std::cout << "Format: " << info.format << std::endl;
            std::cout << "Version: " << info.version << std::endl;

            if (!info.isSupported)
            {
                std::cerr << "Warning: Format is not fully supported" << std::endl;
                if (!info.errorMessage.empty())
                    std::cerr << "  " << info.errorMessage << std::endl;
            }

            if (!info.valid)
            {
                std::cerr << "Error: Failed to load " << inputPath << std::endl;
                // Without this the reader's diagnosis -- which revision, which
                // field, which boundary it missed -- never reaches the operator.
                if (info.isSupported && !info.errorMessage.empty())
                    std::cerr << "  " << info.errorMessage << std::endl;
                throw CLI::RuntimeError(1);
            }
            std::cout << std::endl;

            std::cout << "LOD Levels: " << info.lodCount << std::endl;
            std::cout << std::endl;

            for (const auto& lod : info.lods)
            {
                std::cout << "LOD " << lod.index << " (Resolution: " << lod.resolution << ")" << std::endl;
                std::cout << "  Points:     " << std::setw(6) << lod.points << std::endl;
                std::cout << "  Faces:      " << std::setw(6) << lod.faces << std::endl;
                std::cout << "  Textures:   " << std::setw(6) << lod.textures << std::endl;
                std::cout << "  Selections: " << std::setw(6) << lod.selections << std::endl;

                if (showAll || showTextures)
                {
                    std::cout << "  Texture List:" << std::endl;
                    for (size_t t = 0; t < lod.textureNames.size(); ++t)
                        std::cout << "    [" << t << "] " << lod.textureNames[t] << std::endl;
                }

                if (showAll || showSelections)
                {
                    std::cout << "  Named Selections:" << std::endl;
                    for (size_t s = 0; s < lod.selectionNames.size(); ++s)
                        std::cout << "    [" << s << "] " << lod.selectionNames[s].first << " ("
                                  << lod.selectionNames[s].second << " points)" << std::endl;
                }

                if (showAll || showSections)
                {
                    std::cout << "  Sections: " << lod.sectionInfos.size() << std::endl;
                    for (const auto& sec : lod.sectionInfos)
                    {
                        std::cout << "    [" << sec.index << "] tex=" << sec.textureName
                                  << " tris=" << sec.triangleCount << " flags=0x" << std::hex << sec.hints << std::dec
                                  << " (" << sec.hintsStr << ")" << std::endl;
                    }
                }
                std::cout << std::endl;
            }

            if (classify)
            {
                std::filesystem::path root = texRoot;
                std::filesystem::path modelDir = std::filesystem::path(inputPath).parent_path();
                std::cout << "Alpha classification";
                if (!root.empty())
                    std::cout << " (texroot: " << root.string() << ")";
                std::cout << ":" << std::endl;

                std::set<std::string> seen;
                int nOpaque = 0, nCutout = 0, nBlend = 0, nMissing = 0;
                for (const auto& lod : info.lods)
                {
                    for (const auto& name : lod.textureNames)
                    {
                        if (name.empty() || name.front() == '<' || name.front() == '#')
                            continue;
                        if (!seen.insert(name).second)
                            continue;

                        std::filesystem::path p = resolveTexture(name, root, modelDir);
                        if (p.empty())
                        {
                            ++nMissing; // not under texroot; counted, not listed (use --texroot to resolve)
                            continue;
                        }
                        Poseidon::DecodedImage img = Poseidon::DecodePAAFile(p.string());
                        if (!img.valid())
                        {
                            std::cout << "  " << name << " -> (decode failed)" << std::endl;
                            ++nMissing;
                            continue;
                        }
                        const size_t n = static_cast<size_t>(img.width) * static_cast<size_t>(img.height);
                        const Poseidon::AlphaStats a = Poseidon::ClassifyAlpha(img.rgba.data(), n);
                        const char* route =
                            a.kind == Poseidon::AlphaStats::Blend    ? "back-to-front alpha pass, NO depth-write"
                            : a.kind == Poseidon::AlphaStats::Cutout ? "opaque pass, depth-write, discard holes"
                                                                     : "opaque pass, depth-write";
                        std::cout << "  " << name << " -> " << Poseidon::AlphaKindName(a.kind) << "  [" << route << "]"
                                  << std::endl;
                        if (a.kind == Poseidon::AlphaStats::Blend)
                            ++nBlend;
                        else if (a.kind == Poseidon::AlphaStats::Cutout)
                            ++nCutout;
                        else
                            ++nOpaque;
                    }
                }
                std::cout << "  Summary: " << nBlend << " blend (deferred), " << nCutout << " cutout, " << nOpaque
                          << " opaque";
                if (nMissing > 0)
                    std::cout << ", " << nMissing << " unresolved (not under texroot)";
                std::cout << std::endl << std::endl;
            }
        });
}

static void setupModelConvert(CLI::App& model)
{
    auto* cmd = model.add_subcommand("convert", "Convert P3D model formats (MLOD/ODOL)");
    static std::string inputPath;
    static std::string outputPath;
    static bool verbose = false;

    cmd->add_option("input", inputPath, "Input P3D file path")->required()->check(CLI::ExistingFile);
    cmd->add_option("output", outputPath, "Output P3D file path")->required();
    cmd->add_flag("-v,--verbose", verbose, "Verbose output");

    cmd->callback(
        [&]()
        {
            if (verbose)
                std::cout << "Converting: " << inputPath << " -> " << outputPath << std::endl;

            auto* shape = new LODShapeWithShadow();
            if (!shape->LoadOptimized(inputPath.c_str()))
            {
                std::cerr << "Error: Failed to load " << inputPath << std::endl;
                delete shape;
                throw CLI::RuntimeError(1);
            }

            if (verbose)
            {
                std::cout << "Loaded successfully" << std::endl;
                std::cout << "LOD levels: " << static_cast<int>(shape->NLevels()) << std::endl;
                for (int i = 0; i < shape->NLevels(); ++i)
                {
                    Shape* lod = shape->Level(i);
                    if (lod)
                    {
                        std::cout << "  LOD " << i << " (res=" << shape->Resolution(i) << "): " << lod->NPoints()
                                  << " points, " << lod->NFaces() << " faces, " << lod->NTextures() << " textures, "
                                  << lod->NNamedSel() << " selections" << std::endl;
                    }
                }
            }

            shape->SaveOptimized(outputPath.c_str());

            if (verbose)
                std::cout << "Saved successfully as ODOL v7" << std::endl;
            else
                std::cout << "Converted: " << inputPath << " -> " << outputPath << std::endl;

            delete shape;
        });
}

static void setupModelRender(CLI::App& model)
{
    auto* cmd = model.add_subcommand("render", "Render P3D model wireframe to image file");
    static std::string inputPath;
    static std::string outputPath;
    static int width = 512;
    static int height = 512;
    static int lodIndex = 0;
    static std::string view = "front";

    cmd->add_option("input", inputPath, "Input P3D file path")->required()->check(CLI::ExistingFile);
    cmd->add_option("-o,--output", outputPath, "Output image path (.png, .bmp, .tga)")->required();
    cmd->add_option("-W,--width", width, "Image width in pixels")->default_val(512);
    cmd->add_option("-H,--height", height, "Image height in pixels")->default_val(512);
    cmd->add_option("-l,--lod", lodIndex, "LOD level to render")->default_val(0);
    cmd->add_option("--view", view, "View: front, back, top, bottom, right, left, 3d, quad")->default_val("front");

    cmd->callback(
        [&]()
        {
            Poseidon::ModelPreviewOptions opts;
            opts.width = width;
            opts.height = height;
            opts.lodIndex = lodIndex;
            opts.view = view;

            auto preview = Poseidon::PreviewModel(inputPath, opts);
            if (!preview.valid())
            {
                std::cerr << "Error: Failed to render model: " << inputPath << std::endl;
                throw CLI::RuntimeError(1);
            }

            if (!preview.saveToFile(outputPath))
            {
                std::cerr << "Error: Failed to save: " << outputPath << std::endl;
                throw CLI::RuntimeError(1);
            }

            std::cout << "Rendered: " << inputPath << " (LOD " << lodIndex << ", " << view << " view, " << width << "x"
                      << height << ") -> " << outputPath << std::endl;
        });
}

namespace
{

// AST-019 -- what unit is a named selection's `selectedFaces` array in?
//
// The question is not academic: the converter turns those entries into triangle
// indices, and if it reads them in the wrong unit every selection arrives empty.
// This prints the evidence rather than the conclusion -- for each selection, how
// many of its entries are valid FACE INDICES and how many are valid FACE-STREAM
// BYTE OFFSETS -- so the answer is read off real files, per revision.
void PrintOdolSelections(const std::string& path, bool proxiesOnly)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        std::cerr << "Error: cannot open " << path << "\n";
        std::exit(1);
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.size() < 8 || std::memcmp(bytes.data(), "ODOL", 4) != 0)
    {
        std::cerr << "Error: not an ODOL model: " << path << "\n";
        std::exit(1);
    }

    QIStream stream(bytes.data(), static_cast<int>(bytes.size()));
    Poseidon::Asset::Formats::BinaryReader reader(stream);
    const auto revision = Poseidon::Asset::Formats::P3D::PeekOdolRevision(reader);
    Poseidon::Asset::Formats::P3D::Odol73StaticModel model;
    try
    {
        switch (revision.version)
        {
            case 40:
                model = Poseidon::Asset::Formats::P3D::ReadOdol40StaticModel(reader, static_cast<int>(bytes.size()));
                break;
            case 48:
            case 49:
            case 50:
            case 52:
                model = Poseidon::Asset::Formats::P3D::ReadOdolA2StaticModel(reader, static_cast<int>(bytes.size()),
                                                                             revision.version);
                break;
            case 73:
                model = Poseidon::Asset::Formats::P3D::ReadOdol73StaticModel(reader, static_cast<int>(bytes.size()));
                break;
            default:
                std::cerr << "Error: ODOL revision " << revision.version << " has no static reader\n";
                std::exit(1);
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "parse failed: " << error.what() << "\n";
        std::exit(1);
    }

    std::cout << "Model:    " << path << "\n";
    std::cout << "Revision: " << revision.version << " (" << revision.generation << ")\n";

    int totalSelections = 0, emptyFaces = 0, allIndex = 0, allOffset = 0, neither = 0;
    int emptySections = 0, withSections = 0;
    for (size_t lodIndex = 0; lodIndex < model.lods.size(); ++lodIndex)
    {
        const auto& lod = model.lods[lodIndex];
        const auto offsets = Poseidon::Asset::Formats::P3D::Odol73FaceStreamOffsets(lod.polygons);
        std::set<uint32_t> boundaries(offsets.begin(), offsets.end());
        const uint32_t faceCount = static_cast<uint32_t>(lod.polygons.faces.size());
        std::cout << "\n-- LOD " << lodIndex << " --\n";
        std::cout << "  faces " << faceCount << ", declared faceDataSize " << lod.polygons.faceDataSize
                  << ", rebuilt stream " << (offsets.empty() ? 0u : offsets.back()) << " (header "
                  << lod.polygons.faceStreamHeaderBytes << " + index " << lod.polygons.faceStreamIndexBytes
                  << " per vertex)\n";
        std::cout << "  sections " << lod.sections.size() << ", selections " << lod.namedSelections.size()
                  << ", proxies " << lod.header.proxies.size() << "\n";
        // MAT-046: a proxy's marker face is located by the PROXY record's own sectionIndex,
        // not by its named selection (which is empty on this generation). Print both units
        // side by side so the claim is checkable rather than asserted.
        for (size_t si = 0; si < lod.sections.size(); ++si)
        {
            const auto& sec = lod.sections[si];
            uint32_t secFaces = 0;
            for (size_t face = 0; face < lod.polygons.faces.size(); ++face)
                if (offsets[face] >= static_cast<uint32_t>(sec.faceLower) &&
                    offsets[face] < static_cast<uint32_t>(sec.faceUpper))
                    ++secFaces;
            std::cout << "    section " << std::setw(3) << si << " bytes [" << sec.faceLower << "," << sec.faceUpper
                      << ") faces " << std::setw(5) << secFaces << " texture " << std::setw(4) << sec.textureIndex
                      << " material " << sec.materialIndex << "\n";
        }
        for (const auto& proxy : lod.header.proxies)
            std::cout << "    proxy   " << std::left << std::setw(50) << proxy.model.substr(0, 50) << std::right
                      << " seq " << std::setw(4) << proxy.sequenceId << " namedSel " << std::setw(4)
                      << proxy.namedSelectionIndex << " bone " << std::setw(4) << proxy.boneIndex << " section "
                      << proxy.sectionIndex << "\n";
        for (const auto& selection : lod.namedSelections)
        {
            const bool isProxy = selection.name.rfind("proxy:", 0) == 0;
            if (proxiesOnly && !isProxy)
                continue;
            ++totalSelections;
            size_t indexValid = 0, offsetValid = 0;
            for (int32_t value : selection.selectedFaces)
            {
                if (value >= 0 && static_cast<uint32_t>(value) < faceCount)
                    ++indexValid;
                if (value >= 0 && boundaries.count(static_cast<uint32_t>(value)) != 0)
                    ++offsetValid;
            }
            if (selection.selectedFaces.empty())
                ++emptyFaces;
            else if (indexValid == selection.selectedFaces.size() && offsetValid != selection.selectedFaces.size())
                ++allIndex;
            else if (offsetValid == selection.selectedFaces.size() && indexValid != selection.selectedFaces.size())
                ++allOffset;
            else if (indexValid != selection.selectedFaces.size() && offsetValid != selection.selectedFaces.size())
                ++neither;
            if (selection.sections.empty())
                ++emptySections;
            else
                ++withSections;
            std::cout << "    " << std::left << std::setw(46) << selection.name.substr(0, 46) << " faces "
                      << std::setw(5) << selection.selectedFaces.size() << " valid-as-index " << std::setw(5)
                      << indexValid << " valid-as-offset " << std::setw(5) << offsetValid << " sections "
                      << std::setw(3) << selection.sections.size() << " verts " << selection.selectedVertices.size()
                      << (selection.isSectional ? " sectional" : "") << "\n";
        }
    }
    std::cout << "\n-- totals --\n";
    std::cout << "selections            " << totalSelections << "\n";
    std::cout << "selectedFaces empty   " << emptyFaces << "\n";
    std::cout << "all entries are valid face INDICES only   " << allIndex << "\n";
    std::cout << "all entries are valid byte OFFSETS only   " << allOffset << "\n";
    std::cout << "neither                                   " << neither << "\n";
    std::cout << "sections empty        " << emptySections << "\n";
    std::cout << "sections present      " << withSections << "\n";
}

} // namespace

static void setupModelSelections(CLI::App& model)
{
    static std::string inputFile;
    static bool proxiesOnly = false;
    auto* cmd = model.add_subcommand("selections", "Report a binarised model's named selections and their unit");
    cmd->add_option("input", inputFile, "P3D file")->required()->check(CLI::ExistingFile);
    cmd->add_flag("--proxies", proxiesOnly, "Only list proxy: selections");
    cmd->callback(
        []()
        {
            PrintOdolSelections(inputFile, proxiesOnly);
            std::exit(0);
        });
}

static void setupModelShow(CLI::App& model)
{
    auto* cmd = model.add_subcommand("show", "Display P3D model wireframe in a window");
    static std::string inputPath;
    static std::string screenshotPath;
    static int lodIndex = 0;
    static std::string view = "front";

    cmd->add_option("input", inputPath, "Input P3D file path")->required()->check(CLI::ExistingFile);
    cmd->add_option("--screenshot", screenshotPath, "Save screenshot to file and exit");
    cmd->add_option("-l,--lod", lodIndex, "LOD level to render")->default_val(0);
    cmd->add_option("--view", view, "View: front, back, top, bottom, right, left, 3d, quad")->default_val("front");

    cmd->callback(
        [&]()
        {
            int imgW = (view == "quad") ? 900 : 800;
            int imgH = imgW;

            Poseidon::ModelPreviewOptions opts;
            opts.width = imgW;
            opts.height = imgH;
            opts.lodIndex = lodIndex;
            opts.view = view;

            if (!screenshotPath.empty())
            {
                auto preview = Poseidon::PreviewModel(inputPath, opts);
                if (!preview.valid())
                {
                    std::cerr << "Error: Failed to render model" << std::endl;
                    throw CLI::RuntimeError(1);
                }
                if (!preview.saveToFile(screenshotPath))
                    throw CLI::RuntimeError(1);
                std::cout << "Screenshot: " << screenshotPath << " (" << imgW << "x" << imgH << ")" << std::endl;
                return;
            }

            auto preview = Poseidon::PreviewModel(inputPath, opts);
            if (!preview.valid())
            {
                std::cerr << "Error: Failed to render model" << std::endl;
                throw CLI::RuntimeError(1);
            }

            char title[256];
            std::snprintf(title, sizeof(title), "PoseidonTools - %s (LOD %d, %s)", inputPath.c_str(), lodIndex,
                          view.c_str());
            std::string viewCopy = view;
            std::string pathCopy = inputPath;
            int lodCopy = lodIndex;
            DisplayWindowRGB(title, imgW, imgH, preview.data.data(),
                             [pathCopy, lodCopy, viewCopy](int w, int h) -> std::vector<uint8_t>
                             {
                                 Poseidon::ModelPreviewOptions resizeOpts;
                                 resizeOpts.width = w;
                                 resizeOpts.height = h;
                                 resizeOpts.lodIndex = lodCopy;
                                 resizeOpts.view = viewCopy;
                                 auto resized = Poseidon::PreviewModel(pathCopy, resizeOpts);
                                 return resized.valid() ? std::move(resized.data) : std::vector<uint8_t>{};
                             });
        });
}

void ModelCommand::Setup(CLI::App& app)
{
    auto* model = app.add_subcommand("model", "P3D model operations");
    model->require_subcommand(1);

    setupModelInspect(*model);
    setupModelConvert(*model);
    setupModelRender(*model);
    setupModelShow(*model);
    setupModelSelections(*model);
}

} // namespace PoseidonTools
