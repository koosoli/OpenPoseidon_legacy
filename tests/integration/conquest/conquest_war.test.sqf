// Conquest: the Everon war from its own mission folder, both sides on the AI. The setup screen's Start
// (conquestStart) begins it, each side's ten starting squads deploy at its HQ, and minutes later both
// sides still hold ground and field soldiers. conquestStats = [started, ended, sides, squads,
// [soldiers alive per side], [objectives held per side]].
triSetLanguage "English"
triSimFrames 30

// WEST, leading a squad, contacts fog, no command view (the AI keeps both sides), no Resistance,
// no village target, 800 supplies each, no time limit, no ally, support from the start.
_ok = conquestStart ["WEST", "soldier", "contacts", "noview", [], false, 0, [800, 800], 0, false, 0]
triAssertEq [_ok, true]
triSimFrames 120

_s = conquestStats
triAssertEq [(_s select 0), true]
triAssertEq [(_s select 2), 2]
triAssertGe [(_s select 3), 20] // ten squads a side, and the AI buys at once
triAssertGt [((_s select 4) select 0), 20]
triAssertGt [((_s select 4) select 1), 20]

// Four minutes of war at four times speed.
setAccTime 4
triSimFrames 3600
setAccTime 1

_s = conquestStats
triAssertGt [((_s select 4) select 0), 10]
triAssertGt [((_s select 4) select 1), 10]
triAssertGe [((_s select 5) select 0), 1]
triAssertGe [((_s select 5) select 1), 1]
triEndTest
