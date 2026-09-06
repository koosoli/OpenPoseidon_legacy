// Café furniture that reacts to fire: tables, stove, radio and desk as Things.
//
// Overlay fragment -- merge into the config the game loads, do not replace it:
//
//   PoseidonTools.exe config merge <game>/BIN/CONFIG.BIN docs/gameplay/cwa-cafe-furniture.cpp \
//                                  -o <game>/BIN/CONFIG.BIN --force-overwrite
//
// Back up CONFIG.BIN first; `--force-overwrite` is not optional (see cwa-grenade-fuse.cpp).
//
// WHY
//
// RFG-080 turns the furniture proxies inside map buildings into Things, so a chair in
// the Everon café (hospoda_mesto.p3d, [5093 3996]) can be shot over. It can only do
// that for a model some CfgVehicles class names with simulation="thing": the chairs
// have ChairHangar and ChairSmall, the four models below have no class at all, so
// they stayed part of the wall. These give them one. Table is the retail base: a
// Thing with destructbuilding and no fuel, which is what a wooden table is.
//
// Model names are bare, as retail writes them (GetShapeName prepends data3d\).
class CfgVehicles
{
	class Table;
	class TableHospoda: Table
	{
		model="stul_hospoda.p3d";
		displayName="$STR_DN_TABLE";
	};
	class StoveCoal: Table
	{
		model="kamna_uhli.p3d";
		displayName="$STR_DN_UNKNOWN";
	};
	class RadioHangar: Table
	{
		model="hangar_radio.p3d";
		displayName="$STR_DN_UNKNOWN";
		mapSize=0.4;
	};
	class DeskHangar: Table
	{
		model="hangar_psacistul.p3d";
		displayName="$STR_DN_TABLE";
	};
};
