#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Terrain/ObjectStreamTextureNames.hpp>

#include <algorithm>
#include <string>
#include <unordered_map>

using namespace Poseidon;
namespace Mat = Poseidon::Asset::Material;

namespace
{
bool Has(const std::vector<std::string>& names, const std::string& expected)
{ return std::find(names.begin(), names.end(), expected) != names.end(); }

Model::Material Material(const std::string& path, const std::string& face, const std::string& stage)
{
    Model::Material material;
    material.materialPath = path;
    material.texturePath = face;
    Model::MaterialStage embedded;
    embedded.texturePath = stage;
    material.embeddedStages.push_back(std::move(embedded));
    return material;
}
}

TEST_CASE("Texture inputs deduplicate complete native materials but retain each LOD's inputs", "[preparer][texture-names]")
{
    Model::Model model;
    model.lodLevels.resize(2);
    model.lodLevels[0].mesh.materials.push_back(Material("Assets/Pine.emat", "Face/near.PAA", "Maps/near.PAC"));
    model.lodLevels[1].mesh.materials.push_back(Material("assets\\pine.EMAT", "Face/far.paa", "Maps/far.pac"));
    model.lodLevels[1].mesh.materials.push_back(Material("Assets/Bush.emat", "Face/bush.paa", "Maps/bush.pac"));
    Model::MaterialStage nested;
    nested.texturePath = "enft|0.4,0.5,0.6|enfa|Masks/Layer.edds|Colour/Layer.edds";
    model.lodLevels[0].mesh.materials[0].embeddedStages.push_back(std::move(nested));
    const std::unordered_map<std::string, std::string> identities{
        {"Assets/Pine.emat", "assets/pine.emat"}, {"assets\\pine.EMAT", "assets/pine.emat"},
        {"Assets/Bush.emat", "assets/bush.emat"}};
    int pineReads = 0, bushReads = 0;
    const auto names = CollectObjectStreamTextureNames(model, true, true,
        [&](const std::string& path) { return identities.at(path); },
        [&](const std::string& path, Mat::EmatMaterial& out, bool& complete) {
            const bool pine = identities.at(path) == "assets/pine.emat";
            if (pine) ++pineReads; else ++bushReads;
            out = Mat::ParseEmat(pine ?
                "MatPBR {\n BCRMap \"{1111}Colour/Pine.edds\"\n OpacityMap \"{2222}Masks/Pine.edds\"\n NormalMap \"{3333}Maps/Pine_N.edds\"\n}\n" :
                "MatPBR {\n AlbedoMap \"{4444}Colour/Bush.edds\"\n OpacityMap \"{5555}Masks/Bush.edds\"\n NormalMap \"{6666}Maps/Bush_N.edds\"\n}\n");
            complete = true;
            return out.valid();
        });
    CHECK(pineReads == 1);
    CHECK(bushReads == 1);
    for (const std::string key : {"face\\near.paa", "maps\\near.pac", "face\\far.paa", "maps\\far.pac",
                                 "face\\bush.paa", "maps\\bush.pac", "colour\\pine.edds", "masks\\pine.edds",
                                 "maps\\pine_n.edds", "colour\\bush.edds", "masks\\bush.edds", "maps\\bush_n.edds",
                                 "enfa|masks\\pine.edds|colour\\pine.edds", "enfa|masks\\bush.edds|colour\\bush.edds"})
        CHECK(Has(names, key));
    CHECK(Has(names, "enft|0.4,0.5,0.6|enfa|masks\\layer.edds|colour\\layer.edds"));
    CHECK_FALSE(Has(names, "masks\\layer.edds"));
    CHECK_FALSE(Has(names, "colour\\layer.edds"));
    CHECK(names.size() == 15);
}

