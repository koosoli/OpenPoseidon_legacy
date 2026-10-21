#pragma once

// The contract between the game and the language model.
//
// The model answers with one JSON object:
//   {"say":   [{"speaker": "2", "text": "Two, roger. Moving."}],
//    "orders":[{"to": ["2","3"] | "all", "order": "move",
//               "where": {"ref": "look"|"player"|"unit"|"target"|"vehicle", "id": "3",
//                         "bearing": 90, "distance": 100},
//               "target": "E1", "value": "DOWN"}]}
//
// Speakers are the ids the situation report hands out: squad numbers ("2".."12"), people near
// the player who are not in the squad ("P1".."P8"), and "HQ" (higher command over the radio).
// Orders are whitelisted below and only ever executed for the player's own squad while the
// player leads it -- nothing the model writes is evaluated as script.
//
// Pure (no engine state): unit-tested.

#include <cmath>
#include <string>
#include <vector>

namespace Poseidon::NpcChat
{
struct SpokenLine
{
    std::string speaker;
    std::string text;
};

enum class OrderKind
{
    Follow,     // "follow me", "regroup", "form on me"
    Hold,       // "stay here", "hold position", "stop"
    Move,       // "move to ..."  (where)
    Cover,      // "cover me": hold and watch where the player is looking (or `where`)
    Watch,      // "watch north" (where)
    Engage,     // "engage E1" / "kill P2" (target: a contact or a person nearby)
    Fire,       // "fire!", "shoot into the air": fire the weapon now (where: optional aim point)
    Stance,     // value UP | MIDDLE | DOWN | AUTO
    CombatMode, // value BLUE (never fire) | GREEN (hold fire) | WHITE | YELLOW (fire at will) | RED (engage at will)
    Behaviour,  // value CARELESS | SAFE | AWARE | COMBAT | STEALTH  (whole group)
    Formation,  // value COLUMN | STAG COLUMN | WEDGE | ECH LEFT | ECH RIGHT | VEE | LINE  (whole group)
    Speed,      // value LIMITED | NORMAL | FULL  (whole group)
    GetIn,      // board vehicle (target "V1")
    GetOut,     // dismount
    LineUp,     // "line up", "stand in a line": a real line of soldiers, not the formation (where: optional centre)
    Invalid
};

struct Where
{
    std::string ref; // look | player | unit | target | vehicle | "" (none)
    std::string id;  // for unit/target/vehicle
    float bearing = NAN;  // degrees, 0 = north; relative to the reference point
    float distance = NAN; // metres
    bool Present() const { return !ref.empty() || !std::isnan(bearing); }
};

struct Order
{
    std::vector<std::string> to; // squad numbers
    bool all = false;
    OrderKind kind = OrderKind::Invalid;
    std::string rawKind;
    Where where;
    std::string target; // E# / V#
    std::string value;  // upper-cased, validated against the kind's whitelist
};

struct Reply
{
    std::vector<SpokenLine> lines;
    std::vector<Order> orders;
    std::vector<std::string> rejected; // orders that failed validation (shown in the transcript)
    bool truncated = false;            // the JSON stopped early: lines recovered, orders lost
    bool classifier = false;           // a moderation verdict ("User Safety: unsafe"), not dialogue
};

/// Tolerant: accepts the object wrapped in prose or ```json fences; plain prose becomes one
/// line spoken by `fallbackSpeaker`. Returns false only for empty input.
bool ParseReply(const std::string& text, const std::string& fallbackSpeaker, Reply& out);

OrderKind ParseOrderKind(const std::string& name);
const char* OrderKindName(OrderKind kind);
/// Normalises and validates `value` for the kind (e.g. "prone" -> "DOWN"); empty when invalid.
std::string NormaliseOrderValue(OrderKind kind, const std::string& value);

/// One squad member as the addressing rules see him.
struct SquadCandidate
{
    int id = 0;
    std::string name;     // "James Pound"
    std::string role;     // "Grenadier"
    std::string callsign; // optional, from the panel
};
/// Who "James Pound" / "Pound" / "grenadier" / "Hammer" / "granadier" (a speech-to-text slip)
/// means. Order: call sign, full name, first/last name, role, part of a name, then near-misses
/// (edit distance 1 for short words, 2 for long ones). 0 = nobody.
int MatchSquadMember(const std::vector<SquadCandidate>& squad, const std::string& query);

/// Appended after the player's (editable) system prompt: the JSON contract and the order list.
std::string ResponseFormatInstructions(bool allowOrders);
} // namespace Poseidon::NpcChat
