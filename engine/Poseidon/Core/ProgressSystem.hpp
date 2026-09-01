#pragma once

#include <Poseidon/Foundation/Time/Time.hpp>
#include <Poseidon/Graphics/Rendering/Draw/Font.hpp>

namespace Poseidon { class Script; }

namespace Poseidon
{

// Global ProgressScript reference
extern Ref<Script> ProgressScript;

// Progress system for loading screens and status display.
class ProgressSystem
{
	// progress bar status
	float _progressTot; // should be in sec (estimated) if possible
	float _progressCur; // current state (starts at 0)
	float _progressDrawn; // state drawn (0..1)

	::Poseidon::Foundation::UITime _progressStartTime; // Glob.uiTime when started
	RString _progressTitle,_progressFormat;
	Ref<Font> _progressFont;
	float _progressFontSize;

	SRef<AbstractOptionsUI> _progressDisplay;

	DWORD _lastProgressRefresh;
	DWORD _progressRefreshBase;

	// Splash cost accounting.  Frame() presents, and a present during loading is not
	// free -- it is what turns a refresh point in the loader into wall time.  These
	// count it so the cost can be read off a normal log instead of guessed at.
	DWORD _windowStartTick = 0;   // GlobalTickCount at the last Start()
	DWORD _frameMsTotal = 0;      // wall ms spent inside Frame() since Start()
	DWORD _lastFrameCostMs = 0;   // wall ms the most recent Frame() took
	int _frameCount = 0;          // frames presented since Start()
	int _refreshCount = 0;        // Refresh() calls since Start() (throttled or not)

	bool _clear;

	public:
	ProgressSystem();
	~ProgressSystem();

	private:
	ProgressSystem( const ProgressSystem &src );
	const ProgressSystem &operator = ( const ProgressSystem &src );

	public:
	void Reset();
	void Add( float ammount ); // calculate total estimation
	void Advance( float ammount ); // really advance

	void SetPassed( float value ); // set state 
	void SetRest( float value ); // set state 

	// execute in pairs - no nesting allowed
	void Start( RString title, RString format="%s" );
	void Start( RString title, RString format, Ref<Font> font, float size=-1.0 );
	void Start(AbstractOptionsUI *display);
	void Finish();
	void Draw();

	void Frame(); // FinishDraw // Draw // InitDraw
	void ResetFrameAccounting();
	void Refresh(); // Frame (if neccessary)FinishDraw // ProgressDraw // InitDraw

	bool Active() const;

	void EnableClear(bool enable = true) {_clear = enable;}
};

// Global ProgressScript reference
extern Ref<Script> ProgressScript;

// The "Bohemia Interactive presents" startup splash plays once per launch, on the genuine first
// boot. A mod re-mount re-runs the content-init path but must not replay the splash over the
// rebuilt main menu. Pure decision so it is unit-testable without the app/engine.
bool ShouldArmStartupSplash(bool firstBoot, bool noSplash, bool landEditor, bool dedicatedServer,
                            bool clientConnected);

// Global ProgressSystem accessor - uses dependency injection pattern
// Application sets the instance pointer during initialization
// Tests can inject custom instances using ProgressSystemTestGuard
extern ProgressSystem& GetGProgress();
extern void SetGProgress(ProgressSystem* instance);

// RAII guard for test isolation - automatically restores previous instance
class ProgressSystemTestGuard
{
public:
    explicit ProgressSystemTestGuard(ProgressSystem* testInstance);
    ~ProgressSystemTestGuard();
    ProgressSystemTestGuard(const ProgressSystemTestGuard&) = delete;
    ProgressSystemTestGuard& operator=(const ProgressSystemTestGuard&) = delete;
private:
    ProgressSystem* m_previousInstance;
};

inline void ProgressReset() { GetGProgress().Reset(); }

inline void ProgressAdd( float ammount ) { GetGProgress().Add(ammount); }
inline void ProgressAdvance( float ammount ) { GetGProgress().Advance(ammount); }

inline void ProgressSetPassed( float value ) { GetGProgress().SetPassed(value); }
inline void ProgressSetRest( float value ) { GetGProgress().SetRest(value); }

// execute in pairs - no nesting allowed
inline void ProgressStart( RString title, RString format="%s" )
{
	GetGProgress().Start(title,format);
}
inline void ProgressStart( RString title, RString format, Ref<Font> font, float size=-1.0 )
{
	GetGProgress().Start(title,format,font,size);
}
inline void ProgressStart(AbstractOptionsUI *display)
{
	GetGProgress().Start(display);
}
inline void ProgressFinish() { GetGProgress().Finish(); }
inline void ProgressDraw() { GetGProgress().Draw(); }

inline void ProgressFrame() { GetGProgress().Frame(); }
inline void ProgressRefresh() { GetGProgress().Refresh(); }

inline void ProgressClear(bool clear = true) { GetGProgress().EnableClear(clear); }
} // namespace Poseidon
