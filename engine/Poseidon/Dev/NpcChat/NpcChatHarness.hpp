#pragma once

// Harness commands for driving the NPC chat without a keyboard (tests, captures):
//   npc_chat_configure {settings: {...}}  merge settings for this run (never saved, never keys)
//   npc_chat_say {text}                   what the player types
//   npc_chat_transcript                   the conversation so far + status
//   npc_chat_report                       the situation report the model would get now

namespace Poseidon::Dev
{
class HarnessServer;
}

namespace Poseidon::NpcChat
{
void RegisterHarness(Poseidon::Dev::HarnessServer& hs);
}
