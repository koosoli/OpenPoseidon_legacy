// SPDX-License-Identifier: GPL-3.0-or-later
//
// Arma 3 mission.sqm -> the Arma 2 layout this engine reads (see Arma3Eden.hpp).
//
// Written from the mission files themselves: every rule below was checked against the
// 387 mission.sqm files shipped in Arma 3 and its DLCs (283 Eden, 104 Arma 2 layout).
//
// Eden -> Arma 2 layout, entity by entity:
//
//  Layer      its entities are taken as if they were not in a layer.
//  Group      class Groups >> ItemN, side from the group. Its Objects become the group's
//             units in order; the unit whose flags carry bit 2 is the leader (else the
//             first). An Object that the group's CrewLinks name as a vehicle (item1) is
//             no unit: it goes to class Vehicles (side EMPTY, named after its variable or
//             "op_eden_v<id>"), and its crew boards it from their init lines
//             (moveInDriver / moveInGunner / moveInCommander / moveInCargo by role and
//             turret path). Group attributes (name, behaviour, combatMode, speedMode,
//             formation, init) go to the leader's init line.
//  Object     outside a group: class Vehicles, side EMPTY.
//  Logic      its own group of side LOGIC (a logic inside a group stays in it).
//  Waypoint   class Waypoints of its group; Eden already uses the Arma 2 field names,
//             the type is mapped (SeekAndDestroy -> SAD, TransportUnload -> TR UNLOAD...).
//  Marker     class Markers; Arma 3 marker and colour names are mapped to the nearest
//             ones this engine defines; polylines are dropped.
//  Trigger    class Sensors; Attributes are renamed (sizeA -> a, condition -> expCond,
//             onActivation -> expActiv, repeatable -> repeating, timeout[] -> timeoutMin/
//             Mid/Max, isRectangle -> rectangular, effect* -> class Effects), the angle
//             turned from radians to degrees. ANYPLAYER activation becomes ANY with the
//             player required in the list when the condition is the plain "this".
//  Comment    dropped.
//
// Unit attributes: position from PositionInfo, azimuth = angles[1] in degrees, placed
// exactly (special NONE; aircraft above the ground FLY), name -> text, isPlayer ->
// PLAYER COMMANDER (the first playable unit when no unit is the player), isPlayable ->
// PLAY CDG, rank, skill, health, fuel, ammo, presence, presenceCondition,
// placementRadius -> placement. Inventory becomes removeAllWeapons + addMagazine +
// addWeapon in the init line; stance -> setUnitPos. Every CustomAttributes entry runs its
// own expression with _this = the entity and _value = its value, as Arma 3 does, after
// the unit's own init (attributes that only dress the model -- textures, materials,
// animation sources, pylons, vehicle customisation -- are left out).
//
// Connections: Sync and WaypointActivation become a synchronisation of exactly the two
// items (a group stands for its leader); TriggerOwner sets the trigger's idVehicle and
// its owner activation; RandomStart adds the marker to the unit's markers[].

