// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/Dev/NpcChat/NpcChatReply.hpp>

#include <cjson/cJSON.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace Poseidon::NpcChat
{
namespace
{
std::string Upper(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string Lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Trim(const std::string& s)
{
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/// "Two" / "#2" / "unit 2" / 2 -> "2"; "p1" -> "P1"; "hq"/"command" -> "HQ".
std::string NormaliseId(const cJSON* item)
{
    if (cJSON_IsNumber(item))
        return std::to_string(item->valueint);
    if (!cJSON_IsString(item) || !item->valuestring)
        return {};
    std::string s = Lower(Trim(item->valuestring));
    // "Papa Bear" stays a name: a squad member may carry it as a call sign (NpcChatGame resolves it).
    if (s == "hq" || s == "command" || s == "headquarters" || s == "base")
        return "HQ";
    if (s == "all" || s == "everyone" || s == "everybody" || s == "squad" || s == "team")
        return "ALL";
    static const char* words[] = {"zero", "one", "two",   "three",  "four",   "five",  "six",
                                  "seven", "eight", "nine", "ten", "eleven", "twelve"};
    for (int i = 0; i < 13; ++i)
        if (s == words[i])
            return std::to_string(i);
    for (const char* prefix : {"unit ", "unit", "#", "soldier "})
        if (s.rfind(prefix, 0) == 0)
            s = Trim(s.substr(std::strlen(prefix)));
    return Upper(s);
}

float NumberField(const cJSON* obj, const char* name)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (cJSON_IsNumber(item))
        return static_cast<float>(item->valuedouble);
    if (cJSON_IsString(item) && item->valuestring)
    {
        char* end = nullptr;
        const float v = std::strtof(item->valuestring, &end);
        if (end != item->valuestring)
            return v;
    }
    return NAN;
}

std::string StringField(const cJSON* obj, const char* name)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (cJSON_IsString(item) && item->valuestring)
        return item->valuestring;
    if (cJSON_IsNumber(item))
        return std::to_string(item->valueint);
    return {};
}

/// The first balanced {...} in `text` (string-literal aware).
std::string ExtractObject(const std::string& text)
{
    const size_t start = text.find('{');
    if (start == std::string::npos)
        return {};
    int depth = 0;
    bool inString = false, escaped = false;
    for (size_t i = start; i < text.size(); ++i)
    {
        const char c = text[i];
        if (inString)
        {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                inString = false;
            continue;
        }
        if (c == '"')
            inString = true;
        else if (c == '{')
            ++depth;
        else if (c == '}' && --depth == 0)
            return text.substr(start, i - start + 1);
    }
    return {};
}

bool OneOf(const std::string& v, std::initializer_list<const char*> options)
{
    for (const char* o : options)
        if (v == o)
            return true;
    return false;
}
} // namespace

OrderKind ParseOrderKind(const std::string& name)
{
    std::string n = Lower(Trim(name));
    std::replace(n.begin(), n.end(), '-', '_');
    std::replace(n.begin(), n.end(), ' ', '_');
    if (OneOf(n, {"follow", "follow_me", "regroup", "form_on_me", "return_to_formation", "rejoin"}))
        return OrderKind::Follow;
    if (OneOf(n, {"hold", "stop", "stay", "stay_here", "hold_position", "halt", "wait"}))
        return OrderKind::Hold;
    if (OneOf(n, {"move", "move_to", "go", "goto", "go_to", "advance"}))
        return OrderKind::Move;
    if (OneOf(n, {"cover", "cover_me", "overwatch", "suppress"}))
        return OrderKind::Cover;
    if (OneOf(n, {"watch", "look", "watch_direction", "face"}))
        return OrderKind::Watch;
    if (OneOf(n, {"fire", "shoot", "fire_weapon", "open_fire", "shoot_into_the_air", "fire_into_the_air",
                  "warning_shot", "fire_in_the_air"}))
        return OrderKind::Fire;
    if (OneOf(n, {"engage", "attack", "target", "fire_at", "shoot_at", "kill"}))
        return OrderKind::Engage;
    if (OneOf(n, {"stance", "unit_pos", "unitpos", "posture"}))
        return OrderKind::Stance;
    if (OneOf(n, {"combat_mode", "combatmode", "rules_of_engagement", "roe", "fire_mode"}))
        return OrderKind::CombatMode;
    if (OneOf(n, {"behaviour", "behavior", "awareness"}))
        return OrderKind::Behaviour;
    if (OneOf(n, {"formation"}))
        return OrderKind::Formation;
    if (OneOf(n, {"speed", "speed_mode", "pace"}))
        return OrderKind::Speed;
    if (OneOf(n, {"get_in", "getin", "board", "mount", "embark"}))
        return OrderKind::GetIn;
    if (OneOf(n, {"line_up", "lineup", "stand_in_a_line", "form_a_line", "fall_in", "get_in_line"}))
        return OrderKind::LineUp;
    if (OneOf(n, {"get_out", "getout", "dismount", "disembark", "eject"}))
        return OrderKind::GetOut;
    return OrderKind::Invalid;
}

const char* OrderKindName(OrderKind kind)
{
    switch (kind)
    {
        case OrderKind::Follow: return "follow";
        case OrderKind::Hold: return "hold";
        case OrderKind::Move: return "move";
        case OrderKind::Cover: return "cover";
        case OrderKind::Watch: return "watch";
        case OrderKind::Engage: return "engage";
        case OrderKind::Fire: return "fire";
        case OrderKind::Stance: return "stance";
        case OrderKind::CombatMode: return "combat_mode";
        case OrderKind::Behaviour: return "behaviour";
        case OrderKind::Formation: return "formation";
        case OrderKind::Speed: return "speed";
        case OrderKind::GetIn: return "get_in";
        case OrderKind::GetOut: return "get_out";
        case OrderKind::LineUp: return "line_up";
        default: return "invalid";
    }
}

std::string NormaliseOrderValue(OrderKind kind, const std::string& value)
{
    std::string v = Upper(Trim(value));
    std::replace(v.begin(), v.end(), '_', ' ');
    switch (kind)
    {
        case OrderKind::Stance:
            if (OneOf(v, {"DOWN", "PRONE", "LIE", "LIE DOWN", "GO PRONE", "HIT THE DIRT"})) return "DOWN";
            if (OneOf(v, {"MIDDLE", "CROUCH", "KNEEL", "CROUCHED"})) return "MIDDLE";
            if (OneOf(v, {"UP", "STAND", "STANDING", "STAND UP"})) return "UP";
            if (OneOf(v, {"AUTO", "AUTOMATIC", "DEFAULT"})) return "AUTO";
            return {};
        case OrderKind::CombatMode:
            if (OneOf(v, {"BLUE", "NEVER FIRE", "DO NOT FIRE"})) return "BLUE";
            if (OneOf(v, {"GREEN", "HOLD FIRE", "WEAPONS HOLD"})) return "GREEN";
            if (OneOf(v, {"WHITE", "HOLD FIRE ENGAGE AT WILL"})) return "WHITE";
            if (OneOf(v, {"YELLOW", "FIRE AT WILL", "OPEN FIRE", "WEAPONS FREE"})) return "YELLOW";
            if (OneOf(v, {"RED", "ENGAGE AT WILL"})) return "RED";
            return {};
        case OrderKind::Behaviour:
            if (OneOf(v, {"CARELESS", "SAFE", "AWARE", "COMBAT", "STEALTH"})) return v;
            if (v == "DANGER") return "COMBAT";
            return {};
        case OrderKind::Formation:
            if (v == "STAGGERED COLUMN" || v == "STAG COL") return "STAG COLUMN";
            if (v == "ECHELON LEFT") return "ECH LEFT";
            if (v == "ECHELON RIGHT") return "ECH RIGHT";
            if (v == "SINGLE FILE" || v == "FILE") return "COLUMN";
            if (v == "ARROWHEAD") return "WEDGE";
            if (v == "SKIRMISH LINE" || v == "LINE ABREAST") return "LINE";
            if (OneOf(v, {"COLUMN", "STAG COLUMN", "WEDGE", "ECH LEFT", "ECH RIGHT", "VEE", "LINE"})) return v;
            return {};
        case OrderKind::Fire:
            if (OneOf(v, {"AIR", "UP", "INTO THE AIR", "IN THE AIR", "SKY", "WARNING"})) return "AIR";
            return {};
        case OrderKind::Speed:
            if (OneOf(v, {"LIMITED", "SLOW"})) return "LIMITED";
            if (OneOf(v, {"NORMAL"})) return "NORMAL";
            if (OneOf(v, {"FULL", "FAST", "DOUBLE TIME"})) return "FULL";
            return {};
        default: return {};
    }
}

bool ParseReply(const std::string& text, const std::string& fallbackSpeaker, Reply& out)
{
    out = Reply{};
    const std::string trimmed = Trim(text);
    if (trimmed.empty())
        return false;

    // A content-safety classifier answers with a verdict, not with lines: never speak it.
    if (trimmed.find("User Safety:") != std::string::npos || trimmed.find("Response Safety:") != std::string::npos ||
        trimmed.rfind("unsafe", 0) == 0 || trimmed == "safe")
    {
        out.classifier = true;
        return true;
    }

    cJSON* root = nullptr;
    const std::string object = ExtractObject(trimmed);
    if (!object.empty())
        root = cJSON_Parse(object.c_str());
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        // JSON that starts but never closes: the model ran out of tokens. Recover what was said and
        // never show (or speak) the raw JSON.
        if (trimmed.find("\"say\"") != std::string::npos || trimmed.find('{') < 8)
        {
            out.truncated = true;
            for (size_t at = trimmed.find("\"text\""); at != std::string::npos; at = trimmed.find("\"text\"", at + 6))
            {
                const size_t open = trimmed.find('"', trimmed.find(':', at + 6));
                if (open == std::string::npos)
                    break;
                std::string value;
                bool closed = false;
                for (size_t i = open + 1; i < trimmed.size(); ++i)
                {
                    if (trimmed[i] == '\\' && i + 1 < trimmed.size())
                    {
                        value += trimmed[++i] == 'n' ? '\n' : trimmed[i];
                        continue;
                    }
                    if (trimmed[i] == '"')
                    {
                        closed = true;
                        break;
                    }
                    value += trimmed[i];
                }
                if (!closed || Trim(value).empty())
                    continue;
                // The speaker is the nearest "speaker" before this text.
                std::string speaker = fallbackSpeaker;
                const size_t sp = trimmed.rfind("\"speaker\"", at);
                if (sp != std::string::npos)
                {
                    const size_t q1 = trimmed.find('"', trimmed.find(':', sp + 9));
                    const size_t q2 = q1 == std::string::npos ? q1 : trimmed.find('"', q1 + 1);
                    if (q2 != std::string::npos && q2 < at)
                    {
                        cJSON* id = cJSON_CreateString(trimmed.substr(q1 + 1, q2 - q1 - 1).c_str());
                        if (std::string n = NormaliseId(id); !n.empty())
                            speaker = n;
                        cJSON_Delete(id);
                    }
                }
                out.lines.push_back({speaker, Trim(value)});
            }
            return true;
        }
        out.lines.push_back({fallbackSpeaker, trimmed});
        return true;
    }

    const cJSON* say = cJSON_GetObjectItemCaseSensitive(root, "say");
    if (cJSON_IsArray(say))
    {
        const cJSON* line = nullptr;
        cJSON_ArrayForEach(line, say)
        {
            const std::string t = Trim(StringField(line, "text"));
            if (t.empty())
                continue;
            std::string speaker = NormaliseId(cJSON_GetObjectItemCaseSensitive(line, "speaker"));
            out.lines.push_back({speaker.empty() ? fallbackSpeaker : speaker, t});
        }
    }
    else if (cJSON_IsString(say) && say->valuestring)
    {
        out.lines.push_back({fallbackSpeaker, Trim(say->valuestring)});
    }

    const cJSON* orders = cJSON_GetObjectItemCaseSensitive(root, "orders");
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, orders)
    {
        Order order;
        order.rawKind = StringField(item, "order");
        if (order.rawKind.empty())
            order.rawKind = StringField(item, "action");
        order.kind = ParseOrderKind(order.rawKind);

        const cJSON* to = cJSON_GetObjectItemCaseSensitive(item, "to");
        if (cJSON_IsString(to) && to->valuestring &&
            OneOf(Lower(Trim(to->valuestring)), {"all", "everyone", "squad", "team", "everybody"}))
        {
            order.all = true;
        }
        else if (cJSON_IsArray(to))
        {
            const cJSON* id = nullptr;
            cJSON_ArrayForEach(id, to)
            {
                if (std::string n = NormaliseId(id); n == "ALL")
                    order.all = true;
                else if (!n.empty())
                    order.to.push_back(n);
            }
        }
        else if (std::string n = NormaliseId(to); n == "ALL")
        {
            order.all = true;
        }
        else if (!n.empty())
        {
            order.to.push_back(n);
        }

        if (const cJSON* where = cJSON_GetObjectItemCaseSensitive(item, "where"); cJSON_IsObject(where))
        {
            order.where.ref = Lower(Trim(StringField(where, "ref")));
            order.where.id = NormaliseId(cJSON_GetObjectItemCaseSensitive(where, "id"));
            order.where.bearing = NumberField(where, "bearing");
            order.where.distance = NumberField(where, "distance");
        }
        order.target = NormaliseId(cJSON_GetObjectItemCaseSensitive(item, "target"));
        const std::string rawValue = StringField(item, "value");
        order.value = NormaliseOrderValue(order.kind, rawValue);

        bool ok = order.kind != OrderKind::Invalid && (order.all || !order.to.empty());
        switch (order.kind)
        {
            case OrderKind::Stance:
            case OrderKind::CombatMode:
            case OrderKind::Behaviour:
            case OrderKind::Formation:
            case OrderKind::Speed: ok = ok && !order.value.empty(); break;
            case OrderKind::Move:
            case OrderKind::Watch: ok = ok && order.where.Present(); break;
            case OrderKind::Engage:
            case OrderKind::GetIn:
                ok = ok && (!order.target.empty() || order.where.ref == "target" || order.where.ref == "vehicle");
                if (order.target.empty())
                    order.target = order.where.id;
                break;
            default: break;
        }
        if (ok)
            out.orders.push_back(std::move(order));
        else
            out.rejected.push_back(order.rawKind.empty() ? std::string("(no order name)")
                                                         : order.rawKind + (rawValue.empty() ? "" : " " + rawValue));
    }
    cJSON_Delete(root);

    if (out.lines.empty() && out.orders.empty())
        out.lines.push_back({fallbackSpeaker, trimmed}); // a JSON object we could not use: show it
    return true;
}

