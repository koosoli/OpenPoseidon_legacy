// DIAG-001: the diagnostics layer ported from Malprave (Dev/Diag/OpDiag*).
//
// What is checked here is the part with logic that can be wrong without a mission running:
//   * the JSON text helpers every event and harness response is built from (escaping, non-finite numbers,
//     getPos order),
//   * the stuck / spin detector (OpDiagStuck.hpp) driven with made-up samples,
//   * the event writer end to end: Start -> events -> Shutdown, every line of events.jsonl and summary.json
//     checked as JSON by a small validator below, event counts and category filtering,
//   * diag_step's reconciliation with the dev pause: ticks are let through one per frame and the pause is
//     re-engaged after each.
// The engine hooks and the harness commands need a world and are not exercised here.

#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Dev/Diag/OpDiagJson.hpp>
#include <Poseidon/Dev/Diag/OpDiagStuck.hpp>

#if POSEIDON_DIAG
#include <Poseidon/Dev/Diag/DiagPause.hpp>
#include <Poseidon/Dev/Diag/OpDiag.hpp>
#endif

#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace Json = Poseidon::Dev::OpDiag::Json;
using Poseidon::Dev::OpDiag::StuckSample;
using Poseidon::Dev::OpDiag::StuckTrack;
using Poseidon::Dev::OpDiag::StuckUpdate;

namespace
{
// ---- a minimal strict JSON validator (RFC 8259 grammar, no extensions) --------------------------------
struct JsonCheck
{
    const std::string& s;
    size_t i = 0;

    void Ws()
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            i++;
    }
    bool Lit(const char* w)
    {
        const std::string word(w);
        if (s.compare(i, word.size(), word) != 0)
            return false;
        i += word.size();
        return true;
    }
    bool String()
    {
        if (i >= s.size() || s[i] != '"')
            return false;
        i++;
        while (i < s.size())
        {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c == '"')
            {
                i++;
                return true;
            }
            if (c < 0x20)
                return false;
            if (c == '\\')
            {
                i++;
                if (i >= s.size())
                    return false;
                const char e = s[i];
                if (e == 'u')
                {
                    for (int k = 1; k <= 4; k++)
                        if (i + k >= s.size() || !std::isxdigit(static_cast<unsigned char>(s[i + k])))
                            return false;
                    i += 4;
                }
                else if (std::string("\"\\/bfnrt").find(e) == std::string::npos)
                    return false;
            }
            i++;
        }
        return false;
    }
    bool Number()
    {
        const size_t start = i;
        if (i < s.size() && s[i] == '-')
            i++;
        if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i])))
            return false;
        if (s[i] == '0')
            i++;
        else
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
                i++;
        if (i < s.size() && s[i] == '.')
        {
            i++;
            if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i])))
                return false;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
                i++;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E'))
        {
            i++;
            if (i < s.size() && (s[i] == '+' || s[i] == '-'))
                i++;
            if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i])))
                return false;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
                i++;
        }
        return i > start;
    }
    bool Value()
    {
        Ws();
        if (i >= s.size())
            return false;
        const char c = s[i];
        bool ok = false;
        if (c == '{')
        {
            i++;
            Ws();
            if (i < s.size() && s[i] == '}')
            {
                i++;
                ok = true;
            }
            else
                for (;;)
                {
                    Ws();
                    if (!String())
                        return false;
                    Ws();
                    if (i >= s.size() || s[i] != ':')
                        return false;
                    i++;
                    if (!Value())
                        return false;
                    Ws();
                    if (i < s.size() && s[i] == ',')
                    {
                        i++;
                        continue;
                    }
                    if (i < s.size() && s[i] == '}')
                    {
                        i++;
                        ok = true;
                    }
                    break;
                }
        }
        else if (c == '[')
        {
            i++;
            Ws();
            if (i < s.size() && s[i] == ']')
            {
                i++;
                ok = true;
            }
            else
                for (;;)
                {
                    if (!Value())
                        return false;
                    Ws();
                    if (i < s.size() && s[i] == ',')
                    {
                        i++;
                        continue;
                    }
                    if (i < s.size() && s[i] == ']')
                    {
                        i++;
                        ok = true;
                    }
                    break;
                }
        }
        else if (c == '"')
            ok = String();
        else if (c == 't')
            ok = Lit("true");
        else if (c == 'f')
            ok = Lit("false");
        else if (c == 'n')
            ok = Lit("null");
        else
            ok = Number();
        Ws();
        return ok;
    }
};

