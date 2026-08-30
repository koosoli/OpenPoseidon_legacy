#include <Poseidon/UI/Options/AudioPage.hpp>
#include <Poseidon/UI/Options/MeterWidget.hpp>
#include <Poseidon/UI/Options/MicTestPage.hpp>
#include <Poseidon/UI/Options/OptionsShell.hpp>

#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Audio/IAudioSystem.hpp>

#include <Poseidon/UI/Locale/Stringtable/Stringtable.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <stddef.h>
#include <Poseidon/Foundation/Framework/AppFrame.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>

namespace Poseidon
{

namespace
{
struct AudioRowText
{
    const char* label;
    const char* desc;
};
const AudioRowText kAudioRows[] = {
    {"STR_DISP_MAIN_OPT_AUDIO_OUTPUT", "STR_DISP_MAIN_OPT_AUDIO_OUTPUT_DESC"},
    {"STR_DISP_MAIN_OPT_AUDIO_MUSIC", "STR_DISP_MAIN_OPT_AUDIO_MUSIC_DESC"},
    {"STR_DISP_MAIN_OPT_AUDIO_EFFECTS", "STR_DISP_MAIN_OPT_AUDIO_EFFECTS_DESC"},
    {"STR_DISP_MAIN_OPT_AUDIO_SPEECH", "STR_DISP_MAIN_OPT_AUDIO_SPEECH_DESC"},
    {"STR_DISP_MAIN_OPT_AUDIO_EAX", "STR_DISP_MAIN_OPT_AUDIO_EAX_DESC"},
    // HRTF row: the key is new and ships in no stringtable yet, so RowLabel/
    // RowDescription special-case it through LocalizeStringWithFallback below.
    {"STR_DISP_MAIN_OPT_AUDIO_HRTF", "STR_DISP_MAIN_OPT_AUDIO_HRTF_DESC"},
    {"STR_DISP_MAIN_OPT_AUDIO_MICROPHONE", "STR_DISP_MAIN_OPT_AUDIO_MICROPHONE_DESC"},
    {"STR_DISP_MAIN_OPT_AUDIO_INPUT_LEVEL", "STR_DISP_MAIN_OPT_AUDIO_INPUT_LEVEL_DESC"},
    {"STR_DISP_MAIN_OPT_AUDIO_TEST_MIC", "STR_DISP_MAIN_OPT_AUDIO_TEST_MIC_DESC"},
};
} // namespace

const char* AudioPage::TitleText() const
{
    return LocalizeString("STR_DISP_MAIN_OPT_AUDIO");
}

const char* AudioPage::CloseLabel()
{
    return LocalizeString("STR_DISP_CLOSE");
}

const char* AudioPage::CloseDescription()
{
    return LocalizeString("STR_DISP_MAIN_OPT_CLOSE_DESC");
}

int AudioPage::VolumeToPercent(float value)
{
    return std::clamp((int)std::lround(value * 10.0f), 0, 100);
}

float AudioPage::PercentToVolume(int percent)
{
    return std::clamp(percent, 0, 100) / 10.0f;
}

void AudioPage::OnReshown(OptionsShell& shell)
{
    m_provider.SetCloseTexts(CloseLabel(), CloseDescription());
    ScrollListPage::OnReshown(shell);
}

const char* AudioPage::AudioProvider::RowLabel(int row) const
{
    static_assert(sizeof(kAudioRows) / sizeof(kAudioRows[0]) == kRowCount, "AudioRow table out of sync with kRowCount");
    if (row == kRowHrtf)
        return LocalizeStringWithFallback(kAudioRows[row].label, "3D Headphone Audio (HRTF)");
    if (row >= 0 && row < kRowCount)
        return LocalizeString(kAudioRows[row].label);
    return "";
}

const char* AudioPage::AudioProvider::RowDescription(int row) const
{
    if (row == kRowHrtf)
        return LocalizeStringWithFallback(
            kAudioRows[row].desc,
            "Binaural rendering for headphones. Auto lets the audio device decide; On forces it; Off disables it.");
    if (row >= 0 && row < kRowCount)
        return LocalizeString(kAudioRows[row].desc);
    return "";
}

void AudioPage::Mount(OptionsShell& shell)
{
    m_audio.ClearUnavailableOutputDevices();
    m_audio.RebuildOutputDeviceList();
    m_audio.RebuildInputDeviceList();

    // Seed the picker from the persisted choice (audio.cfg → SoundSystem
    // at boot → here on mount).  If the saved device is no longer
    // enumerated, RowValue's lookup returns 0 (System default), so
    // re-saving on Unmount will normalize the cfg back to "".
    if (GSoundsys)
        m_audio.SetSelectedInputDevice(GSoundsys->GetSelectedInputDevice());

    // Mute any currently-playing menu music so it doesn't fight the
    // preview waves the focused row plays.  The track keeps streaming
    // (timeline advances) — Unmount restores its original gain so the
    // user comes back to where they would have been mid-track.
    if (GSoundsys)
        GSoundsys->SuppressMusicForPreview();

    // Open mic capture for the input-level meter.  16 kHz / mono
    // matches VoN; small ring buffer keeps latency negligible.  read()
    // drains it each frame in TickMeter.
    if (m_capture.open(nullptr, 16000, 4096))
    {
        m_capture.start();
        m_audio.SetCapture(&m_capture);
    }

    ScrollListPage::Mount(shell);
}

void AudioPage::OnSimulate(OptionsShell& shell)
{
    ScrollListPage::OnSimulate(shell);
    m_audio.TickPreviewLoop();
}

void AudioPage::Unmount(OptionsShell& shell)
{
    // Audio screen is non-destructive — volumes apply live.  Esc does
    // NOT revert; once the user pops the page the live values stick.
    // Persist what changed so a restart picks up the new state.
    if (GSoundsys)
    {
        GSoundsys->TerminatePreview();
        GSoundsys->ResumeMusicForPreview();
        GSoundsys->SaveConfig();
    }

    m_audio.SetCapture(nullptr);
    if (m_capture.isCapturing())
        m_capture.stop();
    if (m_capture.isOpen())
        m_capture.close();
    ScrollListPage::Unmount(shell);
}

int AudioPage::AudioProvider::RowValue(int row) const
{
    if (GSoundsys)
    {
        switch (row)
        {
            case kRowMusic:
                return AudioPage::VolumeToPercent(GSoundsys->GetCDVolume());
            case kRowEffects:
                return AudioPage::VolumeToPercent(GSoundsys->GetWaveVolume());
            case kRowSpeech:
                return AudioPage::VolumeToPercent(GSoundsys->GetSpeechVolume());
            case kRowOutputDev:
            {
                if (m_outputDevs.empty())
                    RebuildOutputDeviceList();
                const std::string cur = GSoundsys->GetCurrentOutputDevice();
                if (cur.empty())
                    return 0; // "System default"
                for (size_t i = 1; i < m_outputDevs.size(); ++i)
                    if (m_outputDevs[i] == cur)
                        return (int)i;
                return 0;
            }
            case kRowEax:
                return GSoundsys->GetEAX() ? 1 : 0;
            case kRowHrtf:
                return static_cast<int>(GSoundsys->GetHrtfMode());
        }
    }
    if (row == kRowMicDev)
    {
        if (m_inputDevs.empty())
            RebuildInputDeviceList();
        if (m_selectedInput.empty())
            return 0; // "System default"
        for (size_t i = 1; i < m_inputDevs.size(); ++i)
            if (m_inputDevs[i] == m_selectedInput)
                return (int)i;
        return 0;
    }
    return m_values[row];
}

void AudioPage::AudioProvider::SetRowValue(int row, int v)
{
    // Volume changes apply live with StartPreview() so the user hears
    // the new level immediately (legacy DisplayOptionsAudio behaviour).
    // UI 0..100 maps to internal 0..10 (the IAudioSystem volume scale).
    if (GSoundsys)
    {
        float vf = AudioPage::PercentToVolume(v);
        switch (row)
        {
            case kRowMusic:
                // Music: long-form sample.  Don't restart on volume
                // change — UpdateMixer (called from SetCDVolume) updates
                // the running wave's _volumeAdjustMusic which feeds
                // WaveOAL::ApplyVolume on the next render tick, so the
                // user hears the new level live without the wave
                // cutting and restarting from the head.  StartCategoryPreview
                // only fires from the focus-dwell path in TickPreviewLoop.
                GSoundsys->SetCDVolume(vf);
                return;
            case kRowEffects:
                GSoundsys->SetWaveVolume(vf);
                GSoundsys->StartCategoryPreview(WaveEffect);
                return;
            case kRowSpeech:
                GSoundsys->SetSpeechVolume(vf);
                GSoundsys->StartCategoryPreview(WaveSpeech);
                return;
            case kRowOutputDev:
            {
                if (m_outputDevs.empty())
                    RebuildOutputDeviceList();
                if (v < 0 || v >= (int)m_outputDevs.size())
                    return;

                const int count = (int)m_outputDevs.size();
                const int curr = RowValue(row);
                int direction = 1;
                if (count > 1)
                {
                    if (v == (curr + count - 1) % count)
                        direction = -1;
                    else if (v == (curr + 1) % count)
                        direction = 1;
                    else
                        direction = (v > curr) ? 1 : -1;
                }

                bool filteredAny = false;
                for (int offset = 0; offset < count; ++offset)
                {
                    int candidate = curr + direction * (offset + 1);
                    while (candidate < 0)
                        candidate += count;
                    candidate %= count;

                    if (candidate != 0 && m_unavailableOutputDevs.count(m_outputDevs[candidate]))
                        continue;

                    const char* devName = (candidate == 0) ? nullptr : m_outputDevs[candidate].c_str();
                    // Device-picker preview is continuous: the wave keeps
                    // playing and SwitchOutputDevice carries the offset
                    // across the device swap (see SoundSystemOAL).
                    GSoundsys->StartDevicePreview();
                    if (GSoundsys->SwitchOutputDevice(devName))
                    {
                        if (filteredAny)
                            RebuildOutputDeviceList();
                        return;
                    }

                    if (candidate != 0)
                    {
                        m_unavailableOutputDevs.insert(m_outputDevs[candidate]);
                        filteredAny = true;
                    }
                }

                if (filteredAny)
                    RebuildOutputDeviceList();
                return;
            }
            case kRowEax:
                GSoundsys->EnableEAX(v != 0);
                // Restart the preview so the user hears wet vs dry the
                // instant they flip the toggle.
                GSoundsys->StartEAXPreview();
                return;
            case kRowHrtf:
                GSoundsys->SetHrtfMode(static_cast<IAudioSystem::HrtfMode>(std::clamp(v, 0, 2)));
                // A 3D effect sample makes the binaural difference audible immediately.
                GSoundsys->StartCategoryPreview(WaveEffect);
                return;
        }
    }
    if (row == kRowMicDev && m_capture)
    {
        if (m_inputDevs.empty())
            RebuildInputDeviceList();
        if (v < 0 || v >= (int)m_inputDevs.size())
            return;
        m_selectedInput = (v == 0) ? std::string() : m_inputDevs[v];
        // Mirror to the audio system so SaveConfig persists this choice
        // alongside the output device — the audio backend doesn't itself
        // open the capture, but it owns the value the cfg file round-trips.
        if (GSoundsys)
            GSoundsys->SetSelectedInputDevice(m_selectedInput);
        const char* devName = m_selectedInput.empty() ? nullptr : m_selectedInput.c_str();
        // Reopen the meter capture against the new device so the
        // Input Level row continues to reflect mic activity from the
        // device the user just picked.
        if (m_capture->isCapturing())
            m_capture->stop();
        if (m_capture->isOpen())
            m_capture->close();
        if (m_capture->open(devName, 16000, 4096))
            m_capture->start();
        return;
    }
    m_values[row] = v;
}

void AudioPage::AudioProvider::OnRowFocusChanged(int /*prevRow*/, int newRow)
{
    // Just record the new row + start the dwell timer.  TickPreviewLoop
    // fires the preview once the focus has been stable for ~250ms — that
    // way a Mount-time FocusInitial doesn't race CreateWave during the
    // page's own setup, and a quick scroll-through-rows keystroke does
    // not start (and immediately stop) a preview on every transit row.
    if (!GSoundsys)
        return;
    if (newRow != m_previewActiveRow)
    {
        // Cut whatever was playing; the new row's preview kicks in
        // after the dwell window.
        GSoundsys->TerminatePreview();
        m_previewActiveRow = newRow;
        m_previewLoopTick = Poseidon::Foundation::GlobalTickCount();
        m_previewStartedThisFocus = false;
    }
}

void AudioPage::AudioProvider::TickPreviewLoop()
{
    if (!GSoundsys)
        return;
    if (m_previewActiveRow < 0)
        return;
    uint32_t now = Poseidon::Foundation::GlobalTickCount();
    uint32_t elapsed = now - m_previewLoopTick;
    const uint32_t kDwellMs = 250;
    const uint32_t kLoopGapMs = 5000;
    // Music is looping internally (Repeat=0 in StartCategoryPreview)
    // so it doesn't need a 5 s replay — once started it just keeps
    // playing.  Effects + Speech are one-shot and benefit from the
    // periodic replay so the user doesn't need to re-touch the slider.
    uint32_t threshold =
        m_previewStartedThisFocus ? (m_previewActiveRow == kRowMusic ? UINT32_MAX : kLoopGapMs) : kDwellMs;
    if (elapsed < threshold)
        return;
    bool fired = false;
    switch (m_previewActiveRow)
    {
        case kRowMusic:
            GSoundsys->StartCategoryPreview(WaveMusic);
            fired = true;
            break;
        case kRowEffects:
            GSoundsys->StartCategoryPreview(WaveEffect);
            fired = true;
            break;
        case kRowSpeech:
            GSoundsys->StartCategoryPreview(WaveSpeech);
            fired = true;
            break;
        case kRowEax:
            GSoundsys->StartEAXPreview();
            fired = true;
            break;
        // Output device intentionally NOT auto-started on focus — the
        // device preview is gated on first value change so opening Audio
        // doesn't spam audio when the user is just navigating.
        default:
            break;
    }
    if (fired)
    {
        m_previewStartedThisFocus = true;
        m_previewLoopTick = now;
    }
}

void AudioPage::AudioProvider::OnRowAction(int row, Display& host)
{
    if (row != kRowTestMic)
        return;
    auto* shell = dynamic_cast<OptionsShell*>(&host);
    if (!shell)
        return;
    shell->PushPage(std::make_unique<MicTestPage>());
}

namespace
{
// OpenAL Soft prefixes every enumerated device name with this so
// apps can tell which OAL implementation handles the device.
// Single-OAL build: pure noise.  Stripped for display only — the
// full name still goes back into alcOpenDevice / alcCaptureOpenDevice.
const char kOalPrefix[] = "OpenAL Soft on ";
constexpr size_t kOalPrefixLen = sizeof(kOalPrefix) - 1;

std::string StripOalPrefix(const std::string& full)
{
    if (full.compare(0, kOalPrefixLen, kOalPrefix) == 0)
        return full.substr(kOalPrefixLen);
    return full;
}
} // namespace

void AudioPage::AudioProvider::RebuildOutputDeviceList() const
{
    m_outputDevs.clear();
    m_outputDevsDisplay.clear();
    const char* systemDefault = LocalizeString("STR_DISP_MAIN_OPT_SYSTEM_DEFAULT");
    m_outputDevs.emplace_back(systemDefault);
    m_outputDevsDisplay.emplace_back(systemDefault);
    if (GSoundsys)
    {
        for (auto& d : GSoundsys->ListOutputDevices())
        {
            if (m_unavailableOutputDevs.count(d))
                continue;
            m_outputDevs.push_back(d);
            m_outputDevsDisplay.push_back(StripOalPrefix(d));
        }
    }
    m_outputDevCStrs.clear();
    for (auto& s : m_outputDevsDisplay)
        m_outputDevCStrs.push_back(s.c_str());
}

void AudioPage::AudioProvider::RebuildInputDeviceList() const
{
    m_inputDevs.clear();
    m_inputDevsDisplay.clear();
    const char* systemDefault = LocalizeString("STR_DISP_MAIN_OPT_SYSTEM_DEFAULT");
    m_inputDevs.emplace_back(systemDefault);
    m_inputDevsDisplay.emplace_back(systemDefault);
    for (auto& d : VoNCapture::listDevices())
    {
        m_inputDevs.push_back(d);
        m_inputDevsDisplay.push_back(StripOalPrefix(d));
    }
    m_inputDevCStrs.clear();
    for (auto& s : m_inputDevsDisplay)
        m_inputDevCStrs.push_back(s.c_str());
}

OptionsScrollList::RowDef AudioPage::AudioProvider::RowFor(int row) const
{
    RefreshToggleTexts();

    switch (row)
    {
        case kRowOutputDev:
        {
            if (m_outputDevs.empty())
                RebuildOutputDeviceList();
            return {502, m_outputDevCStrs.data(), (int)m_outputDevCStrs.size()};
        }
        case kRowMusic:
            return {512, nullptr, -1};
        case kRowEffects:
            return {522, nullptr, -1};
        case kRowSpeech:
            return {532, nullptr, -1};
        case kRowEax:
            return {542, m_toggleOptions.data(), 2};
        case kRowHrtf:
            return {547, m_hrtfOptions.data(), 3};
        case kRowMicDev:
        {
            if (m_inputDevs.empty())
                RebuildInputDeviceList();
            return {552, m_inputDevCStrs.data(), (int)m_inputDevCStrs.size()};
        }
        case kRowInputLvl:
            return {562, nullptr, -2};
        default:
            return {-1, nullptr, 0};
    }
}

void AudioPage::AudioProvider::RefreshToggleTexts() const
{
    m_toggleText[0] = LocalizeString("STR_DISABLED");
    m_toggleText[1] = LocalizeString("STR_ENABLED");
    m_toggleOptions[0] = m_toggleText[0].c_str();
    m_toggleOptions[1] = m_toggleText[1].c_str();
    m_hrtfText[0] = LocalizeStringWithFallback("STR_DISP_OFF", "Off");
    m_hrtfText[1] = LocalizeStringWithFallback("STR_DISP_AUTO", "Auto");
    m_hrtfText[2] = LocalizeStringWithFallback("STR_DISP_ON", "On");
    m_hrtfOptions[0] = m_hrtfText[0].c_str();
    m_hrtfOptions[1] = m_hrtfText[1].c_str();
    m_hrtfOptions[2] = m_hrtfText[2].c_str();
}

void AudioPage::AudioProvider::TickMeter(int /*row*/, float& outLevel, float& outPeak)
{
    float target = 0.0f;
    if (m_capture && m_capture->isCapturing())
    {
        // Drain available samples so peak reflects the most recent
        // frame instead of the first 4096 samples ever captured.
        int avail = m_capture->availableSamples();
        if (avail > 0)
        {
            static int16_t scratch[2048];
            while (avail > 0)
            {
                int n = m_capture->read(scratch, (avail < 2048) ? avail : 2048);
                if (n <= 0)
                    break;
                avail -= n;
            }
        }
        target = m_capture->lastFramePeak();
    }

    AdvanceMeterBar(target, outLevel, outPeak);
}

} // namespace Poseidon
