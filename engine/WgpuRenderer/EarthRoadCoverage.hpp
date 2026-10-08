#pragma once
#include <string_view>

namespace Poseidon::EarthRoadCoverage
{
// A dedicated Earth owner may use the retained cutout/A2C pipeline for stock
// surface-road alpha. This never licenses native road ownership or glass.
inline bool StockRoad(std::string_view name)
{
    const auto lower=[](char c){return c>='A'&&c<='Z'?char(c-'A'+'a'):c;};
    const auto slash=[](char c){return c=='/'||c=='\\';};
    if(name.size()<12||lower(name[0])!='o'||!slash(name[1])||
       lower(name[2])!='r'||lower(name[3])!='o'||lower(name[4])!='a'||
       lower(name[5])!='d'||!slash(name[6]))return false;
    const auto file=name.substr(7);
    if(file.find("..")!=std::string_view::npos||file.find(':')!=std::string_view::npos||
       file.find('/')!=std::string_view::npos||file.find('\\')!=std::string_view::npos)return false;
    const auto extension=file.substr(file.size()-4);
    return extension[0]=='.'&&lower(extension[1])=='p'&&lower(extension[2])=='3'&&lower(extension[3])=='d';
}
constexpr bool Section(bool earthRoadOwner,bool alphaBlend,bool onSurface,bool textured,bool hidden)
{
    return earthRoadOwner&&alphaBlend&&onSurface&&textured&&!hidden;
}
constexpr bool CanShade(bool section,bool textureUploaded,bool materialOpaque)
{
    return section&&textureUploaded&&materialOpaque;
}
constexpr float AlphaReference=0.5f;
constexpr float SurfaceLiftMetres=0.02f;
} // namespace Poseidon::EarthRoadCoverage