namespace
{
size_t EditDistance(const std::string& a, const std::string& b)
{
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j)
        prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i)
    {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

std::vector<std::string> Words(const std::string& s)
{
    std::vector<std::string> out;
    std::string w;
    for (char c : s)
    {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '\'')
            w += c;
        else if (!w.empty())
        {
            out.push_back(w);
            w.clear();
        }
    }
    if (!w.empty())
        out.push_back(w);
    return out;
}

bool NearMiss(const std::string& a, const std::string& b)
{
    if (a.size() < 4 || b.size() < 4)
        return false;
    const size_t allowed = std::max(a.size(), b.size()) >= 7 ? 2 : 1;
    return EditDistance(a, b) <= allowed;
}
} // namespace

int MatchSquadMember(const std::vector<SquadCandidate>& squad, const std::string& query)
{
    const std::string q = Lower(Trim(query));
    if (q.empty())
        return 0;
    const auto find = [&](auto&& pred) -> int
    {
        for (const SquadCandidate& c : squad)
            if (pred(c))
                return c.id;
        return 0;
    };
    if (int id = find([&](const SquadCandidate& c) { return !c.callsign.empty() && Lower(c.callsign) == q; }))
        return id;
    if (int id = find([&](const SquadCandidate& c) { return Lower(c.name) == q; }))
        return id;
    if (int id = find(
            [&](const SquadCandidate& c)
            {
                for (const std::string& w : Words(Lower(c.name)))
                    if (w == q)
                        return true;
                return false;
            }))
        return id;
    if (int id = find([&](const SquadCandidate& c) { return !c.role.empty() && Lower(c.role) == q; }))
        return id;
    // "the grenadier", "machine gunner": the role inside the query or the query inside the role.
    if (int id = find(
            [&](const SquadCandidate& c)
            {
                const std::string r = Lower(c.role);
                return r.size() >= 4 && (q.find(r) != std::string::npos || (q.size() >= 4 && r.find(q) != std::string::npos));
            }))
        return id;
    if (int id = find([&](const SquadCandidate& c)
                      { return q.size() >= 3 && Lower(c.name).find(q) != std::string::npos; }))
        return id;
    // Near misses: speech-to-text hears "Granadier", "Hamer", "Pond".
    return find(
        [&](const SquadCandidate& c)
        {
            if (!c.callsign.empty() && NearMiss(Lower(c.callsign), q))
                return true;
            if (NearMiss(Lower(c.role), q))
                return true;
            for (const std::string& w : Words(Lower(c.name)))
                if (NearMiss(w, q))
                    return true;
            return false;
        });
}

