// A fuse for the hand grenade.
//
// Overlay fragment -- merge into the config the game loads, do not replace it:
//
//   PoseidonTools.exe config merge <game>/BIN/CONFIG.BIN docs/gameplay/cwa-grenade-fuse.cpp \
//                                  -o <game>/BIN/CONFIG.BIN --force-overwrite
//
// Back up CONFIG.BIN first: it lives in the game install, which is not under
// version control. `--force-overwrite` is not optional -- retail config classes
// carry an access lock and without it the merge is a silent no-op that still
// reports success.
//
// WHY THIS IS DATA AND NOT A DEFAULT IN THE ENGINE
//
// A 2001 grenade detonates the instant it touches anything. The engine now
// honours `explosionTime` on any ammo class, but defaults it to zero, and zero
// is that old behaviour -- so an unmodified install is unchanged and every
// existing mission still plays the way it was built.
//
// Which rounds get a fuse has to be decided HERE rather than in code, because
// the class tree does not separate them. `Flare`, `SmokeShell` and `MortarShell`
// all descend from `Grenade`, and any rule an engine could apply ("explosive
// shotShells get a fuse") would sweep them in. Only `GrenadeHand` -- the thing a
// soldier actually throws -- is named below.
//
// THREE SECONDS
//
// The owner's choice, as a compromise between the 2001 instant detonation and
// the four-to-five seconds a real M67 or RGD-5 fuse burns. It is a GAMEPLAY
// number, not a measurement: a grenade you can outrun is a different weapon, and
// CWA's AI does not know to run from one. Try it before treating it as settled;
// the dev panel's Physics tab has a live override so it can be compared against
// 2 s and 5 s without rebuilding or re-merging this file.

class CfgAmmo
{
	class GrenadeHand
	{
		explosionTime=3;
	};
};
