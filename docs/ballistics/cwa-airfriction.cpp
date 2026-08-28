// Per-ammo drag data for classic ARMA: Cold War Assault content.
//
// Overlay fragment -- merge into the config the game loads, do not replace it:
//
//   PoseidonTools.exe config merge <game>/BIN/CONFIG.BIN docs/ballistics/cwa-airfriction.cpp \
//                                  -o <game>/BIN/CONFIG.BIN
//
// Back up CONFIG.BIN first. It lives in the game install, which is not under version control.
//
// TWO KINDS OF ENTRY LIVE HERE, and the difference matters more than any single number.
//
//  * The four SMALL-ARMS classes declare a `dragModel` and a `ballisticCoefficient`. That is the
//    real exterior-ballistics model: a BRL/McCoy standard drag function whose Cd varies with Mach
//    number, driven by a PUBLISHED coefficient for the cartridge. `airFriction` is still given for
//    them, but only as the fallback for a build that ignores the drag function -- it is derived
//    from the drag-function trajectory (least squares on velocity over 0-600 m), not guessed.
//  * Everything else keeps a CONSTANT coefficient only, because the standard projectiles G1 and G7
//    describe rifle bullets and nothing else. An APFSDS long rod, a HEAT shell and a 40 mm grenade
//    are not blunt-nosed or boat-tail spitzers at any scale, so quoting a BC for them would be
//    inventing data with a citation attached. Those values are one-point fits (`k = ln(v/v0)/x` at
//    300 m) and their anchors are UNSOURCED -- see the header on that block.
//
// Ballistic coefficients are in lb/in^2, the convention every published figure uses; the engine
// converts once on load (`Ballistics::BallisticCoefficientToSI`).
//
// Only base classes appear here. CWA's CfgAmmo is a clean inheritance tree, so these 11 entries
// reach all 82 classes. README.md next to this file has the derivation and the sources.

// NO CLASS BELOW NAMES ITS PARENT, and that is required, not sloppy. The config parser drops a
// class whose base is not present in the SAME file, and an overlay never carries the tree it
// patches -- writing `class BulletSingle: Default` here makes the whole overlay parse to an empty
// CfgAmmo and the merge silently does nothing. Omitting the base is the only form that works, and
// `ParamClass::Update` now leaves the existing base alone when the overlay does not name one.
//
// That behaviour is load-bearing and was wrong until 2026-08-27: merging this file used to leave
// twelve classes re-rooted, `class BulletSniper: BulletSingle` reduced to `class BulletSniper`,
// losing every inherited property and every child. If you are on an older build, DO NOT merge this
// -- check with `config debin` that the parents survived before you launch the game.
//
// The merge also needs `--force-overwrite`: retail config classes carry an access lock, and without
// it the merge is a silent no-op that writes a byte-identical file.
//
//   PoseidonTools.exe config merge <game>/BIN/CONFIG.BIN docs/ballistics/cwa-airfriction.cpp \
//                                  -o <game>/BIN/CONFIG.BIN --force-overwrite

class CfgAmmo
{
	// ---- small arms: sourced drag functions --------------------------------------------------
	//
	// Coefficients cross-checked 2026-08-27 against the ammunition library shipped with
	// gehtsoft-usa/BallisticCalculator1 (a JBM/McCoy-derived calculator), whose entries carry
	// their own provenance. Our G1/G7 tables are byte-identical to that library's, and our
	// retardation constant agrees with its PIR to six significant figures, so a coefficient taken
	// from there describes the same trajectory in both.