TEST_CASE("Loose material identities preserve case and DDS preparation gates", "[preparer][texture-names]")
{
    Model::Model model;
    model.lodLevels.resize(1);
    auto& materials = model.lodLevels[0].mesh.materials;
    materials.push_back(Material("Loose/Plant.emat", "face.paa", "embedded.edds"));
    materials.push_back(Material("loose/plant.emat", "face.paa", "embedded.dds"));
    materials.push_back(materials[0]);
    int reads = 0;
    const auto names = CollectObjectStreamTextureNames(model, false, true,
        [](const std::string& path) { return path; },
        [&](const std::string& path, Mat::EmatMaterial& out, bool& complete) {
            ++reads;
            out = Mat::ParseEmat(path == "Loose/Plant.emat" ?
                "MatPBR {\n NormalMap \"{1111}upper.paa\"\n OpacityMap \"{2222}coverage.paa\"\n BCRMap \"{3333}colour.paa\"\n}\n" :
                "MatPBR {\n NormalMap \"{4444}lower.pac\"\n}\n");
            complete = true;
            return true;
        });
    CHECK(reads == 2);
    CHECK(names.size() == 5);
    CHECK(Has(names, "face.paa"));
    CHECK(Has(names, "upper.paa"));
    CHECK(Has(names, "lower.pac"));
    CHECK(Has(names, "coverage.paa"));
    CHECK(Has(names, "colour.paa"));
    CHECK_FALSE(Has(names, "embedded.edds"));
    CHECK_FALSE(Has(names, "embedded.dds"));
    CHECK_FALSE(Has(names, "enfa|coverage.paa|colour.paa"));
}

TEST_CASE("Partial material inheritance remains retryable and dedup ends with the model job", "[preparer][texture-names]")
{
    Model::Model model;
    model.lodLevels.resize(1);
    for (int i = 0; i < 3; ++i)
        model.lodLevels[0].mesh.materials.push_back(Material("assets/child.emat", "own.paa", "own-stage.pac"));
    int reads = 0, parentReads = 0;
    auto read = [&](const std::string&, Mat::EmatMaterial& out, bool& complete) {
        ++reads;
        out = Mat::ParseEmat("MatPBR : \"{1111}assets/parent.emat\" {\n BCRMap \"{2222}colour.edds\"\n}\n");
        complete = Mat::ResolveEmatInheritance(out,
            [&](const std::string& path, std::string& text) {
                CHECK(path == "assets/parent.emat");
                if (++parentReads == 1) return false;
                text = "MatPBR {\n NormalMap \"{3333}inherited-normal.edds\"\n OpacityMap \"{4444}inherited-opacity.edds\"\n}\n";
                return true;
            });
        return out.valid();
    };
    auto identity = [](const std::string& path) { return path; };
    const auto names = CollectObjectStreamTextureNames(model, true, true, identity, read);
    CHECK(reads == 2);
    CHECK(parentReads == 2);
    CHECK(Has(names, "own.paa"));
    CHECK(Has(names, "own-stage.pac"));
    CHECK(Has(names, "colour.edds"));
    CHECK(Has(names, "inherited-normal.edds"));
    CHECK(Has(names, "inherited-opacity.edds"));
    CHECK(Has(names, "enfa|inherited-opacity.edds|colour.edds"));
    const auto fresh = CollectObjectStreamTextureNames(model, true, true, identity, read);
    CHECK(reads == 3); // The prior job's complete entry must not suppress this read.
    CHECK(parentReads == 3);
    CHECK(fresh.size() == names.size());
    for (const auto& key : names) CHECK(Has(fresh, key));
}

TEST_CASE("Missing and malformed materials do not poison texture collection or retries", "[preparer][texture-names]")
{
    Model::Model model;
    model.lodLevels.resize(1);
    for (int i = 0; i < 4; ++i)
        model.lodLevels[0].mesh.materials.push_back(Material("assets/temporary.emat", "face.paa", "stage.pac"));
    model.lodLevels[0].mesh.materials.push_back(Material("ignored.rvmat", "other.paa", "other.pac"));
    int reads = 0;
    const auto names = CollectObjectStreamTextureNames(model, true, false,
        [](const std::string& path) { return path; },
        [&](const std::string& path, Mat::EmatMaterial& out, bool& complete) {
            CHECK(path == "assets/temporary.emat");
            ++reads;
            if (reads == 1) return false;
            if (reads == 2) { complete = true; return true; } // invalid output is not cached
            out = Mat::ParseEmat("MatPBR {\n NormalMap \"{1111}normal.paa\"\n BCRMap \"{2222}disabled.edds\"\n}\n");
            complete = true;
            return true;
        });
    CHECK(reads == 3);
    CHECK(names.size() == 5);
    CHECK(Has(names, "face.paa"));
    CHECK(Has(names, "stage.pac"));
    CHECK(Has(names, "normal.paa"));
    CHECK(Has(names, "other.paa"));
    CHECK(Has(names, "other.pac"));
    CHECK_FALSE(Has(names, "disabled.edds"));
}
