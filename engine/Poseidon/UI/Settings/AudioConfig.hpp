#pragma once

// Audio user-settings persisted to <user-dir>/audio.cfg.
//
// Pattern target — graphics.cfg, controls.cfg etc. follow the same
// layout: a plain-data class with sensible Defaults, a Normalize
// against a runtime Environment, and Load / Save against a path.
// All system-global (one file per machine, not per game profile).
//
// Lifecycle:
//   1. Boot:   try Load(path).  Missing/corrupt → LoadDefaults() +
//              Save(path) once (eager-write so the file is always
//              there for users to inspect).
//   2. Boot:   Normalize(env).  Out-of-range volumes clamp to defaults
//              and unknown device names reset to "" (system default).
//              Normalized changes are kept in memory but NOT saved —
//              a temporarily unplugged USB device shouldn't lose its
//              setting.  Persistence on a normalized field happens
//              only when the user enters + closes the Audio screen.
//   3. UI:     AudioPage::Unmount calls SyncFromRuntime(GSoundsys)
//              + Save(path).
//
// Volume convention: 0..100 integer percent in the file and on the
// wire to UI.  IAudioSystem internally uses 0..10 floats; the
// translation lives at the AudioConfig boundary so the rest of the
// engine doesn't have to think about scale.

#include <string>
#include <vector>


namespace Poseidon
{
class AudioConfig
{
public:
	// Persistable fields.  Public for direct access from tests + the
	// AudioPage's read/sync path; the only invariants are enforced by
	// Normalize, not by setters.
	int          musicVolume   = 80;          // 0..100
	int          effectsVolume = 80;          // 0..100
	int          speechVolume  = 80;          // 0..100
	bool         eaxEnabled    = false;
	int          hrtfMode      = 1;           // 0=Off 1=Auto 2=On (IAudioSystem::HrtfMode)
	std::string  outputDevice;                // "" → system default
	std::string  inputDevice;                 // "" → system default

	// Reset every field to factory defaults.
	void LoadDefaults();

	// Validation against the runtime environment.  Volumes outside
	// [0,100] clamp to defaults; device names not in the live device
	// lists reset to "" (system default).  Each field validates
	// independently — a stale outputDevice doesn't affect inputDevice.
	// Returns true if any field was changed.
	struct Environment
	{
		virtual ~Environment() = default;
		virtual std::vector<std::string> ListOutputDevices() const = 0;
		virtual std::vector<std::string> ListInputDevices()  const = 0;
	};
	bool Normalize(const Environment& env);

	// True on successful parse, false if the file doesn't exist or is
	// unparseable.  Caller treats "false" as "use defaults".  Does not
	// call LoadDefaults itself — leaves the instance untouched on
	// failure so callers can chain Load → LoadDefaults → Save.
	bool Load(const std::string& path);

	// Atomic-ish save: writes via ParamFile, returns false on I/O error.
	bool Save(const std::string& path) const;
};

} // namespace Poseidon
