#pragma once

// UI for the NPC chat: the dev-panel tab (NpcChatUi.cpp: settings, keys, transcript) and the
// in-game overlay (NpcChatOverlay.cpp: chat box, recent lines, "listening") shown with the panel closed.

union SDL_Event;

namespace Poseidon::NpcChat
{
struct TranscriptLine;

/// Dev-panel tab body. Call between ImGui::BeginTabItem / EndTabItem.
void DrawTab();
/// "Compact: chat only": the dev panel shows just the chat window until "Full dev panel".
bool CompactPanel();
void DrawCompactPanel();
/// Overlay; call every frame inside the ImGui frame (panel open or closed).
void DrawOverlay(bool panelVisible);
/// Hotkeys (open chat, hold to talk, key binding). Returns true when the event was consumed and
/// must not reach the game.
bool ProcessEvent(const SDL_Event& event);
/// The chat box has keyboard focus: the game must not see typed keys.
bool WantsKeyboard();

// Shared between the tab and the overlay (NpcChatOverlay.cpp).
void DrawTranscriptLine(const TranscriptLine& line, float alpha = 1.0f);
/// "Press a key" rebinding: the next key-down is written to *key (Esc cancels).
void ArmKeyBinding(int* key);
bool KeyBindingArmed(const int* key);
} // namespace Poseidon::NpcChat
