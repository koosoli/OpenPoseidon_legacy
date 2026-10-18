#pragma once

// NPC chat orchestrator: one conversation per mission, one background worker for every network
// call (LLM, speech-to-text, text-to-speech), and the game-thread glue (situation report,
// orders, speech playback, push-to-talk).
//
// Game thread: Update() once per frame, SubmitText(), BeginTalk()/EndTalk(), the UI queries.
// The worker never touches engine state; results come back through a queue drained by Update().

#include <Poseidon/Dev/NpcChat/NpcChatProviders.hpp>

#include <deque>
#include <string>
#include <vector>

namespace Poseidon::NpcChat
{
enum class LineKind
{
    Player,
    Npc,
    Order,
    System,
    Error
};

struct TranscriptLine
{
    LineKind kind = LineKind::System;
    std::string who;
    std::string text;
    double time = 0.0; // seconds since start (SDL ticks)
};

void Update();
void Shutdown();

/// Send what the player typed (or said). Ignored when disabled, rate-limited or busy.
void SubmitText(const std::string& text);

/// Push-to-talk. EndTalk() sends the recording to speech-to-text, then on to the model.
void BeginTalk();
/// Same recording path, but the transcript is only shown ("heard: ...") and never sent.
void BeginSttTest();
void EndTalk(bool cancel = false);
bool IsTalking();
float MicLevel(); // 0..1, for a meter

bool Busy();               // a request is in flight
std::string StatusLine();  // "Thinking...", "Listening...", last error
const std::deque<TranscriptLine>& Transcript();
void ClearConversation();
void StopSpeech();
/// "Stop waiting": abort the request in flight.
void CancelRequest();

/// Send a fixed test prompt (no game state needed) to check the LLM key/model.
void TestLlm();
/// Speak a fixed sentence with the current TTS settings.
void TestVoice();

/// Windows voices (cached; first call enumerates).
const std::vector<std::string>& WindowsVoices();

// ---- connection status and discovery (the panel's "Connected" lights) ----
enum class LinkState
{
    Unknown,  // not checked yet
    Checking, // request in flight
    Ok,
    Failed
};
struct LinkStatus
{
    LinkState state = LinkState::Unknown;
    std::string message;
};
const LinkStatus& LlmStatus();
const LinkStatus& SttStatus();
const LinkStatus& TtsStatus();
/// Validate the key and load the model list (LLM) / voice list (TTS) / a key probe (STT).
/// Results arrive through Update(); a newer check supersedes an older one.
void CheckLlm();
void CheckStt();
void CheckTts();
const std::vector<ModelInfo>& Models();
const std::vector<VoiceInfo>& Voices();
/// Speech models the current STT / TTS provider offers (filled by CheckStt / CheckTts).
const std::vector<ModelInfo>& SttModels();
const std::vector<ModelInfo>& TtsModels();
/// Speak a sample sentence with one specific voice (the voice list's Test button).
void TestVoiceId(const std::string& voiceId);
/// Give every squad mate a different male voice from the loaded list; returns what happened.
std::string AutoAssignMaleVoices();
/// Hand out the voices ticked in Text to speech (the pool) to the squad, in order.
std::string AssignPoolVoices();
/// The conversation as plain text (Copy button).
std::string TranscriptText();
} // namespace Poseidon::NpcChat
