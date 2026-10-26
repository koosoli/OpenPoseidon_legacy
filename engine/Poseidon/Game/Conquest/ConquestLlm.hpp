// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Conquest's optional LLM commander, "Hybrid" mode (docs/roadmap/conquest/CONQUEST-PLAN.md §4.5).
//
// The model never moves a squad. Every few minutes, and soon after a capture, it reads a report of
// what its side KNOWS (never ground truth) and answers with priorities over the objectives, a
// posture and perhaps a purchase. The Standard commander folds those into its own planning and does
// every assignment and everything time-critical. A slow, failing or silly model therefore cannot
// stall the war: on a timeout, an error or an answer that does not parse, the Standard commander
// simply keeps going, as it always does.
//
// It talks through NPC chat's providers, keys and settings (Dev/NpcChat): the same LLM the player
// configured for talking to the squad. Requests run on a background thread; the game thread polls.

#include <string>
#include <utility>
#include <vector>

namespace Poseidon::Conquest::Llm
{
struct Advice
{
    std::vector<std::pair<std::string, float>> priorities; // objective name -> weight (-2 .. +3)
    std::string posture;                                   // "attack", "balanced" or "defend"
    std::string buy;                                       // a squad type the side should buy next, or ""
    std::string barrage;                                   // an objective whose known enemies the mortars should shell, or ""
    std::string reason;                                    // one sentence, for the war log and HQ
    std::string radio;                                     // the situation report HQ radios to the side's soldiers
};

/// The commander's instructions and the JSON it must answer with.
std::string SystemPrompt();

/// Pure: read the model's answer (tolerates prose and ```json fences around the object). Objectives
/// and squad types not in the lists are dropped, weights clamped to -2..3, an unknown posture becomes
/// "balanced". False (with `error`) only when no JSON object is found or nothing usable is in it.
bool ParseAdvice(const std::string& text, const std::vector<std::string>& objectives,
                 const std::vector<std::string>& squadTypes, Advice& out, std::string& error);

/// Ask for advice for `side` with `report` (one request in flight per side; returns false when one
/// is already running or the LLM is not configured, with the reason in `why`).
bool Ask(int side, const std::string& report, const std::vector<std::string>& objectives,
         const std::vector<std::string>& squadTypes, std::string* why = nullptr);
bool InFlight(int side);

struct Answer
{
    bool ok = false;
    Advice advice;
    std::string error;
    double seconds = 0.0; // how long the model took
    std::string note;     // the configured model could not chat; the provider's default answered instead
};
/// The finished answer for `side`, once (game thread).
bool TakeAnswer(int side, Answer& answer);

/// Abandon every request in flight (mission end, shutdown).
void CancelAll();
} // namespace Poseidon::Conquest::Llm
