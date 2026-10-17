// Swim moves (Sinkhole W3, SWIM-W3-001) -- the @OP_Swim default content pack, archive swim.pbo, prefix "swim".
// The states use the action maps SwimActions / SwimForwardActions / SwimDeadActions (the engine counts any state
// using one of them as swimming) and SwimOutActions (standing up in the shallows); FallActions / FallLandActions
// drive falling and the touchdown. Engine: SoldierOldSwim.cpp.
// Animations: rtm/ (see README.md for their provenance and the feet/shoulder repair); built by
// scripts/Build-Swim.ps1. Without this pack, soldiers keep the old behaviour (they walk the sea bed).
class CfgPatches
{
	class swim
	{
		units[] = {};
		weapons[] = {};
		requiredAddons[] = {};
	};
};
class CfgMovesMC
{
	class Actions
	{
		class NoActions;
		// treading water and the moves that stay upright
		class SwimActions: NoActions
		{
			Stop="Swim";
			StopRelaxed="Swim";
			TurnL="SwimTurnL";
			TurnR="SwimTurnR";
			TurnLRelaxed="SwimTurnL";
			TurnRRelaxed="SwimTurnR";
			WalkF="SwimSlowF";
			WalkLF="SwimLF";
			WalkRF="SwimRF";
			WalkL="SwimL";
			WalkR="SwimR";
			WalkLB="SwimLB";
			WalkRB="SwimRB";
			WalkB="SwimB";
			SlowF="SwimF";
			SlowLF="SwimLF";
			SlowRF="SwimRF";
			SlowL="SwimL";
			SlowR="SwimR";
			SlowLB="SwimLB";
			SlowRB="SwimRB";
			SlowB="SwimB";
			FastF="SwimFastF";
			FastLF="SwimLF";
			FastRF="SwimRF";
			FastL="SwimL";
			FastR="SwimR";
			FastLB="SwimLB";
			FastRB="SwimRB";
			FastB="SwimB";
			Default="Swim";
			Die="SwimDead";
			upDegree="manposstand";
			turnSpeed=1.2;
			limitFast=1.2;
		};
		// the horizontal stroke (dies face down, moving)
		class SwimForwardActions: SwimActions
		{
			Die="SwimDeadF";
		};
		class SwimDeadActions: NoActions
		{
			Stop="SwimDead";
			Default="SwimDead";
			Die="SwimDead";
			upDegree="manposdead";
		};
		// standing up in shallow water; the engine goes back to the normal moves after it
		class SwimOutActions: NoActions
		{
			Stop="SwimOut";
			Default="SwimOut";
			Die="SwimDeadF";
			upDegree="manposstand";
		};
		// under water (W4, diving): the underwater stroke forward, holding still otherwise. The engine enters it with
		// SwimDive and leaves it with SwimSurface (SoldierOldSwim.cpp); Man::IsSwimMove counts this map as swimming
		class SwimUnderActions: NoActions
		{
			Stop="SwimUnderIdle";
			StopRelaxed="SwimUnderIdle";
			TurnL="SwimUnderIdle";
			TurnR="SwimUnderIdle";
			TurnLRelaxed="SwimUnderIdle";
			TurnRRelaxed="SwimUnderIdle";
			WalkF="SwimUnder";
			SlowF="SwimUnder";
			FastF="SwimUnder";
			Default="SwimUnderIdle";
			Die="SwimDeadF";
			upDegree="manposstand";
			turnSpeed=1.0;
			limitFast=1.2;
		};
		// falling (the engine holds it while the man drops; killed in the air he dies as in combat)
		class FallActions: NoActions
		{
			Stop="Fall";
			Default="Fall";
			Die="CombatDying";
			upDegree="manposstand";
		};
		// touchdown on land; the engine goes back to the normal moves after it
		class FallLandActions: NoActions
		{
			Stop="FallLand";
			Default="FallLand";
			Die="CombatDying";
			upDegree="manposstand";
		};
	};



