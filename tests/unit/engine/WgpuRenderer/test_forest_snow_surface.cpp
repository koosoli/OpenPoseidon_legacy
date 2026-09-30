#include "../../../../engine/WgpuRenderer/ForestSnowSurface.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <limits>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
using namespace Poseidon::ForestSnowSurface;
int main(int argc, char** argv)
{
    assert(argc == 3);
    std::set<std::string> models;
    size_t visualCount = 0, admitted = 0, rejectedBark = 0;
    for (int file = 1; file < argc; ++file) {
    std::ifstream fixture(argv[file]);
    assert(fixture.good());
    for (std::string line; std::getline(fixture,line); ) {
        std::istringstream record(line);
        std::string name, levelText, resolutionText, textures;
        assert(std::getline(record,name,'|') && std::getline(record,levelText,'|') &&
            std::getline(record,resolutionText,'|') && std::getline(record,textures));
        const auto model = name.starts_with("o\\") ? name : "data3d\\" + name + ".p3d";
        const int level = std::stoi(levelText);
        const float resolution = std::stof(resolutionText);
        assert(AuditedLevel(model,level,resolution));
        assert(!AuditedLevel(model,level,resolution + 0.125f));
        assert(!AuditedLevel(model,-1,resolution) && !AuditedLevel(model,4,resolution));
        std::set<std::string> actualTextures;
        std::istringstream members(textures);
        for (std::string texture; std::getline(members,texture,';'); ) actualTextures.insert(texture);
        // Independent source census detects an overbroad LOD texture membership
        // as well as omission: every known texture is checked at every real LOD.
        for (const auto& texture : Textures) {
            const bool present = actualTextures.count(std::string(texture.path)) != 0;
            const auto expected = present ? texture.atlas : Atlas::None;
            assert(AdmitAtlas(model,level,resolution,texture.path,true,true,true) == expected);
            // Real OptimizeShapes compacts remaining levels. Their source
            // material row follows original resolution, not its former slot.
            for (int retained = 0; retained <= level; ++retained)
                assert(AdmitAtlas(model,retained,resolution,texture.path,true,true,true) == expected);
            assert(AdmitAtlas(model,level+1,resolution,texture.path,true,true,true) == Atlas::None);
            assert(AdmitAtlas("other\\" + name + ".p3d",level,resolution,texture.path,true,true,true) == Atlas::None);
            if (present && expected != Atlas::None) {
                ++admitted;
                assert(AdmitAtlas(model,level,resolution,texture.path,false,true,true) == Atlas::None);
                assert(AdmitAtlas(model,level,resolution,texture.path,true,false,true) == Atlas::None);
                assert(AdmitAtlas(model,level,resolution,texture.path,true,true,false) == Atlas::None);
            } else if (present) { ++rejectedBark; }
        }
        models.insert(model); ++visualCount;
    }
    }
    assert(models.size() == 25 && visualCount == 84 && admitted > 190 && rejectedBark > 35);
    assert(!AuditedModel("o\\tree\\les_nw_ctver_mlaz.p3d")); // no visual geometry
    assert(!AuditedModel("o\\tree\\les_singlestrom.p3d")); // individual tree, not forest owner
    assert(AdmitAtlas("o\\tree\\les_nw_jehl_t1.p3d",0,2,"o\\tree\\dd_les_vetvicky.pac",true,true,true) == Atlas::None);
    assert(AdmitAtlas("o\\tree\\les_nw_jehl_t1.p3d",2,12,"o\\tree\\dd_les_lod.pac",true,true,true) == Atlas::NoeStrip);
    constexpr auto square = "data3d\\les ctverec.p3d";
    constexpr auto passable = "data3d\\les ctverec pruchozi_t1.p3d";
    assert(AssetEquals("DATA3D/LES CTverec.P3D", square));
    assert(AdmitAtlas(square,3,40,"data\\stromky2.pac",true,true,true) == Atlas::StromkyStrip);
    assert(AdmitAtlas(passable,2,12,"merged\\00002.paa",true,true,true) == Atlas::ForestLeft);
    for (auto model : {"data3d\\les unknown.p3d", "other\\les ctverec.p3d", "les ctverec.p3d"}) {
        assert(!AuditedModel(model));
        assert(AdmitAtlas(model,3,40,"data\\stromky2.pac",true,true,true) == Atlas::None);
    }
    for (int level : {-1,4,100})
        assert(AdmitAtlas(square,level,40,"data\\stromky2.pac",true,true,true) == Atlas::None);
    assert(AdmitAtlas(square,0,40,"data\\stromky2.pac",true,true,true) == Atlas::StromkyStrip);
    auto ambiguous = Models[0]; ambiguous.resolutions[1] = ambiguous.resolutions[0];
    assert(SourceVisualIndex(ambiguous,ambiguous.resolutions[0]) == -1);
    for (float resolution : {0.0f,12.0f,39.0f,40.01f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
        assert(AdmitAtlas(square,3,resolution,"data\\stromky2.pac",true,true,true) == Atlas::None);
    for (auto texture : {"data\\kmen1_les.pac", "data\\les_dark_new.pac", "merged\\00002.paa",
                         "data\\stromky2.paa", "other\\stromky2.pac", ""})
        assert(AdmitAtlas(square,3,40,texture,true,true,true) == Atlas::None);
    for (int rejected = 0; rejected < 3; ++rejected) {
        std::array<bool,3> gates{true,true,true}; gates[rejected] = false;
        assert(AdmitAtlas(square,3,40,"data\\stromky2.pac",gates[0],gates[1],gates[2]) == Atlas::None);
    }
    for (int mode : {0,1}) assert(OwnerAllowed(square,true,true,true,true,true,mode,true));
    for (int mode : {-1,2,3}) assert(!OwnerAllowed(square,true,true,true,true,true,mode,true));
    for (int rejected = 0; rejected < 6; ++rejected) {
        std::array<bool,6> gates{true,true,true,true,true,true}; gates[rejected] = false;
        assert(!OwnerAllowed(square,gates[0],gates[1],gates[2],gates[3],gates[4],1,gates[5]));
    }
    assert(!OwnerAllowed("data3d\\str habr.p3d",true,true,true,true,true,1,true));
    assert(Encode(Atlas::None) == 0 && Encode(Atlas::StromkyStrip) == 65536 && Encode(Atlas::ForestLeft) == 131072);
    assert((AtlasMask & 127u) == 0 && OwnerProof != 1.0f && FragmentProof < 0);
    assert((Encode(Atlas::AbelSides) & AtlasMask) == 9u << AtlasShift);
    std::printf("Source census: %zu models, %zu visual LODs, %zu foliage tuples, %zu bark/bare-branch negatives.\n",
        models.size(),visualCount,admitted,rejectedBark);
    std::puts("Forest snow exact stock owner/LOD/texture/material/private-lane admission passed.");
}