bool IsJson(const std::string& text)
{
    JsonCheck c{text};
    return c.Value() && c.i == text.size();
}

std::vector<std::string> ReadLines(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::vector<std::string> out;
    std::string line;
    while (std::getline(f, line))
        out.push_back(line);
    return out;
}

std::string ReadAll(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::filesystem::path FreshDir(const char* tag)
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path p =
        std::filesystem::temp_directory_path() / ("opdiag-" + std::string(tag) + "-" + std::to_string(stamp));
    std::filesystem::remove_all(p);
    return p;
}
} // namespace

// =====================================================================================================
TEST_CASE("DIAG-001 validator: accepts JSON and rejects what is not", "[dev][diag]")
{
    CHECK(IsJson("{\"a\":[1,2.5,-3e2,true,false,null,\"x\\n\"]}"));
    CHECK(IsJson("{}"));
    CHECK_FALSE(IsJson("{\"a\":nan}"));
    CHECK_FALSE(IsJson("{\"a\":1,}"));
    CHECK_FALSE(IsJson("{\"a\":\"tab\there\"}")); // a raw control byte inside a string
    CHECK_FALSE(IsJson("{\"a\":1}x"));
}

TEST_CASE("DIAG-001 JSON: strings are escaped so any engine text stays one valid line", "[dev][diag]")
{
    std::string out;
    Json::AppendString(out, "say \"hi\"\\ now\nnext\tcol\r\x01");
    CHECK(out == "\"say \\\"hi\\\"\\\\ now\\nnext\\tcol\\r\\u0001\"");
    CHECK(IsJson(out));

    std::string none;
    Json::AppendString(none, nullptr);
    CHECK(none == "\"\"");

    // bytes >= 0x80 (UTF-8 or the code page) pass through untouched
    std::string utf8;
    Json::AppendString(utf8, "\xc3\xa9t\xc3\xa9");
    CHECK(utf8 == "\"\xc3\xa9t\xc3\xa9\"");
}

TEST_CASE("DIAG-001 JSON: non-finite numbers are written as null", "[dev][diag]")
{
    std::string out;
    Json::AppendNum(out, std::numeric_limits<double>::quiet_NaN());
    out += ',';
    Json::AppendNum(out, std::numeric_limits<double>::infinity());
    out += ',';
    Json::AppendNum(out, 0.5, "%.3f");
    CHECK(out == "null,null,0.500");
}

TEST_CASE("DIAG-001 JSON: positions are [x, z, height] like getPos", "[dev][diag]")
{
    std::string out;
    Json::AppendPos(out, 100.0f, 7.5f, 2000.25f); // engine X, Y (up), Z
    CHECK(out == "[100.00,2000.25,7.50]");
}

TEST_CASE("DIAG-001 JSON: the response builder makes one object with ok and a newline", "[dev][diag]")
{
    Json::Object o;
    o.Str("name", "a\"b").Num("n", 1.5).Int("i", -3).Bool("b", false).Pos("pos", 1, 2, 3).Raw("raw", "[1,2]");
    o.Raw("empty", std::string());
    const std::string r = o.Ok();
    CHECK(r == "{\"name\":\"a\\\"b\",\"n\":1.5,\"i\":-3,\"b\":false,\"pos\":[1.00,3.00,2.00],\"raw\":[1,2],"
               "\"empty\":null,\"ok\":true}\n");
    CHECK(IsJson(r.substr(0, r.size() - 1)));
}