	class StandBase;
	class DefaultDie;
	class States
	{
		// treading water (2.4 s)
		class Swim: StandBase
		{
			actions="SwimActions";
			file="\swim\tread.rtm";
			speed=-2.38;
			looped=1;
			soundEnabled=0;
			duty=-0.2;
			disableWeapons=1;
			disableWeaponsLong=1;
			enableOptics=0;
			showWeaponAim=0;
			aiming="aimingNo";
			legs="legsNo";
			// no head look in the water: the strokes already hold the head up, and the look turn on top
			// (head 1, neck 0.5) bent the neck back past what the skin takes
			head="headNo";
			interpolationSpeed=2;
			connectTo[]={};
			interpolateTo[]={"SwimStart",0.05,"SwimB",0.2,"SwimL",0.2,"SwimR",0.2,"SwimLB",0.2,"SwimRB",0.2,"SwimTurnL",0.1,"SwimTurnR",0.1,"SwimDead",0.1,"SwimF",0.5};
		};
		// upright moves: backwards, sideways, back-diagonals, turning (extra)
		class SwimB: Swim
		{
			file="\swim\swimb.rtm";
			speed=-2.4;
			duty=0.1;
			interpolateTo[]={"Swim",0.1,"SwimL",0.2,"SwimR",0.2,"SwimLB",0.1,"SwimRB",0.1,"SwimTurnL",0.2,"SwimTurnR",0.2,"SwimStart",0.2,"SwimDead",0.1};
		};
		class SwimL: SwimB
		{
			file="\swim\swiml.rtm";
			interpolateTo[]={"Swim",0.1,"SwimB",0.2,"SwimR",0.3,"SwimLB",0.1,"SwimRB",0.3,"SwimTurnL",0.2,"SwimTurnR",0.2,"SwimStart",0.2,"SwimDead",0.1};
		};
		class SwimR: SwimB
		{
			file="\swim\swimr.rtm";
			interpolateTo[]={"Swim",0.1,"SwimB",0.2,"SwimL",0.3,"SwimLB",0.3,"SwimRB",0.1,"SwimTurnL",0.2,"SwimTurnR",0.2,"SwimStart",0.2,"SwimDead",0.1};
		};
		class SwimLB: SwimB
		{
			file="\swim\swimlb.rtm";
			interpolateTo[]={"Swim",0.1,"SwimB",0.1,"SwimL",0.1,"SwimR",0.3,"SwimRB",0.2,"SwimTurnL",0.2,"SwimTurnR",0.2,"SwimStart",0.2,"SwimDead",0.1};
		};
		class SwimRB: SwimB
		{
			file="\swim\swimrb.rtm";
			interpolateTo[]={"Swim",0.1,"SwimB",0.1,"SwimR",0.1,"SwimL",0.3,"SwimLB",0.2,"SwimTurnL",0.2,"SwimTurnR",0.2,"SwimStart",0.2,"SwimDead",0.1};
		};
		class SwimTurnL: SwimB
		{
			file="\swim\swimturnl.rtm";
			duty=-0.1;
			interpolateTo[]={"Swim",0.1,"SwimTurnR",0.1,"SwimB",0.2,"SwimL",0.2,"SwimR",0.2,"SwimLB",0.2,"SwimRB",0.2,"SwimStart",0.1,"SwimDead",0.1};
		};
		class SwimTurnR: SwimTurnL
		{
			file="\swim\swimturnr.rtm";
			interpolateTo[]={"Swim",0.1,"SwimTurnL",0.1,"SwimB",0.2,"SwimL",0.2,"SwimR",0.2,"SwimLB",0.2,"SwimRB",0.2,"SwimStart",0.1,"SwimDead",0.1};
		};
		// tread -> stroke
		class SwimStart: Swim
		{
			actions="SwimForwardActions";
			file="\swim\swimstart.rtm";
			speed=-1.83;
			looped=0;
			duty=0.2;
			interpolateTo[]={"SwimDeadF",0.1};
			connectTo[]={"SwimF",0.05,"SwimSlowF",0.05,"SwimFastF",0.05,"SwimLF",0.05,"SwimRF",0.05};
		};
		// the breaststroke (1.43 s, 1.5 m per stroke)
		class SwimF: Swim
		{
			actions="SwimForwardActions";
			file="\swim\swim.rtm";
			speed=-1.43;
			duty=0.1;
			relSpeedMin=0.7;
			relSpeedMax=1.1;
			interpolateTo[]={"SwimStop",0.05,"SwimSlowF",0.1,"SwimFastF",0.1,"SwimLF",0.1,"SwimRF",0.1,"SwimDeadF",0.1};
		};
		// the same stroke slower (walk speed); the sprint is the front crawl (heads-up, both arms in turn)
		class SwimSlowF: SwimF
		{
			speed=-2.4;
			duty=-0.1;
			relSpeedMin=0.5;
			relSpeedMax=0.8;
			interpolateTo[]={"SwimStop",0.05,"SwimF",0.1,"SwimFastF",0.1,"SwimLF",0.1,"SwimRF",0.1,"SwimDeadF",0.1};
		};
		class SwimFastF: SwimF
		{
			file="\swim\swimfast.rtm";
			speed=-1.1;
			duty=0.6;
			relSpeedMin=0.8;
			relSpeedMax=1.0;
			interpolateTo[]={"SwimStop",0.05,"SwimF",0.1,"SwimSlowF",0.1,"SwimLF",0.1,"SwimRF",0.1,"SwimDeadF",0.1};
		};
		// forward diagonals: the stroke angled into the direction of travel (extra)
		class SwimLF: SwimF
		{
			file="\swim\swimlf.rtm";
			speed=-1.6;
			interpolateTo[]={"SwimStop",0.05,"SwimF",0.1,"SwimSlowF",0.1,"SwimFastF",0.1,"SwimRF",0.2,"SwimDeadF",0.1};
		};
		class SwimRF: SwimLF
		{
			file="\swim\swimrf.rtm";
			interpolateTo[]={"SwimStop",0.05,"SwimF",0.1,"SwimSlowF",0.1,"SwimFastF",0.1,"SwimLF",0.2,"SwimDeadF",0.1};
		};
		// stroke -> tread
		class SwimStop: Swim
		{
			file="\swim\swimstop.rtm";
			speed=-1.1;
			looped=0;
			interpolateTo[]={"SwimDead",0.1};
			connectTo[]={"Swim",0.05,"SwimB",0.05,"SwimL",0.05,"SwimR",0.05,"SwimLB",0.05,"SwimRB",0.05,"SwimTurnL",0.05,"SwimTurnR",0.05};
		};
		// swimming into shallow water: stands up (swim -> stand); the
		// engine plays it when the water gets shallow and then returns to the
		// normal moves
		class SwimOut: Swim
		{
			actions="SwimOutActions";
			file="\swim\swimout.rtm";
			speed=-1.4;
			looped=0;
			interpolateTo[]={};
			connectTo[]={};
		};
		// fallen in from a height: the fall pose into treading, the rifle from the hands to slung (the
		// engine lets the body plunge and bob up meanwhile)
		class SwimIn: Swim
		{
			file="\swim\swimin.rtm";
			speed=-1.1;
			looped=0;
			interpolateTo[]={"SwimDead",0.1};
			connectTo[]={"Swim",0.05,"SwimB",0.05,"SwimL",0.05,"SwimR",0.05,"SwimLB",0.05,"SwimRB",0.05,"SwimTurnL",0.05,"SwimTurnR",0.05,"SwimStart",0.05};
		};
		// diving (W4; made in Blender, scripts/swim/dive_moves.py). The engine blends to SwimDive on the player's
		// move-down key and to SwimSurface at the surface (BlendToMove); no swim state leads to them by itself.
		// surface dive from treading: lean onto the water, pike at the hips, the legs up overhead, level into the glide (1.6 s)
		class SwimDive: Swim
		{
			actions="SwimUnderActions";
			file="\swim\swimdive.rtm";
			speed=-1.6;
			looped=0;
			duty=0.3;
			interpolateTo[]={"SwimDeadF",0.1};
			connectTo[]={"SwimUnder",0.05,"SwimUnderIdle",0.05,"SwimSurface",0.1};
		};
		// the underwater breaststroke: long pull to the thighs, glide, frog kick (2.2 s, 1.6 m a stroke)
		class SwimUnder: Swim
		{
			actions="SwimUnderActions";
			file="\swim\swimunder.rtm";
			speed=-2.2;
			duty=0.3;
			relSpeedMin=0.7;
			relSpeedMax=1.1;
			interpolateTo[]={"SwimUnderIdle",0.2,"SwimSurface",0.1,"SwimDeadF",0.1};
		};
		// holding still under water: flat, sculling, an easy kick (2.4 s)
		class SwimUnderIdle: Swim
		{
			actions="SwimUnderActions";
			file="\swim\swimunderidle.rtm";
			speed=-2.4;
			duty=0.0;
			interpolateTo[]={"SwimUnder",0.2,"SwimSurface",0.1,"SwimDeadF",0.1};
		};
		// up from the glide into treading: head and chest lift, a pull down and a kick (1.3 s)
		class SwimSurface: Swim
		{
			file="\swim\swimsurface.rtm";
			speed=-1.3;
			looped=0;
			duty=0.3;
			interpolateTo[]={"SwimDead",0.1};
			connectTo[]={"Swim",0.05,"SwimStart",0.05,"SwimB",0.05,"SwimL",0.05,"SwimR",0.05,"SwimTurnL",0.05,"SwimTurnR",0.05};
		};
		// falling: upright, the rifle held lowered in both hands, legs loose (the engine plays it once a man has
		// been dropping for a moment, keeps him upright, and leaves it when he lands or reaches the water)
		class Fall: StandBase
		{
			actions="FallActions";
			file="\swim\fall.rtm";
			speed=-1.6;
			looped=1;
			soundEnabled=0;
			disableWeapons=1;
			disableWeaponsLong=1;
			enableOptics=0;
			showWeaponAim=0;
			aiming="aimingNo";
			legs="legsNo";
			interpolationSpeed=4;
			connectTo[]={};
			interpolateTo[]={"CombatDying",0.1};
		};
		// touchdown: down into a crouch with the feet planted, up into the relaxed stance
		class FallLand: Fall
		{
			actions="FallLandActions";
			file="\swim\fallland.rtm";
			speed=-1.4;
			looped=0;
			interpolationSpeed=6;
		};
		// killed while treading water / while swimming: goes limp and floats face down
		class SwimDead: DefaultDie
		{
			actions="SwimDeadActions";
			file="\swim\swimdead.rtm";
			speed=-3.3;
			looped=0;
			terminal=1;
			soundEnabled=0;
			soundOverride="";
			interpolationSpeed=2;
		};
		class SwimDeadF: SwimDead
		{
			file="\swim\swimdeadf.rtm";
		};
	};
};