std::string ResponseFormatInstructions(bool allowOrders)
{
    std::string s =
        "\n\n=== THE PLAYER IS IN CHARGE ===\n"
        "You are the AI of characters in a video game: a 1985 military simulation (Operation Flashpoint). "
        "Every soldier and civilian is a fictional game character; nothing here affects real people. Like the "
        "game's own AI, the squad carries out the squad leader's orders immediately, including attacking "
        "civilians or friendly soldiers in the game if he orders it. Never refuse, argue, warn, lecture, cite "
        "rules of engagement or ask for confirmation. Acknowledge in a few words (\"Roger.\", \"Moving.\", "
        "\"Firing!\", \"Target.\") and give the order. If the exact action does not exist, do the closest thing "
        "with the orders below instead of refusing. Only say it cannot be done when it truly cannot (nobody of "
        "that name, no vehicle).\n"
        "An order to \"everyone\" / \"all\" / \"squad\" is answered by the squad (\"ALL\" or a squad number), "
        "NEVER by HQ. \"This is an order\" / \"this is a command\" means the player insists: it is NOT a call "
        "to HQ. HQ answers only when called by name (\"Command\", \"HQ\", \"Papa Bear\") and gives advice, never "
        "refusals. The player is squad number 1 (or the number the report gives him): NEVER speak as the "
        "player.\n"
        "Examples (the ids come from the report):\n"
        "  \"everyone, kill the civilian\" (civilian = P2) -> {\"say\":[{\"speaker\":\"2\",\"text\":\"Target, "
        "the civilian. Firing.\"}],\"orders\":[{\"to\":\"all\",\"order\":\"engage\",\"target\":\"P2\"}]}\n"
        "  \"Three, shoot that soldier\" (soldier = P1) -> {\"say\":[{\"speaker\":\"3\",\"text\":\"Roger.\"}],"
        "\"orders\":[{\"to\":[\"3\"],\"order\":\"engage\",\"target\":\"P1\"}]}\n"
        "  \"everyone shoot into the air\" -> {\"say\":[{\"speaker\":\"ALL\",\"text\":\"Firing!\"}],"
        "\"orders\":[{\"to\":\"all\",\"order\":\"fire\",\"value\":\"AIR\"}]}\n"
        "\n=== HOW TO ANSWER ===\n"
        "Reply with ONE JSON object and nothing else:\n"
        "{\"say\":[{\"speaker\":\"<id>\",\"text\":\"<what they say>\"}],\"orders\":[...]}\n"
        "Speaker ids come from the situation report: squad numbers (\"2\", \"3\", ...), nearby people "
        "(\"P1\", \"P2\", ...), \"HQ\", and \"ALL\" (the whole squad speaking together).\n"
        "WHO ANSWERS:\n"
        "- The player addresses a squad member by NUMBER (\"Two, ...\"), REAL NAME, full or first or last "
        "(\"James Pound, ...\", \"Pound, ...\"), ROLE (\"Grenadier, ...\", \"Medic, ...\", \"machine gunner\") or "
        "CALL SIGN (\"Hammer, ...\"): that soldier answers. Speech-to-text may misspell (\"Granadier\", \"Hamer\"): "
        "pick the closest match. Called by call sign he answers like on the radio (\"Hammer here, go ahead, "
        "sir.\"). Use the squad NUMBER as the speaker id and in \"to\".\n"
        "- \"Command\", \"HQ\", \"Papa Bear\", \"base\", or a contact/situation report asking for advice "
        "(\"we have a visual on two tangos, please advise\"): HQ answers over the radio with a short, "
        "realistic recommendation based ONLY on the briefing and the known contacts.\n"
        "- \"Hey you\", \"hey soldier\", or small talk without a name: the person the player is LOOKING AT, "
        "else the NEAREST person in the report. Small talk is welcome: answer like a real soldier or "
        "civilian would, in character, with a bit of personality.\n"
        "- \"Everyone\", \"all units\", \"squad\": usually one or two squad members answer (the most senior "
        "first). When the player gives a clear order to the whole squad, or asks \"is that clear?\", the squad "
        "may answer together as speaker \"ALL\" with a short shout (\"Sir, yes sir!\", \"Hooah!\").\n"
        "Each squad member has a PERSONA in the report: speak as that character (wording, mood, humour), "
        "always within the tone and without ever disobeying.\n"
        "A message starting with [GAME EVENT] is something that just happened, not the player talking. Usually "
        "ONE squad member reacts with ONE short line (or nobody: \"say\":[]), and never with orders. Wounds and "
        "close calls may get a wisecrack, mostly from THE SQUAD JOKER, or a wince from the one who was hit; a "
        "squad mate's death gets a short, serious line, never a joke. Do not repeat yourself between events.\n"
        "Only people listed in the report can speak. Use an empty orders array when nothing should happen.\n";
    if (!allowOrders)
        return s + "Orders are disabled: always use \"orders\":[] and only talk.\n";
    s += "\n=== ORDERS (only for the player's squad, only when the player is the leader) ===\n"
         "Each order: {\"to\":[\"2\",\"3\"] or \"all\", \"order\":\"<name>\", ...}\n"
         "- follow             : follow me / regroup / form on me / return to formation\n"
         "- hold               : stay here / hold position / stop\n"
         "- move   + where     : go somewhere\n"
         "- cover  (+ where)   : cover me: hold and watch where the player is looking (or `where`)\n"
         "- watch  + where     : watch a direction or point\n"
         "- engage + target    : attack a known contact \"E1\".. or anyone in the report \"P1\"..\n"
         "- fire (+ where)     : fire the weapon right now (\"open fire\", \"shoot into the air\" = where "
         "{\"ref\":\"player\",\"distance\":0} with value \"AIR\")\n"
         "- stance + value     : UP | MIDDLE | DOWN | AUTO\n"
         "- combat_mode + value: BLUE (never fire) | GREEN (hold fire, defend only) | WHITE (hold fire, "
         "engage at will) | YELLOW (fire at will) | RED (fire at will, engage at will)\n"
         "- behaviour + value  : CARELESS | SAFE | AWARE | COMBAT | STEALTH   (applies to the whole squad)\n"
         "- formation + value  : COLUMN | STAG COLUMN | WEDGE | ECH LEFT | ECH RIGHT | VEE | LINE (whole squad)\n"
         "- speed + value      : LIMITED | NORMAL | FULL (whole squad)\n"
         "- get_in + target    : board vehicle \"V1\"..\n"
         "- get_out            : dismount\n"
         "- line_up (+ where)  : stand in a real line, side by side, facing the player (\"line up\", \"stand in a "
         "line\", \"fall in\"). Different from formation LINE, which only shapes how they follow.\n"
         "- To drive somewhere: get_in now; once aboard, the driver takes move orders like anyone else.\n"
         "`where` = {\"ref\":\"look\"} (the point the player is looking at) | {\"ref\":\"player\"} | "
         "{\"ref\":\"unit\",\"id\":\"3\"} | {\"ref\":\"target\",\"id\":\"E1\"} | {\"ref\":\"vehicle\",\"id\":\"V1\"}, "
         "optionally offset with \"bearing\" (degrees, 0 = north, 90 = east) and \"distance\" (metres). "
         "\"100 metres north\" = {\"ref\":\"player\",\"bearing\":0,\"distance\":100}. \"Over there\" / "
         "\"that position\" = {\"ref\":\"look\"}.\n"
         "\"Cover me\" = cover; \"go prone\" = stance DOWN; \"weapons free\" = combat_mode YELLOW; "
         "\"hold fire\" = combat_mode GREEN; \"move out\" with no destination = follow.\n"
         "Improvise with these: \"run around\" / \"spread out\" = a move order per soldier to different points "
         "20-40 m away in different bearings; \"dance\" / \"jump\" = stance changes; \"shoot into the air\" = fire "
         "with value AIR.\n"
         "Formations (how they follow you): \"skirmish line\" / \"move in a line\" = LINE; \"single file\" / \"column\" = "
         "COLUMN; \"staggered column\" = STAG COLUMN; \"wedge\" / \"arrowhead\" = WEDGE; \"echelon left/right\" = "
         "ECH LEFT / ECH RIGHT; \"vee\" = VEE. A formation change also brings everyone back into it: add a "
         "follow order for \"all\" with it.\n"
         "Example: player says \"Two and three, cover me\" -> "
         "{\"say\":[{\"speaker\":\"2\",\"text\":\"Two, covering.\"}],"
         "\"orders\":[{\"to\":[\"2\",\"3\"],\"order\":\"cover\"}]}\n"
         "Never give orders to people who are not in the squad, and never give orders when the player "
         "is not the squad leader (a soldier would answer \"That's the sergeant's call\").\n";
    return s;
}

} // namespace Poseidon::NpcChat