// =====================================================================================================
TEST_CASE("DIAG-001 stuck: a unit with somewhere to go that does not move is reported once", "[dev][diag]")
{
    StuckTrack t;
    StuckSample s;
    s.x = 10, s.z = 20, s.wantsToMove = true;
    CHECK(StuckUpdate(t, s, 0.0, 5.0f) == 0u); // first sample anchors
    for (int sec = 1; sec < 5; sec++)
        CHECK(StuckUpdate(t, s, sec, 5.0f) == 0u);
    double secOut = 0;
    CHECK(StuckUpdate(t, s, 5.0, 5.0f, &secOut) == Poseidon::Dev::OpDiag::StuckEventStuck);
    CHECK(secOut == 5.0);
    // once per stretch
    CHECK(StuckUpdate(t, s, 6.0, 5.0f) == 0u);
    CHECK(StuckUpdate(t, s, 30.0, 5.0f) == 0u);
    // moving more than 1 m starts a new stretch, after which it can be reported again
    s.x += 2;
    CHECK(StuckUpdate(t, s, 31.0, 5.0f) == 0u);
    CHECK(StuckUpdate(t, s, 36.0, 5.0f) == Poseidon::Dev::OpDiag::StuckEventStuck);
}

TEST_CASE("DIAG-001 stuck: a unit waiting with nowhere to go is not stuck", "[dev][diag]")
{
    StuckTrack t;
    StuckSample s;
    s.wantsToMove = false;
    for (int sec = 0; sec <= 60; sec++)
        CHECK(StuckUpdate(t, s, sec, 5.0f) == 0u);
}

TEST_CASE("DIAG-001 stuck: a slow walker is not flagged", "[dev][diag]")
{
    StuckTrack t;
    StuckSample s;
    s.wantsToMove = true;
    // 0.5 m per second: never 1 m from the anchor within a second, but never still either
    for (int sec = 0; sec <= 60; sec++)
    {
        s.z = 0.5f * static_cast<float>(sec);
        CHECK(StuckUpdate(t, s, sec, 5.0f) == 0u);
    }
}

TEST_CASE("DIAG-001 stuck: turning on the spot is a spin, reported once", "[dev][diag]")
{
    StuckTrack t;
    StuckSample s;
    s.wantsToMove = false;
    unsigned all = 0;
    int spins = 0;
    // 90 degrees per sample, standing still: 540 degrees are passed on the 7th sample
    for (int k = 0; k <= 12; k++)
    {
        const float a = static_cast<float>(k) * 1.5707963f;
        s.dirX = std::sin(a);
        s.dirZ = std::cos(a);
        const unsigned ev = StuckUpdate(t, s, k * 0.5, 5.0f);
        all |= ev;
        if (ev & Poseidon::Dev::OpDiag::StuckEventSpin)
            spins++;
    }
    CHECK(spins == 1);
    CHECK((all & Poseidon::Dev::OpDiag::StuckEventStuck) == 0u);
}