	// 5.56x45 M855, 940 m/s. G7 0.151 is the standard published figure for the 62 gr boat-tail
	// ball, and it cross-validates against G1 0.304 to 0.3 % at 300 m (see drag-functions/SOURCE.md
	// and the regression test) -- two different tables and two different coefficients landing on
	// one trajectory, which is the check that catches a bad normalisation.
	// Retained velocity: 641 m/s at 300 m, 402 m/s at 600 m.
	class BulletSingle
	{
		// 62 gr = 4.0 g, 5.56 mm. Published and exact, unlike the material
		// resistances they feed: those are estimates, the projectile is not.
		bulletMass = 0.004;
		caliber = 0.00556;
		dragModel = "G7";
		ballisticCoefficient = 0.151;
		airFriction = -0.001328; // fallback only; was -0.000983, fitted to an unsourced anchor
	};
	// 7.62x51 M80 ball, 810 m/s. G1 0.397, source BPN via the reference library.
	// CWA's sniper class stands in for the whole 7.62 NATO family; a match round (M118LR) would be
	// nearer G7 0.268 and is the variant to author if that distinction is ever wanted.
	// Retained velocity: 598 m/s at 300 m, 426 m/s at 600 m.
	class BulletSniper
	{
		// 147 gr = 9.5 g, 7.62 mm.
		bulletMass = 0.0095;
		caliber = 0.00762;
		dragModel = "G1";
		ballisticCoefficient = 0.397;
		airFriction = -0.001039; // fallback only; was -0.000600
	};
	// 7.62x54R belt-fed ball, 830 m/s. G1 0.426, source Federal 146 gr FMJ via the reference
	// library (the GRAU 7N1 sniper load measures 0.430 -- the two agree, which is reassuring).
	// Retained velocity: 628 m/s at 300 m, 460 m/s at 600 m.
	class Bullet7_6
	{
		// 148 gr = 9.6 g, 7.62 mm.
		bulletMass = 0.0096;
		caliber = 0.00762;
		dragModel = "G1";
		ballisticCoefficient = 0.426;
		airFriction = -0.000954; // fallback only; was -0.000616
	};
	// 12.7x99 M2 ball, 890 m/s. G1 0.670, source US Mil via the reference library.
	// Retained velocity: 752 m/s at 300 m, 628 m/s at 600 m -- the heavy round really does hold
	// speed like that, and the old -0.000314 made it hold rather more.
	class Bullet12_7
	{
		// 647 gr = 41.9 g, 12.7 mm.
		bulletMass = 0.0419;
		caliber = 0.0127;
		dragModel = "G1";
		ballisticCoefficient = 0.670;
		airFriction = -0.000570; // fallback only; was -0.000314
	};

	// ---- the inheritance boundary ------------------------------------------------------------
	//
	// MEASURED IN THE RETAIL CONFIG, and it is not obvious from the class names: CWA's autocannon
	// rounds descend from the rifle round.
	//
	//     Bullet4x20 / Bullet4x23 / Bullet30  ->  ExplosiveBullet  ->  BulletSingle
	//
	// A drag function is inherited like any other property, and it OVERRIDES airFriction wherever it
	// arrives (Ballistics::DragRetardation ignores the constant once a model and a coefficient are
	// present). So declaring G7 + BC 0.151 on BulletSingle -- correct for a 5.56 -- would silently
	// make every 20 mm, 23 mm and 30 mm HEI round fly as a 62-grain bullet, and their own carefully
	// ordered airFriction values below would never be read at all.
	//
	// This entry stops the function at that boundary. "constant" is the parser's explicit way of
	// saying no drag function (Ballistics.cpp), and the zero coefficient makes it true twice over.
	// airFriction is deliberately not set here: each of the three children sets its own.
	//
	// The shell and HEAT branch needs no such guard -- Shell and Heat descend from Default, not from
	// BulletSingle, so nothing reaches them.
	class ExplosiveBullet
	{
		dragModel = "constant";
		ballisticCoefficient = 0;
	};

	// ---- everything below: CONSTANT drag, UNSOURCED anchors ----------------------------------
	//
	// Read this before quoting any number in this block. Each is `k = ln(v/v0)/x` at x = 300 m
	// applied to a retained-velocity figure that was NOT taken from a document. They are reasoned
	// estimates, correctly ORDERED by calibre and far better than the one shared constant the
	// engine used for twenty-five years -- but they are not measurements, and the small-arms block
	// above showed what that costs: every one of those four anchors was too optimistic, by 35 % in
	// drag for the 5.56 and by 80 % for the 12.7.
	//
	// Expect the same bias here. Fixing it needs published retained-velocity data per round, which
	// is exactly what is scarce for autocannon and tank ammunition. Until someone has it, these
	// stay flagged rather than quietly promoted.

	// ---- autocannon ---------------------------------------------------------------------
	// 20x102 HEI: 1030 -> 900 m/s at 300 m. UNSOURCED anchor.
	class Bullet4x20
	{
		airFriction = -0.000450;
	};
	// 23x152 HEI: 980 -> 850 m/s at 300 m. UNSOURCED anchor.
	class Bullet4x23
	{
		airFriction = -0.000474;
	};
	// 30x173 HEI: 990 -> 870 m/s at 300 m. UNSOURCED anchor.
	class Bullet30
	{
		airFriction = -0.000431;
	};

	// ---- tank and recoilless ------------------------------------------------------------
	// 73mm OG-15V, low-pressure gun: 400 -> 370 m/s at 300 m. UNSOURCED anchor.
	class Shell73
	{
		airFriction = -0.000260;
	};
	// 105mm APFSDS long rod: 1450 -> 1390 m/s at 300 m. Nearly flat. UNSOURCED anchor.
	class Shell105
	{
		airFriction = -0.000141;
	};
	// 125mm APFSDS: 1700 -> 1630 m/s at 300 m. UNSOURCED anchor.
	class Shell125
	{
		airFriction = -0.000140;
	};
	// HEAT, shaped charge, all calibres: 900 -> 850 m/s at 300 m. UNSOURCED anchor.
	class Heat
	{
		airFriction = -0.000191;
	};
};
