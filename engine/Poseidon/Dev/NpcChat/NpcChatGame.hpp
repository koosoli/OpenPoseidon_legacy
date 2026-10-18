#pragma once

// Game-thread side of the NPC chat: what the model is told (the situation report), who a
// speaker id refers to, and turning validated orders into the game's own SQF order commands.
// Everything here must run on the game thread.

#include <Poseidon/Dev/NpcChat/NpcChatReply.hpp>
#include <Poseidon/Dev/NpcChat/NpcChatSettings.hpp>

#include <string>
#include <vector>

namespace Poseidon::NpcChat
{
struct ReportResult
{
    bool ok = false;
    std::string error;
    std::string text;           // the situation report (+ briefing)
    std::string defaultSpeaker; // who answers when the model names nobody
    int registryId = 0;         // ids (P#/E#/V#) resolve against this report
};

/// Snapshot the world for one request. Keeps the id registry until ReleaseRegistry().
ReportResult BuildSituationReport(const Settings& settings);
void ReleaseRegistry(int registryId);

struct SpeakerInfo
{
    bool valid = false;
    bool isHq = false;
    bool inSquad = false;
    std::string id;
    std::string name;      // "Pvt. Miller", "Command"
    float distance = 0.0f; // from the player, metres
    float x = 0, y = 0, z = 0;
    int voiceSeed = 0; // stable per person, picks a voice from the pool
};

/// Resolve "2", "P1", "HQ" or a soldier's name.
SpeakerInfo ResolveSpeaker(int registryId, const std::string& id);
/// Current position of a speaker (for 3D speech as they move); false when gone.
bool SpeakerPosition(int registryId, const std::string& id, float& x, float& y, float& z);

/// Execute validated orders. Returns one human-readable line per order ("2, 3: follow").
std::vector<std::string> ExecuteOrders(int registryId, const std::vector<Order>& orders);

/// Push a line into the game's own chat list (bottom-left), coloured by channel.
void PostChatLine(int registryId, const std::string& speakerId, const std::string& text, bool fromPlayer);

/// Single-player mission running and a player body to talk from.
bool GameAvailable(std::string* why = nullptr);

/// Identifies the running mission; the conversation resets when it changes. Empty without one.
std::string MissionKey();

/// Player-relative listener info for the audio path.
bool PlayerPosition(float& x, float& y, float& z);

struct SquadSlot
{
    int id = 0;
    std::string name;
    std::string role;
    bool isPlayer = false;
    bool dead = false;
};
/// The player's squad right now (for the call-sign editor); empty without a mission.
std::vector<SquadSlot> CurrentSquad();

/// Strip mission briefing HTML to plain text (pure; unit-tested).
std::string HtmlToText(const std::string& html);
} // namespace Poseidon::NpcChat
