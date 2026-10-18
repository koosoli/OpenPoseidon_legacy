// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/Dev/NpcChat/NpcChatHarness.hpp>

#include <Poseidon/Dev/Harness/HarnessProtocol.hpp>
#include <Poseidon/Dev/Harness/HarnessServer.hpp>
#include <Poseidon/Dev/NpcChat/NpcChat.hpp>
#include <Poseidon/Dev/NpcChat/NpcChatGame.hpp>
#include <Poseidon/Dev/NpcChat/NpcChatSettings.hpp>

#include <cjson/cJSON.h>

#include <cctype>
#include <string>

namespace Poseidon::NpcChat
{
namespace HP = Poseidon::Dev::HarnessProtocol;

void RegisterHarness(Poseidon::Dev::HarnessServer& hs)
{
    hs.RegisterCommand({"npc_chat_configure", "Merge NPC chat settings for this run (not saved; keys refused)",
                        {{"settings", "object", true}}},
                       [](const std::string&, cJSON* root) -> std::string
                       {
                           cJSON* settings = cJSON_GetObjectItemCaseSensitive(root, "settings");
                           if (!cJSON_IsObject(settings))
                               return HP::ErrorResponse("settings object required");
                           for (cJSON* item = settings->child; item; item = item->next)
                           {
                               std::string name = item->string ? item->string : "";
                               const bool allowed = name == "chatKey" || name == "talkKey" || name == "rememberKeys";
                               for (char& c : name)
                                   c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                               if (!allowed && (name.find("key") != std::string::npos ||
                                                name.find("token") != std::string::npos ||
                                                name.find("secret") != std::string::npos))
                                   return HP::ErrorResponse("API keys are never accepted over the harness");
                           }
                           char* text = cJSON_PrintUnformatted(settings);
                           const bool ok = SettingsFromJson(text ? text : "{}", CurrentSettings());
                           cJSON_free(text);
                           return ok ? HP::OkResponse() : HP::ErrorResponse("bad settings");
                       });

    hs.RegisterCommand({"npc_chat_say", "Send a line to the NPC chat as if the player typed it", {{"text", "string", true}}},
                       [](const std::string&, cJSON* root) -> std::string
                       {
                           const char* text = HP::GetString(root, "text");
                           if (!text || !text[0])
                               return HP::ErrorResponse("text required");
                           SubmitText(text);
                           return HP::OkResponse();
                       });

    hs.RegisterCommand({"npc_chat_transcript", "NPC chat transcript and status", {}},
                       [](const std::string&, cJSON*) -> std::string
                       {
                           static const char* kinds[] = {"player", "npc", "order", "system", "error"};
                           cJSON* resp = cJSON_CreateObject();
                           cJSON_AddBoolToObject(resp, "busy", Busy());
                           cJSON_AddStringToObject(resp, "status", StatusLine().c_str());
                           cJSON* lines = cJSON_AddArrayToObject(resp, "lines");
                           for (const TranscriptLine& line : Transcript())
                           {
                               cJSON* item = cJSON_CreateObject();
                               cJSON_AddStringToObject(item, "kind", kinds[static_cast<int>(line.kind)]);
                               cJSON_AddStringToObject(item, "who", line.who.c_str());
                               cJSON_AddStringToObject(item, "text", line.text.c_str());
                               cJSON_AddItemToArray(lines, item);
                           }
                           return HP::JsonResponse(resp);
                       });

    hs.RegisterCommand({"npc_chat_test_voice", "Speak the sample sentence (optional voice id)", {{"voice", "string", false}}},
                       [](const std::string&, cJSON* root) -> std::string
                       {
                           const char* voice = HP::GetString(root, "voice");
                           if (voice && voice[0])
                               TestVoiceId(voice);
                           else
                               TestVoice();
                           return HP::OkResponse();
                       });

    hs.RegisterCommand({"npc_chat_report", "The situation report the model would receive now", {}},
                       [](const std::string&, cJSON*) -> std::string
                       {
                           const ReportResult r = BuildSituationReport(CurrentSettings());
                           if (!r.ok)
                               return HP::ErrorResponse(r.error.c_str());
                           cJSON* resp = cJSON_CreateObject();
                           cJSON_AddStringToObject(resp, "report", r.text.c_str());
                           cJSON_AddStringToObject(resp, "defaultSpeaker", r.defaultSpeaker.c_str());
                           return HP::JsonResponse(resp);
                       });
}
} // namespace Poseidon::NpcChat
