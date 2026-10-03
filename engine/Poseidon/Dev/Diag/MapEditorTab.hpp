#pragma once
#include <cstdint>
namespace Poseidon::Dev
{
enum class MapEditorAction
{
    None,
    StartPainting,
    RestoreTerrain,
    StartCave,
    UndoCave,
    RotateCave,
    DeleteCave
};
MapEditorAction DrawMapEditorTab();
std::uint32_t MapEditorActionId();
float MapEditorActionHeading();
} // namespace Poseidon::Dev
