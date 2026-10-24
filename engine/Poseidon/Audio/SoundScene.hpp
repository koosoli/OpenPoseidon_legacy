#ifdef _MSC_VER
#pragma once
#endif

#ifndef _SOUNDSCENE_HPP
#define _SOUNDSCENE_HPP

#include <Poseidon/Core/Types.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Audio/IAudioSystem.hpp>
#include <Poseidon/Core/HandledList.hpp>
namespace Poseidon
{

class Speaker;
class RadioChannel;
class RadioMessage;

enum {ChannelEast,ChannelWest,ChannelGuerrila,NChannels};

struct SoundPars;

} // namespace Poseidon
#include <Poseidon/Foundation/Time/Time.hpp>
namespace Poseidon
{

class SoundScene
{
	AutoArray< SRef<Speaker> > _speakers;
	SRef<Speaker> _speakerPlayer;

	RefArray<IWave> _globalSounds;

	LinkArray<IWave> _all2DSounds;
	LinkArray<IWave> _all3DSounds;

	Ref<IWave> _musicTrack;

	float _musicVolume; // volume controls for _musicTrack
	float _musicTargetVolume;
	Poseidon::Foundation::Time _musicTargetTime;

	float _soundVolume; // sound controls for all sounds with accomodation
	float _soundTargetVolume;
	Poseidon::Foundation::Time _soundTargetTime;

	float _musicInternalVolume; // given from config / wave
	float _musicInternalFrequency; // given from config / wave

	enum {MaxEnvSounds=8};
	Ref<IWave> _envSounds[MaxEnvSounds]; // four corners of environment, two stages
	bool _envSoundRenewed[MaxEnvSounds];

	float _earAccommodation;
	
	public:
	SoundScene();
	~SoundScene();
	
	IWave *Open( const char *filename, bool is3D=true, bool accom=true );
	IWave *OpenAndPlay
	(
		const char *filename, Vector3Par pos, Vector3Par speed,
		float volume=1.0, float frequency=1.0
	);
	IWave *OpenAndPlayOnce
	(
		const char *filename, Vector3Par pos, Vector3Par speed,
		float volume=1.0, float frequency=1.0
	);
	IWave *OpenAndPlay2D
	(
		const char *filename,
		float volume=1.0, float frequency=1.0,
		bool accomodate=true
	);
	IWave *OpenAndPlayOnce2D
	(
		const char *filename,
		float volume=1.0, float frequency=1.0,
		bool accomodate=true
	);
	void SimulateSpeedOfSound(IWave *sound);

	void AddSound( IWave *wave ); // global sounds (Ref is in SoundScene)
	void DeleteSound( IWave *wave );

	void AdvanceAll( float deltaT, bool paused ); // maintain all sounds 
	void Reset();

	// environmental sounds
	void SetEnvSound( const char *name, float volume );
	void AdvanceEnvSounds();
	void StartMusicTrack(const SoundPars &pars, float from=0);
	void StopMusicTrack();
	// A music track was started and has not finished.
	bool MusicPlaying() { return _musicTrack && !_musicTrack->IsTerminated(); }
	float GetMusicVolume() const;
	void SetMusicVolume(float vol, float time);

	float GetSoundVolume() const;
	void SetSoundVolume(float vol, float time);

	// Test-only observers for the live music fade (AdvanceAll ramps
	// `_musicVolume` toward the target set by SetMusicVolume/fadeMusic).
	// `GetMusicVolume` returns the *target*; these return the live ramp
	// and the volume actually applied to the music wave this frame.
	float MusicLiveVolumeForTest() const { return _musicVolume; }
	float MusicAppliedVolumeForTest() const { return _musicTrack ? _musicTrack->GetVolume() : -1.f; }

	IWave *Say( int speakerID, RString id );
	IWave *SayPause( float time );
	// Diagnostics: enumerate active 2D speech waves (used by tri test verbs)
	int CountActive2DSpeechWaves() const;

	// Mission-manifest audio preload (asset-loader plan Phase B): walks the
	// mission's CfgSounds/CfgRadio and queues each referenced wave into the
	// backend's PCM cache during the loading screen, so the first say /
	// playSound / radio sentence skips its file read + decode.
	void PreloadMissionAudio();
	// Returns a pipe-separated diagnostic string "name:state|name:state|..."
	// where state ∈ {playing, paused, stopped, terminated}.  Limited to
	// 2D waves of kind WaveSpeech.
	RString Describe2DSpeechWaves() const;
	// Find a 2D wave by case-insensitive substring match on its name and
	// return its current playback offset in seconds.  Returns -1.f if no
	// matching wave is in _all2DSounds (used by the triRadioWaveOffset tri
	// verb so a tester can poll a radio message's offset across pause/resume).
	float Find2DWaveOffsetSeconds(const char* nameSubstring) const;
};

extern SoundScene *GSoundScene;

} // namespace Poseidon

#endif
