#pragma once
namespace Poseidon::Dev
{
enum class MapEditorAction { None, StartPainting, RestoreTerrain, StartCave, UndoCave };
MapEditorAction DrawMapEditorTab();
}
