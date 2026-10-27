// SPDX-License-Identifier: GPL-3.0-or-later
// The last war's settings, kept in the profile (conquest-settings.json): the setup screen opens on them and
// "New war" starts with them (CONQUEST-PLAN.md 3.7 and 4.6). Included by ConquestReferee.cpp inside its
// anonymous namespace.

std::string SettingsPath()
{
    return GamePaths::Instance().UserDir() + "conquest-settings.json";
}

void SaveSettings(const Options& o)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "side", o.side);
    cJSON_AddBoolToObject(root, "startInCommand", o.startInCommand);
    cJSON_AddBoolToObject(root, "commanderView", o.commanderView);
    cJSON_AddBoolToObject(root, "shroud", o.fog == FogMode::Shroud);
    cJSON_AddBoolToObject(root, "possession", o.possession);
    cJSON_AddBoolToObject(root, "hqRadio", o.hqRadio);
    cJSON_AddBoolToObject(root, "resistance", o.resistance);
    cJSON_AddBoolToObject(root, "resistanceAlly", o.resistanceAlly);
    cJSON_AddBoolToObject(root, "enemyMarkers", o.enemyMarkers);
    cJSON_AddNumberToObject(root, "victoryTowns", o.victoryTowns);
    cJSON_AddNumberToObject(root, "timeLimit", o.timeLimit);
    cJSON_AddNumberToObject(root, "aiSkill", o.aiSkill);
    cJSON_AddNumberToObject(root, "weather", o.weather);
    cJSON* sides = cJSON_AddArrayToObject(root, "sides");
    for (int s = 0; s < MaxSides; s++)
    {
        cJSON* e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "startPool", o.startPool[s]);
        cJSON_AddBoolToObject(e, "llm", o.llmCommander[s]);
        cJSON_AddStringToObject(e, "llmModel", o.llmModel[s].c_str());
        cJSON_AddItemToArray(sides, e);
    }
    if (char* text = cJSON_Print(root))
    {
        if (FILE* f = std::fopen(SettingsPath().c_str(), "w"))
        {
            std::fputs(text, f);
            std::fclose(f);
        }
        cJSON_free(text);
    }
    cJSON_Delete(root);
}

// The remembered settings over `o` (anything missing keeps its value); false when there are none.
bool LoadSettings(Options& o, int sideCount)
{
    FILE* f = std::fopen(SettingsPath().c_str(), "r");
    if (!f)
        return false;
    std::string text;
    char buffer[4096];
    for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0;)
        text.append(buffer, n);
    std::fclose(f);
    cJSON* root = cJSON_Parse(text.c_str());
    if (!root)
        return false;
    const auto number = [&](const char* key, float fallback)
    {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(root, key);
        return cJSON_IsNumber(v) ? static_cast<float>(v->valuedouble) : fallback;
    };
    const auto flag = [&](const cJSON* parent, const char* key, bool fallback)
    {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(parent, key);
        return cJSON_IsBool(v) ? cJSON_IsTrue(v) != 0 : fallback;
    };
    o.side = std::clamp(static_cast<int>(number("side", static_cast<float>(o.side))), 0, std::max(0, sideCount - 1));
    o.startInCommand = flag(root, "startInCommand", o.startInCommand);
    o.commanderView = flag(root, "commanderView", o.commanderView);
    o.fog = flag(root, "shroud", o.fog == FogMode::Shroud) ? FogMode::Shroud : FogMode::Contacts;
    o.possession = flag(root, "possession", o.possession);
    o.hqRadio = flag(root, "hqRadio", o.hqRadio);
    o.resistance = flag(root, "resistance", o.resistance);
    o.resistanceAlly = flag(root, "resistanceAlly", o.resistanceAlly);
    o.enemyMarkers = flag(root, "enemyMarkers", o.enemyMarkers);
    o.victoryTowns = std::max(0, static_cast<int>(number("victoryTowns", static_cast<float>(o.victoryTowns))));
    o.timeLimit = std::max(0, static_cast<int>(number("timeLimit", static_cast<float>(o.timeLimit))));
    o.aiSkill = std::clamp(static_cast<int>(number("aiSkill", static_cast<float>(o.aiSkill))), 0, 2);
    o.weather = std::clamp(static_cast<int>(number("weather", static_cast<float>(o.weather))), 0, 6);
    if (const cJSON* sides = cJSON_GetObjectItemCaseSensitive(root, "sides"); cJSON_IsArray(sides))
    {
        int s = 0;
        const cJSON* e = nullptr;
        cJSON_ArrayForEach(e, sides)
        {
            if (s >= MaxSides)
                break;
            // 500 was every side's default until 2026-10-26 (now the mission's, 2000): a profile that still holds
            // it never chose it, and keeps the new default.
            if (const cJSON* pool = cJSON_GetObjectItemCaseSensitive(e, "startPool");
                cJSON_IsNumber(pool) && pool->valuedouble != 500.0)
                o.startPool[s] = std::max(0.0f, static_cast<float>(pool->valuedouble));
            o.llmCommander[s] = flag(e, "llm", o.llmCommander[s]);
            if (const cJSON* model = cJSON_GetObjectItemCaseSensitive(e, "llmModel"); cJSON_IsString(model))
                o.llmModel[s] = model->valuestring;
            s++;
        }
    }
    cJSON_Delete(root);
    return true;
}

// Multiplayer has no setup screen: the host picks the war's settings in the game's own lobby, as the
// mission's parameters (description.ext titleParam1/2), and the server sets param1 and param2 before the
// war begins: param1 the supplies each side starts with, param2 the time limit in minutes (0: none).
// Without them (a mission that defines none) the mission's defaults stand.
void ApplyLobbyParams(Options& o)
{
    GameState* gs = GWorld ? GWorld->GetGameState() : nullptr;
    if (!gs)
        return;
    const GameValue p1 = gs->VarGet("param1"), p2 = gs->VarGet("param2");
    if (p1.GetType() == GameScalar && static_cast<float>(p1) > 0.0f && static_cast<float>(p1) <= 100000.0f)
        o.startPool.fill(static_cast<float>(p1));
    if (p2.GetType() == GameScalar && static_cast<float>(p2) >= 0.0f && static_cast<float>(p2) <= 1440.0f)
        o.timeLimit = static_cast<int>(static_cast<float>(p2));
}