#include "Arma3Eden.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Poseidon::Arma1Compat
{
namespace
{
// ---------------------------------------------------------------------------------------
// Config text -> tree
// ---------------------------------------------------------------------------------------

bool IEquals(const std::string& a, const char* b)
{
    const size_t n = std::strlen(b);
    if (a.size() != n)
        return false;
    for (size_t i = 0; i < n; i++)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

std::string Lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Val
{
    bool array = false;
    bool quoted = false;
    std::string s;
    std::vector<Val> items;
};

struct Node
{
    std::string name;
    std::string base;
    bool isClass = false;
    bool isExtern = false;
    bool hasValue = false;
    bool append = false; // "name[]+={...}"
    Val value;
    std::vector<std::unique_ptr<Node>> kids;

    Node* Find(const char* key) const
    {
        for (const auto& k : kids)
            if (IEquals(k->name, key))
                return k.get();
        return nullptr;
    }
    const Node* Class(const char* key) const
    {
        const Node* n = Find(key);
        return n && n->isClass && !n->isExtern ? n : nullptr;
    }
    const Val* V(const char* key) const
    {
        const Node* n = Find(key);
        return n && n->hasValue ? &n->value : nullptr;
    }
    std::string S(const char* key, const std::string& def = std::string()) const
    {
        const Val* v = V(key);
        return v && !v->array ? v->s : def;
    }
    bool Has(const char* key) const { return V(key) != nullptr; }
    double D(const char* key, double def) const
    {
        const Val* v = V(key);
        if (!v || v->array || v->s.empty())
            return def;
        char* end = nullptr;
        const double d = std::strtod(v->s.c_str(), &end);
        return end == v->s.c_str() ? def : d;
    }
    long I(const char* key, long def) const { return static_cast<long>(std::lround(D(key, static_cast<double>(def)))); }
    // The "ItemN" classes of a list class (Entities, Links, MagazineCargo, ...).
    std::vector<const Node*> Items() const
    {
        std::vector<const Node*> out;
        for (const auto& k : kids)
            if (k->isClass && !k->isExtern && k->name.size() > 4 && Lower(k->name.substr(0, 4)) == "item")
                out.push_back(k.get());
        return out;
    }
};

struct Token
{
    enum Type
    {
        End,
        Word,
        String,
        Punct
    } type = End;
    std::string s;
};

class Lexer
{
    const std::string& _t;
    size_t _i = 0;
    bool _lineStart = true;

  public:
    explicit Lexer(const std::string& t) : _t(t) {}

    Token Next()
    {
        const size_t n = _t.size();
        while (_i < n)
        {
            const char c = _t[_i];
            if (c == '\n')
            {
                _lineStart = true;
                _i++;
                continue;
            }
            if (std::isspace(static_cast<unsigned char>(c)))
            {
                _i++;
                continue;
            }
            if (c == '/' && _i + 1 < n && _t[_i + 1] == '/')
            {
                while (_i < n && _t[_i] != '\n')
                    _i++;
                continue;
            }
            if (c == '/' && _i + 1 < n && _t[_i + 1] == '*')
            {
                const size_t end = _t.find("*/", _i + 2);
                _i = end == std::string::npos ? n : end + 2;
                continue;
            }
            if (c == '#' && _lineStart)
            {
                while (_i < n && _t[_i] != '\n')
                    _i++;
                continue;
            }
            break;
        }
        _lineStart = false;
        Token tok;
        if (_i >= n)
            return tok;
        const char c = _t[_i];
        if (c == '"')
        {
            tok.type = Token::String;
            _i++;
            while (_i < n)
            {
                if (_t[_i] == '"')
                {
                    if (_i + 1 < n && _t[_i + 1] == '"')
                    {
                        tok.s += '"';
                        _i += 2;
                        continue;
                    }
                    _i++;
                    break;
                }
                tok.s += _t[_i++];
            }
            return tok;
        }
        if (c == '[' && _i + 1 < n && _t[_i + 1] == ']')
        {
            tok.type = Token::Punct;
            tok.s = "[]";
            _i += 2;
            return tok;
        }
        if (c == '+' && _i + 1 < n && _t[_i + 1] == '=')
        {
            tok.type = Token::Punct;
            tok.s = "+=";
            _i += 2;
            return tok;
        }
        if (std::strchr("{};=,:", c))
        {
            tok.type = Token::Punct;
            tok.s = std::string(1, c);
            _i++;
            return tok;
        }
        tok.type = Token::Word;
        while (_i < n)
        {
            const char d = _t[_i];
            if (std::isspace(static_cast<unsigned char>(d)) || std::strchr("{};=,:\"", d) ||
                (d == '[' && _i + 1 < n && _t[_i + 1] == ']'))
                break;
            tok.s += d;
            _i++;
        }
        return tok;
    }
};

class Parser
{
    Lexer _lex;
    Token _tok;
    int _errors = 0;

    void Advance() { _tok = _lex.Next(); }
    bool IsPunct(const char* p) const { return _tok.type == Token::Punct && _tok.s == p; }

    Val ParseArray(int depth)
    {
        Val v;
        v.array = true;
        if (!IsPunct("{"))
        {
            _errors++;
            return v;
        }
        Advance();
        while (_tok.type != Token::End && !IsPunct("}"))
        {
            if (IsPunct("{"))
            {
                if (depth > 32)
                {
                    _errors++;
                    return v;
                }
                v.items.push_back(ParseArray(depth + 1));
            }
            else if (_tok.type == Token::String || _tok.type == Token::Word)
            {
                Val item;
                item.quoted = _tok.type == Token::String;
                item.s = _tok.s;
                Advance();
                while (item.quoted && _tok.type == Token::Word && _tok.s == "\\n")
                {
                    Advance();
                    if (_tok.type != Token::String)
                        break;
                    item.s += "\n" + _tok.s;
                    Advance();
                }
                // an unquoted expression item ("1 + 2") keeps every word
                while (_tok.type == Token::Word)
                {
                    item.s += " " + _tok.s;
                    Advance();
                }
                v.items.push_back(std::move(item));
                if (IsPunct(","))
                    Advance();
                continue;
            }
            else
            {
                Advance();
                continue;
            }
            if (IsPunct(","))
                Advance();
        }
        if (IsPunct("}"))
            Advance();
        return v;
    }

    void ParseBody(Node& node, int depth)
    {
        while (_tok.type != Token::End && !IsPunct("}"))
        {
            if (_tok.type == Token::Punct)
            {
                Advance(); // stray ';' and the like
                continue;
            }
            const std::string word = _tok.s;
            const bool quotedWord = _tok.type == Token::String;
            Advance();
            if (!quotedWord && word == "class")
            {
                auto cls = std::make_unique<Node>();
                cls->isClass = true;
                cls->name = _tok.s;
                Advance();
                if (IsPunct(":"))
                {
                    Advance();
                    cls->base = _tok.s;
                    Advance();
                }
                if (IsPunct("{"))
                {
                    Advance();
                    if (depth < 128)
                        ParseBody(*cls, depth + 1);
                    if (IsPunct("}"))
                        Advance();
                }
                else
                    cls->isExtern = true;
                node.kids.push_back(std::move(cls));
                continue;
            }
            if (!quotedWord && word == "delete")
            {
                Advance();
                continue;
            }
            auto entry = std::make_unique<Node>();
            entry->name = word;
            entry->hasValue = true;
            if (IsPunct("[]"))
            {
                Advance();
                entry->append = IsPunct("+=");
                if (IsPunct("=") || IsPunct("+="))
                    Advance();
                entry->value = ParseArray(0);
            }
            else
            {
                if (IsPunct("="))
                    Advance();
                else
                    _errors++;
                if (IsPunct("{"))
                    entry->value = ParseArray(0); // "name = {...}" without []
                else
                {
                    entry->value.quoted = _tok.type == Token::String;
                    entry->value.s = _tok.s;
                    Advance();
                    // "line 1" \n "line 2": a string over several lines, joined by newlines
                    while (entry->value.quoted && _tok.type == Token::Word && _tok.s == "\\n")
                    {
                        Advance();
                        if (_tok.type != Token::String)
                            break;
                        entry->value.s += "\n" + _tok.s;
                        Advance();
                    }
                    while (_tok.type == Token::Word || _tok.type == Token::String)
                    {
                        entry->value.s += " " + _tok.s;
                        entry->value.quoted = false;
                        Advance();
                    }
                }
            }
            node.kids.push_back(std::move(entry));
        }
    }

  public:
    explicit Parser(const std::string& text) : _lex(text) { Advance(); }
    std::unique_ptr<Node> Parse()
    {
        auto root = std::make_unique<Node>();
        root->isClass = true;
        while (_tok.type != Token::End)
        {
            ParseBody(*root, 0);
            if (IsPunct("}"))
            {
                _errors++;
                Advance();
            }
        }
        return root;
    }
    int Errors() const { return _errors; }
};

// ---------------------------------------------------------------------------------------
// Tree -> config text
// ---------------------------------------------------------------------------------------

std::string Quote(const std::string& s)
{
    std::string q = "\"";
    for (char c : s)
    {
        if (c == '"')
            q += "\"\"";
        else if (c == '\r' || c == '\n')
            q += ' ';
        else
            q += c;
    }
    return q + "\"";
}

std::string ValText(const Val& v)
{
    if (!v.array)
        return v.quoted ? Quote(v.s) : (v.s.empty() ? std::string("0") : v.s);
    std::string out = "{";
    for (size_t i = 0; i < v.items.size(); i++)
    {
        if (i)
            out += ",";
        out += ValText(v.items[i]);
    }
    return out + "}";
}

std::string Num(double d)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", d);
    if (std::strchr(buf, 'e') || std::strchr(buf, 'E'))
        std::snprintf(buf, sizeof(buf), "%.6f", d);
    return buf;
}

class Writer
{
    std::string _s;
    int _depth = 0;

    void Indent() { _s.append(static_cast<size_t>(_depth), '\t'); }

  public:
    void Open(const std::string& name, const std::string& base = std::string())
    {
        Indent();
        _s += "class " + name + (base.empty() ? "" : " : " + base) + "\r\n";
        Indent();
        _s += "{\r\n";
        _depth++;
    }
    void Close()
    {
        _depth--;
        Indent();
        _s += "};\r\n";
    }
    void Str(const std::string& name, const std::string& value)
    {
        Indent();
        _s += name + "=" + Quote(value) + ";\r\n";
    }
    void Raw(const std::string& name, const std::string& value)
    {
        Indent();
        _s += name + "=" + value + ";\r\n";
    }
    void Arr(const std::string& name, const std::string& text)
    {
        Indent();
        _s += name + "[]=" + text + ";\r\n";
    }
    void Value(const std::string& name, const Val& v)
    {
        if (v.array)
            Arr(name, ValText(v));
        else
            Raw(name, ValText(v));
    }
    void Copy(const Node& n)
    {
        if (n.isClass)
        {
            if (n.isExtern)
            {
                Indent();
                _s += "class " + n.name + ";\r\n";
                return;
            }
            Open(n.name, n.base);
            for (const auto& k : n.kids)
                Copy(*k);
            Close();
            return;
        }
        if (n.hasValue)
        {
            Indent();
            _s += n.name + (n.value.array ? (n.append ? "[]+=" : "[]=") : "=") + ValText(n.value) + ";\r\n";
        }
    }
    std::string& Text() { return _s; }
};

std::string IntArray(const std::vector<int>& v)
{
    std::string out = "{";
    for (size_t i = 0; i < v.size(); i++)
        out += (i ? "," : "") + std::to_string(v[i]);
    return out + "}";
}

// ---------------------------------------------------------------------------------------
// Name tables
// ---------------------------------------------------------------------------------------

std::string SideName(const std::string& eden)
{
    const std::string s = Lower(eden);
    if (s == "west" || s == "blufor")
        return "WEST";
    if (s == "east" || s == "opfor")
        return "EAST";
    if (s == "independent" || s == "guer" || s == "resistance")
        return "GUER";
    if (s == "civilian" || s == "civ" || s == "ambientlife" || s == "ambient life")
        return "CIV";
    if (s == "logic")
        return "LOGIC";
    if (s == "empty")
        return "EMPTY";
    return eden.empty() ? std::string("EMPTY") : eden;
}

std::string WaypointType(const std::string& eden)
{
    static const std::map<std::string, const char*> table = {
        {"move", "MOVE"},          {"destroy", "DESTROY"},           {"getin", "GETIN"},
        {"seekanddestroy", "SAD"}, {"sad", "SAD"},                   {"join", "JOIN"},
        {"joinandlead", "LEADER"}, {"leader", "LEADER"},             {"getout", "GETOUT"},
        {"cycle", "CYCLE"},        {"load", "LOAD"},                 {"unload", "UNLOAD"},
        {"transportunload", "TR UNLOAD"}, {"tr unload", "TR UNLOAD"}, {"hold", "HOLD"},
        {"sentry", "SENTRY"},      {"guard", "GUARD"},               {"talk", "TALK"},
        {"scripted", "SCRIPTED"},  {"support", "SUPPORT"},           {"getinnearest", "GETIN NEAREST"},
        {"getin nearest", "GETIN NEAREST"}, {"dismiss", "DISMISS"},  {"and", "AND"},
        {"or", "OR"},
    };
    const auto it = table.find(Lower(eden));
    // Loiter, Land, Unhook, Hook, MoveFast, Patrol, Demine...: no counterpart; the group
    // still goes there.
    return it != table.end() ? std::string(it->second) : std::string("MOVE");
}

std::string MarkerTypeFor(const std::string& type, const EdenEnvironment& env)
{
    const auto exists = [&](const std::string& n) { return !env.markerExists || env.markerExists(n); };
    if (type.empty() || exists(type))
        return type;
    const std::string t = Lower(type);
    std::vector<std::string> candidates;
    const auto after = [&](const char* prefix) { return t.rfind(prefix, 0) == 0 ? t.substr(std::strlen(prefix)) : std::string(); };
    std::string word = after("hd_");
    if (word.empty())
        word = after("mil_");
    if (word == "dot" || word == "box" || word == "triangle" || word == "circle" || word == "dot_noshadow")
        candidates = {"Dot"};
    else if (word == "flag")
        candidates = {"Flag1", "Flag"};
    else if (word == "destroy")
        candidates = {"Destroy"};
    else if (word == "start")
        candidates = {"Start"};
    else if (word == "end")
        candidates = {"End"};
    else if (word == "warning" || word == "ambush")
        candidates = {"Warning"};
    else if (word == "join")
        candidates = {"Join"};
    else if (word == "pickup")
        candidates = {"Pickup"};
    else if (word == "unknown")
        candidates = {"Unknown"};
    else if (word.rfind("arrow", 0) == 0)
        candidates = {"Arrow"};
    else if (word == "objective")
        candidates = {"Marker", "Flag"};
    else if (t.rfind("flag_", 0) == 0)
        candidates = {"Flag", "Flag1"};
    else if (t.rfind("b_", 0) == 0 || t.rfind("o_", 0) == 0 || t.rfind("n_", 0) == 0 || t.rfind("c_", 0) == 0 ||
             t.rfind("u_", 0) == 0)
        candidates = {"Marker", "Dot"};
    candidates.insert(candidates.end(), {"Dot", "Marker", "Empty"});
    for (const std::string& c : candidates)
        if (exists(c))
            return c;
    return type;
}

std::string ColorFor(const std::string& color, const EdenEnvironment& env)
{
    const auto exists = [&](const std::string& n) { return !env.colorExists || env.colorExists(n); };
    if (color.empty() || exists(color))
        return color;
    static const std::map<std::string, const char*> table = {
        {"colorwest", "ColorBlue"},        {"colorblufor", "ColorBlue"},  {"coloreast", "ColorRed"},
        {"coloropfor", "ColorRed"},        {"colorguer", "ColorGreen"},   {"colorindependent", "ColorGreen"},
        {"colorciv", "ColorYellow"},       {"colorcivilian", "ColorYellow"}, {"colorunknown", "ColorYellow"},
        {"colororange", "ColorYellow"},    {"colorpink", "ColorRed"},     {"colorbrown", "ColorBlack"},
        {"colorkhaki", "ColorYellow"},     {"colorgrey", "ColorWhite"},   {"colorgreen", "ColorGreen"},
    };
    const auto it = table.find(Lower(color));
    if (it != table.end() && exists(it->second))
        return it->second;
    return exists("Default") ? std::string("Default") : color;
}

std::string FillFor(const std::string& fill)
{
    const std::string f = Lower(fill);
    if (f == "solidborder" || f == "solidfull")
        return "Solid";
    return fill;
}

// ---------------------------------------------------------------------------------------
// SQF from Eden attribute values
// ---------------------------------------------------------------------------------------

// A group's init code names its group `this` (Arma 3 runs it with `this` = the group). Here
// it runs from the leader's init line, where `this` is the leader and cannot be assigned, so
// the code is given the group as _this and every `this` of its own reads _this instead
// (strings and longer names such as thisList are left alone).
std::string GroupInitCode(const std::string& code)
{
    std::string out;
    out.reserve(code.size() + 16);
    const auto ident = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    for (size_t i = 0; i < code.size();)
    {
        const char c = code[i];
        if (c == '"' || c == '\'')
        {
            // a string runs to its closing quote; a doubled quote stays inside it
            size_t j = i + 1;
            while (j < code.size())
            {
                if (code[j] == c)
                {
                    if (j + 1 < code.size() && code[j + 1] == c)
                    {
                        j += 2;
                        continue;
                    }
                    break;
                }
                j++;
            }
            out.append(code, i, std::min(code.size(), j + 1) - i);
            i = j + 1;
            continue;
        }
        if ((c == 't' || c == 'T') && (i == 0 || !ident(code[i - 1])) && code.size() - i >= 4 &&
            _strnicmp(code.c_str() + i, "this", 4) == 0 && (i + 4 == code.size() || !ident(code[i + 4])))
        {
            out += "_this";
            i += 4;
            continue;
        }
        out.push_back(c);
        i++;
    }
    return out;
}

std::string SqfString(const std::string& s)
{
    std::string q = "\"";
    for (char c : s)
    {
        if (c == '"')
            q += "\"\"";
        else if (c == '\r' || c == '\n')
            q += ' ';
        else
            q += c;
    }
    return q + "\"";
}

// class Value >> class data { class type { type[]={"SCALAR"}; }; value=... }
bool SqfFromData(const Node& data, std::string& out, int depth = 0)
{
    if (depth > 16)
        return false;
    std::string type;
    if (const Node* t = data.Class("type"))
        if (const Val* arr = t->V("type"); arr && arr->array && !arr->items.empty())
            type = Lower(arr->items[0].s);
    const Node* valueNode = data.Find("value");
    if (!valueNode)
        return false;
    if (valueNode->isClass)
    {
        // an array: class value { items=N; class Item0 { class data {...} } }
        std::string list = "[";
        bool first = true;
        for (const Node* item : valueNode->Items())
        {
            const Node* d = item->Class("data");
            std::string element;
            if (!d || !SqfFromData(*d, element, depth + 1))
                return false;
            list += (first ? "" : ",") + element;
            first = false;
        }
        out = list + "]";
        return true;
    }
    const Val& v = valueNode->value;
    if (v.array)
    {
        std::string list = "[";
        for (size_t i = 0; i < v.items.size(); i++)
            list += (i ? "," : "") + (v.items[i].quoted ? SqfString(v.items[i].s) : v.items[i].s);
        out = list + "]";
        return true;
    }
    if (type == "bool")
        out = (v.s == "0" || IEquals(v.s, "false")) ? "false" : "true";
    else if (type == "string" || type == "text" || v.quoted)
        out = SqfString(v.s);
    else
        out = v.s.empty() ? "0" : v.s;
    return true;
}

bool SkippedAttribute(const std::string& property)
{
    const std::string p = Lower(property);
    static const char* const prefixes[] = {"objecttexturecustom", "objectmaterialcustom", "vehiclecustomization",
                                           "pylons", "ammobox", "dynamicsimulation", "#"};
    for (const char* prefix : prefixes)
        if (p.rfind(prefix, 0) == 0)
            return true;
    return p.size() > 5 && p.compare(p.size() - 5, 5, "_hide") == 0; // animation sources
}

void AttributeCalls(const Node& entity, std::vector<std::string>& parts)
{
    const Node* custom = entity.Class("CustomAttributes");
    if (!custom)
        return;
    for (const auto& k : custom->kids)
    {
        if (!k->isClass || k->isExtern)
            continue;
        const std::string property = k->S("property");
        const std::string expression = k->S("expression");
        if (expression.empty() || SkippedAttribute(property))
            continue;
        const Node* value = k->Class("Value");
        const Node* data = value ? value->Class("data") : nullptr;
        std::string sqf;
        if (!data || !SqfFromData(*data, sqf))
            continue;
        parts.push_back("[this," + sqf + "] call {private _value = _this select 1; _this = _this select 0; " +
                        expression + "}");
    }
}

// Inventory class -> addMagazine / addWeapon. Magazines first, so the weapons load.
void InventoryCalls(const Node& inventory, std::vector<std::string>& parts)
{
    std::vector<std::string> mags;
    std::vector<std::string> weapons;
    for (const char* slot : {"primaryWeapon", "secondaryWeapon", "handgun"})
    {
        const Node* w = inventory.Class(slot);
        if (!w)
            continue;
        const std::string name = w->S("name");
        if (name.empty())
            continue;
        for (const char* muzzle : {"primaryMuzzleMag", "secondaryMuzzleMag"})
            if (const Node* m = w->Class(muzzle))
                if (const std::string mag = m->S("name"); !mag.empty())
                    mags.push_back(mag);
        weapons.push_back(name);
    }
    for (const char* container : {"uniform", "vest", "backpack"})
    {
        const Node* c = inventory.Class(container);
        const Node* cargo = c ? c->Class("MagazineCargo") : nullptr;
        if (!cargo)
            continue;
        for (const Node* item : cargo->Items())
        {
            const std::string mag = item->S("name");
            const long count = std::clamp(item->I("count", 1), 0L, 20L);
            for (long i = 0; i < count && !mag.empty(); i++)
                mags.push_back(mag);
        }
    }
    if (const std::string binocular = inventory.Class("binocular") ? inventory.Class("binocular")->S("name") : "";
        !binocular.empty())
        weapons.push_back(binocular);
    if (mags.empty() && weapons.empty())
        return;
    std::string code = "removeAllWeapons this";
    for (const std::string& m : mags)
        code += "; this addMagazine " + SqfString(m);
    for (const std::string& w : weapons)
        code += "; this addWeapon " + SqfString(w);
    if (!weapons.empty())
        code += "; this selectWeapon " + SqfString(weapons.front());
    parts.push_back(code);
}

std::string JoinInit(const std::vector<std::string>& parts)
{
    std::string out;
    for (std::string p : parts)
    {
        while (!p.empty() && (std::isspace(static_cast<unsigned char>(p.back())) || p.back() == ';'))
            p.pop_back();
        size_t start = 0;
        while (start < p.size() && std::isspace(static_cast<unsigned char>(p[start])))
            start++;
        p = p.substr(start);
        if (p.empty())
            continue;
        out += (out.empty() ? "" : "; ") + p;
    }
    return out;
}

// ---------------------------------------------------------------------------------------
// Eden -> Arma 2 layout
// ---------------------------------------------------------------------------------------

struct CrewSeat
{
    long vehicle = -1;
    long role = 0;
    std::vector<long> turret;
};

struct UnitOut
{
    const Node* node = nullptr;
    long edenId = -1;
    int legacyId = -1;
    bool logic = false;
    bool leader = false;
    std::string player;
    std::vector<int> syncs;
    std::vector<std::string> markers;
    std::vector<std::string> prefix; // init statements ahead of the unit's own
    std::vector<std::string> layers; // the Eden layers it sits in, innermost last
    CrewSeat seat;
};

struct GroupOut
{
    const Node* node = nullptr;
    long edenId = -1;
    std::string side;
    std::vector<UnitOut> units;
    std::vector<const Node*> waypoints;
    std::vector<std::vector<int>> waypointSyncs;
    std::map<long, CrewSeat> crew; // unit eden id -> seat
};

struct TriggerOut
{
    const Node* node = nullptr;
    std::vector<int> syncs;
    int owner = -1;
};

struct Ref
{
    enum Kind
    {
        None,
        Unit,
        Vehicle,
        Waypoint,
        Trigger,
        Group,
        Marker
    } kind = None;
    int a = -1; // group / list index
    int b = -1; // unit / waypoint index
};

class EdenConverter
{
    const EdenEnvironment& _env;
    EdenReport& _report;
    std::vector<GroupOut> _groups;
    std::vector<UnitOut> _vehicles; // class Vehicles (side EMPTY)
    std::vector<const Node*> _markers;
    std::vector<std::vector<std::string>> _markerLayers; // parallel to _markers
    std::vector<TriggerOut> _triggers;
    std::vector<std::string> _layers; // the layers being collected, outermost first
    std::map<long, Ref> _refs;
    std::map<long, std::string> _vehicleNames;
    int _nextSync = 0;

    static const Node* PositionNode(const Node& n)
    {
        if (const Node* p = n.Class("PositionInfo"))
            return p;
        return &n;
    }
    static std::vector<double> Floats(const Node& n, const char* key)
    {
        std::vector<double> out;
        if (const Val* v = n.V(key); v && v->array)
            for (const Val& i : v->items)
                out.push_back(std::strtod(i.s.c_str(), nullptr));
        return out;
    }
    const Node* Attr(const Node& n) const { return n.Class("Attributes"); }
    std::string AttrS(const Node& n, const char* key) const
    {
        const Node* a = Attr(n);
        return a ? a->S(key) : std::string();
    }

    void AddObjectToGroup(GroupOut& g, const Node& item, bool logic)
    {
        UnitOut u;
        u.node = &item;
        u.edenId = item.I("id", -1);
        u.logic = logic;
        u.layers = _layers;
        g.units.push_back(std::move(u));
    }

    void CollectGroupEntities(GroupOut& g, const Node& entities, int depth)
    {
        for (const Node* item : entities.Items())
        {
            const std::string dt = Lower(item->S("dataType"));
            if (dt == "object")
                AddObjectToGroup(g, *item, false);
            else if (dt == "logic")
                AddObjectToGroup(g, *item, true);
            else if (dt == "waypoint")
                g.waypoints.push_back(item);
            else if (dt == "layer" && depth < 16)
            {
                _layers.push_back(item->S("name"));
                if (const Node* e = item->Class("Entities"))
                    CollectGroupEntities(g, *e, depth + 1);
                _layers.pop_back();
            }
            else
                Collect(*item, depth + 1); // markers, triggers... placed inside a group
        }
    }

    void Collect(const Node& item, int depth)
    {
        if (depth > 32)
            return;
        const std::string dt = Lower(item.S("dataType"));
        if (dt == "layer")
        {
            _layers.push_back(item.S("name"));
            if (const Node* e = item.Class("Entities"))
                for (const Node* child : e->Items())
                    Collect(*child, depth + 1);
            _layers.pop_back();
        }
        else if (dt == "group")
        {
            GroupOut g;
            g.node = &item;
            g.edenId = item.I("id", -1);
            g.side = SideName(item.S("side"));
            if (const Node* e = item.Class("Entities"))
                CollectGroupEntities(g, *e, depth);
            if (const Node* crew = item.Class("CrewLinks"))
                if (const Node* links = crew->Class("Links"))
                    for (const Node* link : links->Items())
                    {
                        CrewSeat seat;
                        seat.vehicle = link->I("item1", -1);
                        if (const Node* cd = link->Class("CustomData"))
                        {
                            seat.role = cd->I("role", 0);
                            if (const Val* tp = cd->V("turretPath"); tp && tp->array)
                                for (const Val& v : tp->items)
                                    seat.turret.push_back(std::strtol(v.s.c_str(), nullptr, 10));
                        }
                        g.crew[link->I("item0", -1)] = seat;
                    }
            _groups.push_back(std::move(g));
        }
        else if (dt == "object")
        {
            UnitOut v;
            v.node = &item;
            v.edenId = item.I("id", -1);
            v.layers = _layers;
            _vehicles.push_back(std::move(v));
        }
        else if (dt == "logic")
        {
            GroupOut g;
            g.side = "LOGIC";
            AddObjectToGroup(g, item, true);
            _groups.push_back(std::move(g));
        }
        else if (dt == "marker")
        {
            if (Lower(item.S("markerType")) == "polyline")
                _report.skipped++;
            else
            {
                _markers.push_back(&item);
                _markerLayers.push_back(_layers);
            }
        }
        else if (dt == "trigger")
        {
            TriggerOut t;
            t.node = &item;
            _triggers.push_back(t);
        }
        else
            _report.skipped++; // comments, stray waypoints
    }

    // Vehicles that a group's crew sits in leave the group for class Vehicles.
    void SeparateVehicles()
    {
        for (GroupOut& g : _groups)
        {
            std::set<long> vehicleIds;
            for (const auto& [unit, seat] : g.crew)
                vehicleIds.insert(seat.vehicle);
            std::vector<UnitOut> kept;
            for (UnitOut& u : g.units)
            {
                if (!u.logic && vehicleIds.count(u.edenId))
                    _vehicles.push_back(std::move(u));
                else
                    kept.push_back(std::move(u));
            }
            g.units = std::move(kept);
            for (UnitOut& u : g.units)
                if (const auto it = g.crew.find(u.edenId); it != g.crew.end())
                    u.seat = it->second;
        }
    }

    void AssignIds()
    {
        int next = 0;
        for (size_t gi = 0; gi < _groups.size(); gi++)
        {
            GroupOut& g = _groups[gi];
            if (g.edenId >= 0)
                _refs[g.edenId] = {Ref::Group, static_cast<int>(gi), -1};
            for (size_t ui = 0; ui < g.units.size(); ui++)
            {
                g.units[ui].legacyId = next++;
                if (g.units[ui].edenId >= 0)
                    _refs[g.units[ui].edenId] = {Ref::Unit, static_cast<int>(gi), static_cast<int>(ui)};
            }
            g.waypointSyncs.resize(g.waypoints.size());
            for (size_t wi = 0; wi < g.waypoints.size(); wi++)
                if (const long id = g.waypoints[wi]->I("id", -1); id >= 0)
                    _refs[id] = {Ref::Waypoint, static_cast<int>(gi), static_cast<int>(wi)};
        }
        for (size_t vi = 0; vi < _vehicles.size(); vi++)
        {
            _vehicles[vi].legacyId = next++;
            if (_vehicles[vi].edenId >= 0)
                _refs[_vehicles[vi].edenId] = {Ref::Vehicle, static_cast<int>(vi), -1};
        }
        for (size_t ti = 0; ti < _triggers.size(); ti++)
            if (const long id = _triggers[ti].node->I("id", -1); id >= 0)
                _refs[id] = {Ref::Trigger, static_cast<int>(ti), -1};
        for (size_t mi = 0; mi < _markers.size(); mi++)
            if (const long id = _markers[mi]->I("id", -1); id >= 0)
                _refs[id] = {Ref::Marker, static_cast<int>(mi), -1};
    }

    int LeaderIndex(const GroupOut& g) const
    {
        for (size_t i = 0; i < g.units.size(); i++)
            if (g.units[i].node->I("flags", 0) & 2)
                return static_cast<int>(i);
        return g.units.empty() ? -1 : 0;
    }

    UnitOut* UnitFor(const Ref& r)
    {
        if (r.kind == Ref::Unit)
            return &_groups[static_cast<size_t>(r.a)].units[static_cast<size_t>(r.b)];
        if (r.kind == Ref::Vehicle)
            return &_vehicles[static_cast<size_t>(r.a)];
        if (r.kind == Ref::Group)
        {
            GroupOut& g = _groups[static_cast<size_t>(r.a)];
            const int l = LeaderIndex(g);
            return l >= 0 ? &g.units[static_cast<size_t>(l)] : nullptr;
        }
        return nullptr;
    }

    // The list an entity's synchronisations go into (a group's are its leader's); null for
    // entities that cannot be synchronised here (markers, logics left out, ...).
    std::vector<int>* SyncList(long edenId)
    {
        const auto it = _refs.find(edenId);
        if (it == _refs.end())
            return nullptr;
        const Ref& r = it->second;
        if (r.kind == Ref::Waypoint)
            return &_groups[static_cast<size_t>(r.a)].waypointSyncs[static_cast<size_t>(r.b)];
        if (r.kind == Ref::Trigger)
            return &_triggers[static_cast<size_t>(r.a)].syncs;
        if (UnitOut* u = UnitFor(r))
            return &u->syncs;
        return nullptr;
    }

    void ApplyConnections(const Node* connections)
    {
        const Node* links = connections ? connections->Class("Links") : nullptr;
        if (!links)
            return;
        for (const Node* link : links->Items())
        {
            const long a = link->I("item0", -1), b = link->I("item1", -1);
            const Node* cd = link->Class("CustomData");
            const std::string type = Lower(cd ? cd->S("type") : std::string());
            if (type == "sync" || type == "waypointactivation")
            {
                // Both ends or neither: a synchronisation the engine sees with one member is
                // an error in its mission loader (ArcadeTemplate: "syncCounts >= 2"). A group
                // linked to its own leader is one item too.
                std::vector<int>* la = SyncList(a);
                std::vector<int>* lb = SyncList(b);
                if (!la || !lb || la == lb)
                    continue;
                const int sync = _nextSync++;
                la->push_back(sync);
                lb->push_back(sync);
                _report.links++;
            }
            else if (type == "triggerowner")
            {
                long trigger = a, owner = b;
                if (_refs.count(a) && _refs[a].kind != Ref::Trigger)
                    std::swap(trigger, owner);
                const auto rt = _refs.find(trigger), ro = _refs.find(owner);
                if (rt == _refs.end() || ro == _refs.end() || rt->second.kind != Ref::Trigger)
                    continue;
                if (const UnitOut* u = UnitFor(ro->second))
                {
                    _triggers[static_cast<size_t>(rt->second.a)].owner = u->legacyId;
                    _report.links++;
                }
            }
            else if (type == "randomstart")
            {
                long unit = a, marker = b;
                if (_refs.count(a) && _refs[a].kind == Ref::Marker)
                    std::swap(unit, marker);
                const auto ru = _refs.find(unit), rm = _refs.find(marker);
                if (ru == _refs.end() || rm == _refs.end() || rm->second.kind != Ref::Marker)
                    continue;
                if (UnitOut* u = UnitFor(ru->second))
                {
                    u->markers.push_back(_markers[static_cast<size_t>(rm->second.a)]->S("name"));
                    _report.links++;
                }
            }
        }
    }

    std::string VehicleName(long edenId)
    {
        if (const auto it = _vehicleNames.find(edenId); it != _vehicleNames.end())
            return it->second;
        std::string name;
        if (const auto r = _refs.find(edenId); r != _refs.end() && r->second.kind == Ref::Vehicle)
            name = AttrS(*_vehicles[static_cast<size_t>(r->second.a)].node, "name");
        if (name.empty())
            name = "op_eden_v" + std::to_string(edenId);
        _vehicleNames[edenId] = name;
        return name;
    }

    // getMissionLayerEntities: each object registers in the lists of its layers
    // (op_eden_layer_<name>), markers through one logic of their own.
    static std::string LayerVariable(const std::string& layer, bool markers)
    {
        return std::string(markers ? "op_eden_layer_markers_" : "op_eden_layer_") + Lower(layer);
    }
    static void RegisterInLayers(UnitOut& u)
    {
        for (const std::string& layer : u.layers)
        {
            // pushBack into one shared array: a layer of thousands of objects stays linear
            const std::string var = SqfString(LayerVariable(layer, false));
            u.prefix.push_back("if (isNil {missionNamespace getVariable " + var +
                               "}) then {missionNamespace setVariable [" + var + ", []]}; (missionNamespace getVariable " +
                               var + ") pushBack this");
        }
    }
    void AddMarkerLayerLogic()
    {
        std::map<std::string, std::vector<std::string>> byLayer;
        for (size_t i = 0; i < _markers.size(); i++)
            for (const std::string& layer : _markerLayers[i])
                byLayer[layer].push_back(_markers[i]->S("name"));
        if (byLayer.empty())
            return;
        std::string init;
        for (const auto& [layer, names] : byLayer)
        {
            std::string list = "[";
            for (size_t i = 0; i < names.size(); i++)
                list += (i ? "," : "") + SqfString(names[i]);
            init += (init.empty() ? "" : "; ") + std::string("missionNamespace setVariable [") +
                    SqfString(LayerVariable(layer, true)) + ", " + list + "]]";
        }
        _layerLogicInit = init;
    }
    std::string _layerLogicInit;

    void PrepareUnits()
    {
        for (GroupOut& g : _groups)
            for (UnitOut& u : g.units)
                RegisterInLayers(u);
        for (UnitOut& v : _vehicles)
            RegisterInLayers(v);
        // the player: isPlayer, else the first playable unit
        bool havePlayer = false;
        for (GroupOut& g : _groups)
            for (UnitOut& u : g.units)
                if (const Node* a = Attr(*u.node); a && a->I("isPlayer", 0))
                {
                    u.player = "PLAYER COMMANDER";
                    havePlayer = true;
                }
        for (GroupOut& g : _groups)
            for (UnitOut& u : g.units)
                if (const Node* a = Attr(*u.node); a && a->I("isPlayable", 0) && u.player.empty())
                {
                    u.player = havePlayer ? "PLAY CDG" : "PLAYER COMMANDER";
                    havePlayer = true;
                }
        for (GroupOut& g : _groups)
        {
            const int leader = LeaderIndex(g);
            if (leader >= 0)
                g.units[static_cast<size_t>(leader)].leader = true;
            for (UnitOut& u : g.units)
            {
                if (u.seat.vehicle < 0)
                    continue;
                const std::string vehicle = VehicleName(u.seat.vehicle);
                std::string cmd;
                if (u.seat.role == 1)
                    cmd = "moveInDriver";
                else if (u.seat.role == 2 && u.seat.turret.size() == 1 && u.seat.turret[0] == 0)
                    cmd = "moveInGunner";
                else if (u.seat.role == 2 && (u.seat.turret.size() == 2 || (u.seat.turret.size() == 1 && u.seat.turret[0] == 1)))
                    cmd = "moveInCommander";
                else
                    cmd = "moveInCargo";
                u.prefix.push_back("this " + cmd + " " + vehicle);
            }
            // group attributes -> the leader's init line
            if (leader < 0 || !g.node)
                continue;
            UnitOut& l = g.units[static_cast<size_t>(leader)];
            if (const Node* a = Attr(*g.node))
            {
                if (const std::string name = a->S("name"); !name.empty())
                    l.prefix.push_back(name + " = group this");
                static const std::pair<const char*, const char*> settings[] = {{"behaviour", "setBehaviour"},
                                                                               {"combatMode", "setCombatMode"},
                                                                               {"speedMode", "setSpeedMode"},
                                                                               {"formation", "setFormation"}};
                for (const auto& [key, cmd] : settings)
                    if (const std::string v = a->S(key); !v.empty())
                        l.prefix.push_back(std::string("group this ") + cmd + " " + SqfString(v));
                if (const std::string init = a->S("init"); !init.empty())
                    l.prefix.push_back("[group this] call {private _this = _this select 0; " + GroupInitCode(init) + "}");
            }
        }
    }

    void WriteIntel(Writer& w, const Node* intel) const
    {
        if (!intel)
            return;
        w.Open("Intel");
        for (const auto& k : intel->kids)
            if (k->hasValue)
                w.Copy(*k);
        w.Close();
    }

    void WriteUnit(Writer& w, size_t index, const UnitOut& u, const std::string& side, bool emptyVehicle)
    {
        const Node& n = *u.node;
        const Node* a = Attr(n);
        const Node* pos = PositionNode(n);
        w.Open("Item" + std::to_string(index));
        if (const Val* p = pos->V("position"))
            w.Value("position", *p);
        const std::vector<double> angles = Floats(*pos, "angles");
        if (angles.size() >= 2)
        {
            double deg = angles[1] * 180.0 / 3.14159265358979323846;
            deg = std::fmod(deg, 360.0);
            if (deg < 0)
                deg += 360.0;
            w.Raw("azimut", Num(deg));
        }
        if (a && a->Has("placementRadius"))
            w.Raw("placement", a->S("placementRadius"));
        const std::string type = n.S("type");
        bool fly = false;
        if (_env.kind && n.D("atlOffset", 0.0) > 5.0)
            fly = _env.kind(type) == EdenKind::Air;
        w.Str("special", fly ? "FLY" : "NONE");
        w.Raw("id", std::to_string(u.legacyId));
        w.Str("side", side);
        w.Str("vehicle", type.empty() ? std::string(u.logic ? "Logic" : "") : type);
        if (!u.player.empty())
            w.Str("player", u.player);
        if (u.leader && !emptyVehicle)
            w.Raw("leader", "1");
        if (a)
        {
            if (const std::string rank = a->S("rank"); !rank.empty())
                w.Str("rank", rank);
            for (const char* key : {"skill", "health", "fuel", "ammo"})
                if (a->Has(key))
                    w.Raw(key, a->S(key));
        }
        const std::string name = a ? a->S("name") : std::string();
        if (emptyVehicle && name.empty() && _vehicleNames.count(u.edenId))
            w.Str("text", _vehicleNames.at(u.edenId));
        else if (!name.empty())
            w.Str("text", name);
        if (!u.markers.empty())
        {
            std::string list = "{";
            for (size_t i = 0; i < u.markers.size(); i++)
                list += (i ? "," : "") + Quote(u.markers[i]);
            w.Arr("markers", list + "}");
        }
        std::vector<std::string> parts = u.prefix;
        // Arma 3 keeps a soldier's height -- on an upper floor, on a carrier's deck. CWA stands
        // every soldier on the ground (or the sea) at the start, so one the file holds above
        // the terrain (atlOffset) is put back at his height from his own init line, if that is
        // more than 1.5 m above the terrain or the sea: by then the objects he stands on exist
        // (the carrier's deck is built by its base's init line), and he drops onto them.
        if (_env.kind && !emptyVehicle && n.D("atlOffset", 0.0) > 1.0 && _env.kind(type) == EdenKind::Man)
            if (const std::vector<double> p = Floats(*pos, "position"); p.size() >= 3)
            {
                const std::string x = Num(p[0]), y = Num(p[2]), h = Num(p[1]);
                parts.insert(parts.begin(), "if (" + h + " - ((getTerrainHeightASL [" + x + "," + y +
                                                "]) max 0) > 1.5) then {this setPosASL [" + x + "," + y + "," + h +
                                                "]}");
            }
        if (a)
        {
            if (const Node* inv = a->Class("Inventory"))
                InventoryCalls(*inv, parts);
            if (const std::string stance = Lower(a->S("stance")); !stance.empty() && stance != "auto")
                parts.push_back("this setUnitPos " +
                                SqfString(stance == "middle" ? "MIDDLE" : stance == "down" ? "DOWN" : "UP"));
            if (const std::string init = a->S("init"); !init.empty())
                parts.push_back(init);
        }
        AttributeCalls(n, parts);
        if (const std::string init = JoinInit(parts); !init.empty())
            w.Str("init", init);
        if (!u.syncs.empty())
            w.Arr("synchronizations", IntArray(u.syncs));
        if (a)
        {
            if (a->Has("presence"))
                w.Raw("presence", a->S("presence"));
            if (const std::string cond = a->S("presenceCondition"); !cond.empty())
                w.Str("presenceCondition", cond);
        }
        w.Close();
    }

    void WriteWaypoint(Writer& w, size_t index, const Node& n, const std::vector<int>& syncs)
    {
        static const char* const copied[] = {"position", "placement", "combatMode", "formation", "speed",
                                             "combat",   "description", "expCond", "expActiv", "script",
                                             "timeoutMin", "timeoutMid", "timeoutMax", "showWP"};
        w.Open("Item" + std::to_string(index));
        for (const char* key : copied)
            if (const Node* k = n.Find(key); k && k->hasValue)
            {
                if (std::strcmp(key, "position") == 0)
                    w.Value("position", k->value);
                else
                    w.Copy(*k);
            }
        w.Str("type", WaypointType(n.S("type")));
        if (const Node* effects = n.Class("Effects"))
            w.Copy(*effects);
        if (!syncs.empty())
            w.Arr("synchronizations", IntArray(syncs));
        w.Close();
    }

    void WriteTrigger(Writer& w, size_t index, const TriggerOut& t)
    {
        const Node& n = *t.node;
        const Node* a = Attr(n);
        static const Node empty;
        if (!a)
            a = &empty;
        w.Open("Item" + std::to_string(index));
        if (const Val* p = n.V("position"))
            w.Value("position", *p);
        w.Raw("a", a->Has("sizeA") ? a->S("sizeA") : "0");
        w.Raw("b", a->Has("sizeB") ? a->S("sizeB") : "0");
        if (n.Has("angle"))
            w.Raw("angle", Num(n.D("angle", 0.0) * 180.0 / 3.14159265358979323846));
        if (a->I("isRectangle", 0))
            w.Raw("rectangular", "1");
        std::string activation = a->S("activationBy");
        std::string condition = a->S("condition", "this");
        if (IEquals(activation, "ANYPLAYER"))
        {
            activation = "ANY";
            if (condition == "this")
                condition = "this && (player in thisList || vehicle player in thisList)";
        }
        if (t.owner >= 0 && !a->S("activationByOwner").empty())
            activation = a->S("activationByOwner");
        if (!activation.empty())
            w.Str("activationBy", activation);
        if (const std::string type = a->S("activationType"); !type.empty())
            w.Str("activationType", type);
        if (a->I("repeatable", 0))
            w.Raw("repeating", "1");
        if (const Val* timeout = a->V("timeout"); timeout && timeout->array && timeout->items.size() >= 3)
        {
            w.Raw("timeoutMin", timeout->items[0].s);
            w.Raw("timeoutMid", timeout->items[1].s);
            w.Raw("timeoutMax", timeout->items[2].s);
        }
        else
            for (const char* key : {"timeoutMin", "timeoutMid", "timeoutMax"})
                if (a->Has(key))
                    w.Raw(key, a->S(key));
        if (a->I("interuptable", 0) || a->I("interruptable", 0))
            w.Raw("interruptable", "1");
        w.Str("type", a->S("type", "NONE"));
        w.Str("object", n.S("type", "EmptyDetector"));
        w.Str("age", "UNKNOWN");
        if (t.owner >= 0)
            w.Raw("idVehicle", std::to_string(t.owner));
        if (const std::string text = a->S("text"); !text.empty())
            w.Str("text", text);
        if (const std::string name = a->S("name"); !name.empty())
            w.Str("name", name);
        w.Str("expCond", condition);
        if (const std::string act = a->S("onActivation"); !act.empty())
            w.Str("expActiv", act);
        if (const std::string deact = a->S("onDeactivation"); !deact.empty())
            w.Str("expDesactiv", deact);
        static const std::pair<const char*, const char*> effects[] = {
            {"effectCondition", "condition"},     {"effectSound", "sound"}, {"effectVoice", "voice"},
            {"effectSoundEnvironment", "soundEnv"}, {"effectSoundTrigger", "soundDet"},
            {"effectMusic", "track"},             {"effectTitle", "title"}};
        bool anyEffect = false;
        for (const auto& [from, to] : effects)
            anyEffect = anyEffect || !a->S(from).empty();
        if (anyEffect)
        {
            w.Open("Effects");
            for (const auto& [from, to] : effects)
                if (const std::string v = a->S(from); !v.empty())
                    w.Str(to, v);
            if (!a->S("effectTitle").empty())
                w.Str("titleType", "TEXT");
            w.Close();
        }
        if (!t.syncs.empty())
            w.Arr("synchronizations", IntArray(t.syncs));
        w.Close();
    }

    void WriteMarker(Writer& w, size_t index, const Node& n)
    {
        w.Open("Item" + std::to_string(index));
        if (const Val* p = n.V("position"))
            w.Value("position", *p);
        w.Str("name", n.S("name"));
        if (const std::string text = n.S("text"); !text.empty())
            w.Str("text", text);
        const std::string markerType = n.S("markerType");
        const bool area = IEquals(markerType, "RECTANGLE") || IEquals(markerType, "ELLIPSE");
        if (area)
            w.Str("markerType", IEquals(markerType, "RECTANGLE") ? "RECTANGLE" : "ELLIPSE");
        w.Str("type", area ? std::string("Empty") : MarkerTypeFor(n.S("type", "Empty"), _env));
        if (const std::string color = n.S("colorName"); !color.empty())
            w.Str("colorName", ColorFor(color, _env));
        if (const std::string fill = n.S("fillName"); !fill.empty())
            w.Str("fillName", FillFor(fill));
        for (const char* key : {"a", "b", "angle"})
            if (n.Has(key))
                w.Raw(key, n.S(key));
        w.Close();
    }

  public:
    EdenConverter(const EdenEnvironment& env, EdenReport& report) : _env(env), _report(report) {}

    void Scenario(Writer& w, const Node& scenario, const char* className, const Node* addons, const Node* seed)
    {
        _groups.clear();
        _vehicles.clear();
        _markers.clear();
        _markerLayers.clear();
        _layerLogicInit.clear();
        _layers.clear();
        _triggers.clear();
        _refs.clear();
        _vehicleNames.clear();
        _nextSync = 0;
        if (const Node* e = scenario.Class("Entities"))
            for (const Node* item : e->Items())
                Collect(*item, 0);
        SeparateVehicles();
        AssignIds();
        ApplyConnections(scenario.Class("Connections"));
        PrepareUnits();
        AddMarkerLayerLogic();
        // a vehicle someone boards needs a name, also when the mission gave it none
        for (const GroupOut& g : _groups)
            for (const UnitOut& u : g.units)
                if (u.seat.vehicle >= 0)
                    VehicleName(u.seat.vehicle);

        w.Open(className);
        if (addons)
            w.Value("addOns", addons->value);
        w.Arr("addOnsAuto", "{}");
        if (seed)
            w.Raw("randomSeed", ValText(seed->value));
        WriteIntel(w, scenario.Class("Intel"));
        const bool layerLogic = !_layerLogicInit.empty();
        if (!_groups.empty() || layerLogic)
        {
            w.Open("Groups");
            w.Raw("items", std::to_string(_groups.size() + (layerLogic ? 1 : 0)));
            for (size_t gi = 0; gi < _groups.size(); gi++)
            {
                const GroupOut& g = _groups[gi];
                w.Open("Item" + std::to_string(gi));
                w.Str("side", g.side);
                w.Open("Vehicles");
                w.Raw("items", std::to_string(g.units.size()));
                for (size_t ui = 0; ui < g.units.size(); ui++)
                {
                    const UnitOut& u = g.units[ui];
                    WriteUnit(w, ui, u, u.logic ? std::string("LOGIC") : g.side, false);
                    _report.units++;
                    if (u.logic)
                        _report.logics++;
                }
                w.Close();
                if (!g.waypoints.empty())
                {
                    w.Open("Waypoints");
                    w.Raw("items", std::to_string(g.waypoints.size()));
                    for (size_t wi = 0; wi < g.waypoints.size(); wi++)
                        WriteWaypoint(w, wi, *g.waypoints[wi], g.waypointSyncs[wi]);
                    w.Close();
                    _report.waypoints += static_cast<int>(g.waypoints.size());
                }
                w.Close();
                _report.groups++;
            }
            if (layerLogic)
            {
                // the markers' layer lists, set by a logic of its own
                int nextId = 0;
                for (const GroupOut& g : _groups)
                    for (const UnitOut& u : g.units)
                        nextId = std::max(nextId, u.legacyId + 1);
                for (const UnitOut& v : _vehicles)
                    nextId = std::max(nextId, v.legacyId + 1);
                w.Open("Item" + std::to_string(_groups.size()));
                w.Str("side", "LOGIC");
                w.Open("Vehicles");
                w.Raw("items", "1");
                w.Open("Item0");
                w.Arr("position", "{0,0,0}");
                w.Str("special", "NONE");
                w.Raw("id", std::to_string(nextId));
                w.Str("side", "LOGIC");
                w.Str("vehicle", "Logic");
                w.Raw("leader", "1");
                w.Str("init", _layerLogicInit);
                w.Close();
                w.Close();
                w.Close();
            }
            w.Close();
        }
        if (!_vehicles.empty())
        {
            w.Open("Vehicles");
            w.Raw("items", std::to_string(_vehicles.size()));
            for (size_t vi = 0; vi < _vehicles.size(); vi++)
                WriteUnit(w, vi, _vehicles[vi], "EMPTY", true);
            w.Close();
            _report.vehicles += static_cast<int>(_vehicles.size());
        }
        if (!_markers.empty())
        {
            w.Open("Markers");
            w.Raw("items", std::to_string(_markers.size()));
            for (size_t mi = 0; mi < _markers.size(); mi++)
                WriteMarker(w, mi, *_markers[mi]);
            w.Close();
            _report.markers += static_cast<int>(_markers.size());
        }
        if (!_triggers.empty())
        {
            w.Open("Sensors");
            w.Raw("items", std::to_string(_triggers.size()));
            for (size_t ti = 0; ti < _triggers.size(); ti++)
                WriteTrigger(w, ti, _triggers[ti]);
            w.Close();
            _report.triggers += static_cast<int>(_triggers.size());
        }
        w.Close();
    }
};

// ---------------------------------------------------------------------------------------
// Arma 2 layout: pairwise synchronisations -> groups of two
// ---------------------------------------------------------------------------------------

void CollectSyncNodes(Node& n, std::vector<Node*>& out, int depth)
{
    if (depth > 64)
        return;
    if (n.Find("syncId") || n.Find("synchronizations"))
        if (n.isClass)
            out.push_back(&n);
    for (auto& k : n.kids)
        if (k->isClass && !k->isExtern)
            CollectSyncNodes(*k, out, depth + 1);
}

bool RewritePairwiseSyncs(Node& scenario)
{
    std::vector<Node*> nodes;
    CollectSyncNodes(scenario, nodes, 0);
    std::map<long, Node*> bySyncId;
    for (Node* n : nodes)
        if (n->Has("syncId"))
            bySyncId[n->I("syncId", -1)] = n;
    if (bySyncId.empty())
        return false; // the engine's own layout: synchronizations are already groups
    std::map<std::pair<long, long>, int> edges;
    std::map<Node*, std::vector<int>> groups;
    for (Node* n : nodes)
    {
        const long self = n->I("syncId", -1);
        const Val* list = n->V("synchronizations");
        if (self < 0 || !list || !list->array)
            continue;
        for (const Val& item : list->items)
        {
            const long other = std::strtol(item.s.c_str(), nullptr, 10);
            const auto partner = bySyncId.find(other);
            if (partner == bySyncId.end() || other == self)
                continue;
            const std::pair<long, long> key{std::min(self, other), std::max(self, other)};
            auto [it, inserted] = edges.emplace(key, static_cast<int>(edges.size()));
            if (inserted)
            {
                groups[n].push_back(it->second);
                groups[partner->second].push_back(it->second);
            }
        }
    }
    for (Node* n : nodes)
    {
        // drop syncId and the pairwise list, then write the groups
        n->kids.erase(std::remove_if(n->kids.begin(), n->kids.end(),
                                     [](const std::unique_ptr<Node>& k)
                                     { return IEquals(k->name, "syncId") || IEquals(k->name, "synchronizations"); }),
                      n->kids.end());
        const auto it = groups.find(n);
        if (it == groups.end())
            continue;
        auto entry = std::make_unique<Node>();
        entry->name = "synchronizations";
        entry->hasValue = true;
        entry->value.array = true;
        for (int g : it->second)
        {
            Val v;
            v.s = std::to_string(g);
            entry->value.items.push_back(v);
        }
        n->kids.push_back(std::move(entry));
    }
    return true;
}
} // namespace

bool ConvertArma3MissionText(const std::string& in, std::string& out, EdenReport& report, const EdenEnvironment& env)
{
    out.clear();
    report = EdenReport{};
    Parser parser(in);
    std::unique_ptr<Node> root = parser.Parse();
    Node* mission = root->Find("Mission");
    bool anyEntities = mission && mission->isClass && mission->Class("Entities");
    for (const char* scenario : {"Intro", "OutroWin", "OutroLoose"})
        if (const Node* s = root->Class(scenario); s && s->Class("Entities"))
            anyEntities = true;
    if ((!mission || !mission->isClass) && !anyEntities)
        return false;
    const bool eden = anyEntities || root->I("version", 0) >= 50;
    report.eden = eden;
    Writer w;
    if (!eden)
    {
        bool changed = false;
        for (const char* scenario : {"Mission", "Intro", "OutroWin", "OutroLoose"})
            if (Node* s = root->Find(scenario); s && s->isClass && !s->isExtern)
                changed = RewritePairwiseSyncs(*s) || changed;
        if (!changed)
            return false;
        for (const auto& k : root->kids)
            w.Copy(*k);
        out = std::move(w.Text());
        report.changed = true;
        return true;
    }
    w.Raw("version", "12");
    EdenConverter converter(env, report);
    const Node* addons = root->Find("addons");
    const Node* seed = root->Find("randomSeed");
    static const Node emptyMission;
    converter.Scenario(w, mission && mission->isClass ? *mission : emptyMission, "Mission",
                       addons && addons->hasValue ? addons : nullptr, seed && seed->hasValue ? seed : nullptr);
    for (const char* scenario : {"Intro", "OutroWin", "OutroLoose"})
        if (const Node* s = root->Class(scenario))
            converter.Scenario(w, *s, scenario, nullptr, nullptr);
    out = std::move(w.Text());
    report.changed = true;
    return true;
}
} // namespace Poseidon::Arma1Compat
