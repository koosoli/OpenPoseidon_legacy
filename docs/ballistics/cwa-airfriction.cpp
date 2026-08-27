// Per-ammo drag coefficients for classic ARMA: Cold War Assault content.
//
// Overlay fragment -- merge into the config the game loads, do not replace it:
//
//   PoseidonTools.exe config merge <game>/BIN/CONFIG.BIN docs/ballistics/cwa-airfriction.cpp \
//                                  -o <game>/BIN/CONFIG.BIN
//
// Back up CONFIG.BIN first. It lives in the game install, which is not under version control.
//
// Every value is `k = ln(v / v0) / x` applied to one published retained-velocity figure for the
// cartridge the class stands in for, at x = 300 m. That is the exact inverse of the engine's drag
// law (`accel = speed * |speed| * airFriction`), so there is no fitting and no free parameter.
// The derivation, the source figures and the honest limits are in README.md next to this file --
// in particular: this is a CONSTANT drag coefficient, not a G1 or G7 drag function, so it cannot
// track the transonic rise. Fitted at 300 m, it is closest where infantry actually fight.
//
// Only base classes appear here. CWA's CfgAmmo is a clean inheritance tree, so these 11 entries
// reach all 82 classes.

class CfgAmmo
{
	// ---- small arms ---------------------------------------------------------------------
	// 5.56x45 M855: 940 -> 700 m/s at 300 m. The draggiest thing on the battlefield per metre,
	// which is exactly what one shared constant used to hide.
	class BulletSingle
	{
		airFriction = -0.000983;
	};
	// 7.62x51 M118: 838 -> 700 m/s at 300 m. Closest to the engine's old universal -0.0005,
	// which is why that value never looked obviously wrong.
	class BulletSniper
	{
		airFriction = -0.000600;
	};
	// 7.62x54R, belt-fed: 830 -> 690 m/s at 300 m.
	class Bullet7_6
	{
		airFriction = -0.000616;
	};
	// 12.7x99 M33: 890 -> 810 m/s at 300 m. Heavy, sectionally dense, sheds speed slowly.
	class Bullet12_7
	{
		airFriction = -0.000314;
	};

	// ---- autocannon ---------------------------------------------------------------------
	// 20x102 HEI: 1030 -> 900 m/s at 300 m.
	class Bullet4x20
	{
		airFriction = -0.000450;
	};
	// 23x152 HEI: 980 -> 850 m/s at 300 m.
	class Bullet4x23
	{
		airFriction = -0.000474;
	};
	// 30x173 HEI: 990 -> 870 m/s at 300 m.
	class Bullet30
	{
		airFriction = -0.000431;
	};

	// ---- tank and recoilless ------------------------------------------------------------
	// The four below are the softest numbers here: tank retained-velocity data is published far
	// less consistently than small arms, and CWA's shells are generic stand-ins rather than
	// specific rounds. Reasoned estimates, not measurements. Said plainly so nobody cites them.
	//
	// 73mm OG-15V, low-pressure gun: 400 -> 370 m/s at 300 m.
	class Shell73
	{
		airFriction = -0.000260;
	};
	// 105mm APFSDS long rod: 1450 -> 1390 m/s at 300 m. Nearly flat.
	class Shell105
	{
		airFriction = -0.000141;
	};
	// 125mm APFSDS: 1700 -> 1630 m/s at 300 m.
	class Shell125
	{
		airFriction = -0.000140;
	};
	// HEAT, shaped charge, all calibres: 900 -> 850 m/s at 300 m.
	class Heat
	{
		airFriction = -0.000191;
	};
};
