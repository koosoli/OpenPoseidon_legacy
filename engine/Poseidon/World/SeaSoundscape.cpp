// (OP) Tidewater's sea sounds (owner, 2026-10-19: "port the ocean and wave sounds").
//
// A port of the sea half of Tidewater's src/audio/SoundScape.js (dgreenheck/tidewater; the recordings are CC0 field
// recordings from Freesound, see the pack's sound/CREDITS.md), on OFP's sound scene:
// * Surf, wave by wave. Tidewater mirrors its shore waves' phase at a row of stations along the waterline near the
//   listener and plays, per wave, a crash where it breaks, the wash when its bore runs up the sand and the backwash
//   when the swash drains back. OFP's sea has no shore waves to mirror, so the stations are found by walking the
//   terrain out from the listener to the waterline, and each keeps its own wave clock (Tidewater's 9 s period,
//   jittered); the three events follow each wave as in Tidewater (crash 6-14 m out, wash 1.8 s later on the sand,
//   backwash 5 s after the crash). Each a random slice of the recordings, placed where it happens.
// * The distant surf bed: Tidewater's level at 50 m from the shore; here three 3D loops on the nearest stretches of
//   shore, so the sea is heard from where it is (Tidewater's bed is unpanned).
// * Gulls and terns calling over the shore by day (Tidewater's calls where its wildlife has no bird to call from).
// * Levels: Tidewater's mixer targets (loudness of each sound in its reference situation) minus each file's measured
//   loudness, as in Tidewater; config class CfgTidewaterSounds holds both (generate_tidewater_whale.py) and the
//   reference that maps them onto OFP's volume scale.

#include <Poseidon/World/SeaSoundscape.hpp>

#include <Poseidon/Audio/IAudioSystem.hpp>
#include <Poseidon/Audio/SoundScene.hpp>
#include <Poseidon/World/Entities/Infantry/SwimView.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>