// =====================================================================================================
#if POSEIDON_DIAG
TEST_CASE("DIAG-001 writer: events.jsonl and summary.json are valid JSON with counts", "[dev][diag]")
{
    namespace D = Poseidon::Dev::OpDiag;
    REQUIRE_FALSE(D::Enabled());
    const std::filesystem::path dir = FreshDir("writer");
    REQUIRE(D::Start(dir.string(), std::string(), false, false, 40.0f));
    CHECK(D::Enabled());
    CHECK(D::On("shot"));
    CHECK_FALSE(D::On("moveall")); // opt-in only

    {
        D::Ev e("custom");
        e.Str("text", "line\nwith \"quotes\"").Num("nan", std::nan("")).Int("n", 42).Bool("b", true);
        e.Raw("arr", "[1,2]").Raw("none", nullptr).Obj("obj", nullptr);
    }
    D::Mark("hello");
    D::Mark("again");
    D::OnScriptError("init.sqs, line 3", "Error Type Any, expected Number");
    D::OnBoot(false, "test.Intro", 2);
    D::Shutdown();
    CHECK_FALSE(D::Enabled());
    D::Shutdown(); // twice is harmless

    const std::vector<std::string> lines = ReadLines(dir / "events.jsonl");
    REQUIRE(lines.size() == 7); // start, custom, mark, mark, script, boot, exit
    for (const std::string& l : lines)
    {
        INFO(l);
        CHECK(IsJson(l));
        CHECK(l.rfind("{\"ev\":\"", 0) == 0);
        CHECK(l.find(",\"t\":") != std::string::npos);
        CHECK(l.find(",\"rt\":") != std::string::npos);
        CHECK(l.find(",\"f\":") != std::string::npos);
    }
    CHECK(lines[0].find("\"ev\":\"start\"") != std::string::npos);
    CHECK(lines[1].find("\"nan\":null") != std::string::npos);
    CHECK(lines[1].find("\"none\":null") != std::string::npos);
    CHECK(lines[1].find("\"obj\":null") != std::string::npos);
    CHECK(lines[4].find("\"ev\":\"script\"") != std::string::npos);
    // a failed boot names the problems recorded before it (other tests may have recorded some too: the
    // problem list is process-wide, so only its presence is checked)
    CHECK(lines[5].find("\"ok\":false") != std::string::npos);
    CHECK(lines[5].find("problem(s), first: ") != std::string::npos);
    CHECK(lines[6].find("\"ev\":\"exit\"") != std::string::npos);

    const std::string summary = ReadAll(dir / "summary.json");
    std::string body = summary;
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r'))
        body.pop_back();
    INFO(summary);
    CHECK(IsJson(body));
    CHECK(summary.find("\"mark\":2") != std::string::npos);
    CHECK(summary.find("\"custom\":1") != std::string::npos);
    CHECK(summary.find("\"exit\":1") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("DIAG-001 writer: --diag-cats limits the event types", "[dev][diag]")
{
    namespace D = Poseidon::Dev::OpDiag;
    const std::filesystem::path dir = FreshDir("cats");
    REQUIRE(D::Start(dir.string(), "script,moveall", false, false, 40.0f));
    CHECK(D::On("script"));
    CHECK(D::On("moveall"));
    CHECK_FALSE(D::On("shot"));
    CHECK_FALSE(D::On("boot"));
    D::OnScriptError("pos", "err");
    D::OnBoot(true, "m", -1); // filtered out
    D::Shutdown();
    const std::vector<std::string> lines = ReadLines(dir / "events.jsonl");
    REQUIRE(lines.size() == 3); // start, script, exit (start and exit are always written)
    CHECK(lines[1].find("\"ev\":\"script\"") != std::string::npos);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("DIAG-001 writer: off by default, no file and every hook a no-op", "[dev][diag]")
{
    namespace D = Poseidon::Dev::OpDiag;
    REQUIRE_FALSE(D::Enabled());
    CHECK_FALSE(D::On("shot"));
    CHECK_FALSE(D::DrawEnabled());
    CHECK_FALSE(D::KeepGoingOnScriptError());
    D::Mark("nobody hears this");
    D::OnScriptError("pos", "err");
    {
        D::Ev e("ignored");
        e.Str("a", "b");
        CHECK(e.Buffer().empty());
    }
    CHECK_FALSE(D::Start(std::string(), std::string(), false, false, 40.0f));
}

TEST_CASE("DIAG-001 step: diag_step lets single ticks through the dev pause", "[dev][diag]")
{
    namespace D = Poseidon::Dev::OpDiag;
    // no world in a unit test, so SetDiagPause refuses; engage the flag directly as the panel would
    Poseidon::Dev::g_diagPauseActive = true;
    D::Step(2);
    CHECK(D::Paused());

    CHECK(D::BeginStepTick());
    CHECK_FALSE(Poseidon::Dev::DiagPauseActive()); // the simulation may run this tick
    D::EndStepTick(true);
    CHECK(Poseidon::Dev::DiagPauseActive()); // and is paused again straight after

    CHECK(D::BeginStepTick());
    D::EndStepTick(true);

    CHECK_FALSE(D::BeginStepTick()); // two ticks asked for, two given
    D::EndStepTick(false);
    CHECK(Poseidon::Dev::DiagPauseActive());

    // not paused: a pending step does nothing
    Poseidon::Dev::g_diagPauseActive = false;
    D::Step(1);
    Poseidon::Dev::g_diagPauseActive = false;
    CHECK_FALSE(D::BeginStepTick());
    Poseidon::Dev::g_diagPauseActive = false;
}
#endif