#include <Random/randomGen.hpp>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace Poseidon
{
using namespace Foundation;

namespace
{
struct SeaSound
{
    std::vector<RString> files;
    std::vector<float> lufs;
    float target = -30;
    float distance = 10; // where Tidewater's level applies (m)
};

struct Bank
{
    bool loaded = false;
    bool present = false;
    float reference = 0;
    std::unordered_map<std::string, SeaSound> sounds;
};

Bank& GetBank()
{
    static Bank bank;
    if (bank.loaded)
        return bank;
    bank.loaded = true;
    const ParamEntry* cfg = Pars.FindEntry("CfgTidewaterSounds");
    if (!cfg || !cfg->IsClass())
        return bank;
    bank.present = true;
    if (const ParamEntry* r = cfg->FindEntry("reference"))
        bank.reference = *r;
    for (int i = 0; i < cfg->GetEntryCount(); i++)
    {
        const ParamEntry& e = cfg->GetEntry(i);
        if (!e.IsClass())
            continue;
        SeaSound s;
        const ParamEntry& files = e >> "files";
        const ParamEntry& lufs = e >> "lufs";
        for (int k = 0; k < files.GetSize(); k++)
        {
            s.files.push_back(GetSoundName(files[k])); // (as config sounds are: the leading backslash went to the file cache as is)
            s.lufs.push_back(k < lufs.GetSize() ? float(lufs[k]) : -23.0f);
        }
        s.target = e >> "target";
        if (const ParamEntry* dist = e.FindEntry("distance"))
            s.distance = *dist;
        std::string name = (const char*)e.GetName();
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        bank.sounds[name] = std::move(s);
    }
    LOG_INFO(Audio, "Tidewater sea sounds: {} sounds", bank.sounds.size());
    return bank;
}

const SeaSound* Find(const char* name)
{
    Bank& b = GetBank();
    if (!b.present)
        return nullptr;
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    const auto it = b.sounds.find(key);
    return it == b.sounds.end() || it->second.files.empty() ? nullptr : &it->second;
}

float Level(const SeaSound& s, int file)
{
    return std::pow(10.0f, (s.target - s.lufs[file] + GetBank().reference) / 20.0f);
}

// OFP's 3D "volume" is a distance: full level out to 30 m x sqrt(volume), then 1/d (DX8Reference.hpp
// referenceDistance, the same on OpenAL and Steam Audio). The volume whose falloff gives Tidewater's level L at
// Tidewater's distance r: L = 30 sqrt(v) / r.
float Volume3D(float level, float distance)
{
    return Square(level * distance / 30.0f);
}

// ---- the surf

constexpr int Directions = 16;
constexpr float SearchStep = 4.0f, SearchRange = 300.0f;
constexpr float WavePeriod = 9.0f; // Tidewater's ShoreParams period (s)
constexpr float EventRange = 120.0f; // waves breaking further away are only the bed

struct Station
{
    bool valid = false;
    Vector3 shore = VZero; // on the waterline
    Vector3 seaward = VZero;
    float nextWave = -1;
};

struct Pending
{
    float at;
    const char* name;
    Vector3 pos;
    float gainDb;
};

struct Surf
{
    Station stations[Directions];
    std::vector<Pending> pending;
    float nextScan = 0;
    float nearest = 1e9f;
    float nextGull = 6; // Tidewater's first call after 6 s
};

Surf& GetSurf()
{
    static Surf surf;
    return surf;
}

// The waterline out from the listener in each direction (where the terrain crosses the sea level), with real sea
// on its seaward side (not a pond): the shore stations.
void Scan(Surf& s, Vector3Par listener)
{
    const float sea = GLandscape->GetSeaLevel();
    s.nearest = 1e9f;
    const float y0 = GLandscape->SurfaceY(listener.X(), listener.Z()) - sea;
    for (int d = 0; d < Directions; d++)
    {
        const float a = 6.2831853f * d / Directions;
        const Vector3 dir(std::sin(a), 0, std::cos(a));
        Station& st = s.stations[d];
        const bool had = st.valid;
        st.valid = false;
        float prev = y0;
        for (float r = SearchStep; r <= SearchRange; r += SearchStep)
        {
            const Vector3 p = listener + dir * r;
            const float y = GLandscape->SurfaceY(p.X(), p.Z()) - sea;
            if ((prev < 0) != (y < 0))
            {
                const float t = prev / (prev - y);
                Vector3 shore = listener + dir * (r - SearchStep + t * SearchStep);
                const Vector3 seaward = y < 0 ? dir : -dir;
                const Vector3 out = shore + seaward * 20.0f;
                if (sea - GLandscape->SurfaceY(out.X(), out.Z()) > 1.5f)
                {
                    shore[1] = sea;
                    st.shore = shore;
                    st.seaward = seaward;
                    st.valid = true;
                    s.nearest = std::min(s.nearest, Vector3(shore - listener).SizeXZ());
                }
                break;
            }
            prev = y;
        }
        if (st.valid && !had)
            st.nextWave = Glob.time.toFloat() + GRandGen.RandomValue() * WavePeriod;
    }
    // stations close to one another hear the same stretch of beach: keep one
    for (int d = 0; d < Directions; d++)
        for (int e = 0; e < d; e++)
            if (s.stations[d].valid && s.stations[e].valid &&
                (s.stations[d].shore - s.stations[e].shore).SquareSizeXZ() < Square(25.0f))
                s.stations[d].valid = false;
}
} // namespace

float SeaSoundVolume(const char* name, float gain)
{
    const SeaSound* s = Find(name);
    return s ? Volume3D(Level(*s, 0) * gain, s->distance) : 0.0f;
}

void PlaySeaSound(const char* name, Vector3Par pos, Vector3Par speed, float gainDb, float freq)
{
    if (!GSoundScene)
        return;
    const SeaSound* s = Find(name);
    if (!s)
        return;
    const int i = std::min((int)(GRandGen.RandomValue() * s->files.size()), (int)s->files.size() - 1);
    const float vol = Volume3D(Level(*s, i) * std::pow(10.0f, gainDb / 20.0f), s->distance);
    IWave* wave = GSoundScene->OpenAndPlayOnce(s->files[i], pos, speed, vol, freq);
    if (wave)
    {
        GSoundScene->SimulateSpeedOfSound(wave);
        GSoundScene->AddSound(wave);
    }
    static const bool diag = std::getenv("POSEIDON_SEA_SOUND_DIAG") != nullptr;
    if (diag)
        LOG_INFO(Audio, "Sea sound {} ({}): vol {:.2f} freq {:.2f} at {:.0f},{:.1f},{:.0f}{}", name,
                 (const char*)s->files[i], vol, freq, pos.X(), pos.Y(), pos.Z(), wave ? "" : " -- COULD NOT OPEN");
}

IWave* OpenSeaLoop(const char* name, Vector3Par pos)
{
    const SeaSound* s = Find(name);
    if (!s || !GSoundScene)
        return nullptr;
    return GSoundScene->OpenAndPlay(s->files[0], pos, VZero, 0.0f, 1.0f);
}

// Under water (Tidewater's SoundScape._mix): the mixer muffles everything from above the surface (IAudioSystem::
// SetUnderwater), a hydrophone bed -- snapping shrimp, a low rumble -- fades in, and going under and coming up splash.
// The bed and the splashes are not muffled (Tidewater's `under` and `near` buses).
void UpdateUnderwater(Vector3Par listener)
{
    static float wasUnder = 0;
    static Ref<IWave>* const bed = new Ref<IWave>; // (never destroyed, see the surf beds)
    const float water = SwimWaterSurfaceY(listener.X(), listener.Z());
    const float depth = std::max(water - listener.Y(), 0.0f);
    const float under = std::clamp((water - listener.Y()) / 0.15f, 0.0f, 1.0f);
    if (GSoundsys)
        GSoundsys->SetUnderwater(under, depth);
    const auto play2D = [](const char* name, float gain)
    {
        const SeaSound* s = Find(name);
        if (!s)
            return;
        const int i = std::min((int)(GRandGen.RandomValue() * s->files.size()), (int)s->files.size() - 1);
        if (IWave* wave = GSoundScene->OpenAndPlayOnce2D(s->files[i], Level(*s, i) * gain, 0.95f + GRandGen.RandomValue() * 0.1f, false))
        {
            wave->SetUnmuffled(true);
            GSoundScene->AddSound(wave);
        }
        static const bool diag = std::getenv("POSEIDON_SEA_SOUND_DIAG") != nullptr;
        if (diag)
            LOG_INFO(Audio, "Sea sound {} (2D, unmuffled): vol {:.3f}", name, Level(*s, i) * gain);
    };
    if (under > 0.5f && wasUnder <= 0.5f)
        play2D("submerge", 1);
    else if (under <= 0.5f && wasUnder > 0.5f)
        play2D("emerge", std::pow(10.0f, (-38.0f - -30.0f) / 20.0f)); // (emerge's target is the whale's fluke drip)
    wasUnder = under;
    if (const SeaSound* reef = Find("under_reef"))
    {
        const float vol = Level(*reef, 0) * (0.8f + 0.3f * std::min(depth / 12.0f, 1.0f)) * under;
        if (!*bed && vol > 0.001f)
        {
            *bed = GSoundScene->OpenAndPlay2D(reef->files[0], 0.0f, 1.0f, false);
            if (*bed)
                (*bed)->SetUnmuffled(true);
        }
        if (*bed)
            (*bed)->SetVolume(vol, 1.0f);
    }
}

void UpdateSeaSoundscape(Vector3Par listener, float deltaT)
{
    if (!GLandscape || !GSoundScene || !Find("surf_crash"))
        return;
    UpdateUnderwater(listener);
    Surf& s = GetSurf();
    const float now = Glob.time.toFloat();
    if (now >= s.nextScan || now < s.nextScan - 5.0f)
    {
        s.nextScan = now + 0.5f;
        Scan(s, listener);
    }
    const float sea = GLandscape->GetSeaLevel();
    const bool under = listener.Y() < sea - 0.3f;

    // the distant surf bed, from where the shore is (owner, 2026-10-19: "if the ocean is behind me i want to hear it
    // that way"): Tidewater's bed level 50 m from the shore, as 3D loops on the three nearest shore stations, which
    // OpenAL / Steam Audio place around the listener; their 1/d falloff is the bed fading inland
    {
        // (never destroyed: a static Ref released its wave at exit, after the sound system was gone -- a crash in
        // Mtx_lock on every quit, owner's smoke test 2026-10-19)
        static Ref<IWave>* const beds = new Ref<IWave>[3];
        const Station* near[3] = {};
        float nd[3] = {1e9f, 1e9f, 1e9f};
        for (const Station& st : s.stations)
        {
            if (!st.valid)
                continue;
            const float d = (st.shore - listener).SizeXZ();
            for (int k = 0; k < 3; k++)
                if (d < nd[k])
                {
                    for (int j = 2; j > k; j--)
                        near[j] = near[j - 1], nd[j] = nd[j - 1];
                    near[k] = &st;
                    nd[k] = d;
                    break;
                }
        }
        int count = 0;
        for (const Station* st : near)
            count += st ? 1 : 0;
        for (int k = 0; k < 3; k++)
        {
            if (!near[k])
            {
                if (beds[k])
                    beds[k]->SetVolume(0.0f, 1.0f);
                continue;
            }
            if (!beds[k])
                beds[k] = OpenSeaLoop("surf_far", near[k]->shore);
            if (!beds[k])
                continue;
            // (the sound of the surf rises a little above the water; the three share the bed's level)
            beds[k]->SetPosition(near[k]->shore + VUp * 1.5f, VZero);
            beds[k]->SetVolume(SeaSoundVolume("surf_far", (under ? 0.15f : 1.0f) / std::sqrt((float)count)), 1.0f);
        }
    }

    // wave by wave at each station; only the nearest few play their waves (a long beach in view found a dozen
    // stations and broke more than a wave a second; Tidewater limits its crashes to 7 voices)
    float ranked[Directions];
    int n = 0;
    for (const Station& st : s.stations)
        if (st.valid)
            ranked[n++] = (st.shore - listener).SizeXZ();
    std::sort(ranked, ranked + n);
    const float cutoff = n > 5 ? ranked[4] : 1e9f;
    for (Station& st : s.stations)
    {
        if (!st.valid || st.nextWave < 0 || now < st.nextWave)
            continue;
        st.nextWave = now + WavePeriod * (0.85f + 0.3f * GRandGen.RandomValue());
        const float d = (st.shore - listener).SizeXZ();
        if (d > EventRange || d > cutoff || under)
            continue;
        const float size = (GRandGen.RandomValue() - 0.5f) * 6.0f; // Tidewater: +-3 dB, the waves' heights
        const Vector3 brk = st.shore + st.seaward * (6.0f + 8.0f * GRandGen.RandomValue()) + VUp * 0.4f;
        // (a little along the beach each time: a wave breaks along the shore, not at one point)
        const Vector3 along(st.seaward.Z(), 0, -st.seaward.X());
        const Vector3 shift = along * ((GRandGen.RandomValue() - 0.5f) * 16.0f);
        s.pending.push_back({now, "surf_crash", brk + shift, size});
        s.pending.push_back({now + 1.8f, "surf_wash", st.shore + shift + VUp * 0.2f, size * 0.5f});
        s.pending.push_back({now + 5.0f, "surf_backwash", st.shore + st.seaward * 2.0f + shift, size * 0.5f});
    }
    // gulls (and now and then a tern) calling over the shore by day: Tidewater's call 25-80 m out and 6-25 m up,
    // every 7-25 s, rarer the further from the shore (owner, 2026-10-19: "also port sound for the seagulls")
    if (s.nextGull > now + 60.0f)
        s.nextGull = now + 6.0f;
    if (now >= s.nextGull)
    {
        const float shore = std::min(s.nearest, SearchRange);
        s.nextGull = now + 7.0f + GRandGen.RandomValue() * 18.0f + shore * 0.05f;
        const float tod = Glob.clock.GetTimeOfDay();
        if (!under && s.nearest < SearchRange && tod > 0.22f && tod < 0.86f)
        {
            const float a = GRandGen.RandomValue() * 6.2831853f;
            const float d = 25.0f + GRandGen.RandomValue() * 55.0f;
            const Vector3 at(listener.X() + std::cos(a) * d, listener.Y() + 6 + GRandGen.RandomValue() * 19,
                             listener.Z() + std::sin(a) * d);
            const bool tern = GRandGen.RandomValue() < 0.3f;
            PlaySeaSound(tern ? "tern" : "gull", at, VZero, (GRandGen.RandomValue() - 0.5f) * 4,
                         0.93f + GRandGen.RandomValue() * 0.14f);
        }
    }

    for (size_t i = 0; i < s.pending.size();)
    {
        const Pending p = s.pending[i];
        if (now >= p.at)
        {
            PlaySeaSound(p.name, p.pos, VZero, p.gainDb, 0.92f + 0.12f * GRandGen.RandomValue());
            s.pending[i] = s.pending.back();
            s.pending.pop_back();
        }
        else if (p.at > now + 30.0f) // the clock went back (a new mission)
        {
            s.pending[i] = s.pending.back();
            s.pending.pop_back();
        }
        else
        {
            i++;
        }
    }
}

} // namespace Poseidon
