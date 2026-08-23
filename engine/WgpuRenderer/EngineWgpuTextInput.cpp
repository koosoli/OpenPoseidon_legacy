// SDL text-input / IME arming for the WGPU backend.
//
// Backported from upstream CWR-CE 955d6c46 ("Arm text input/IME only while a
// real text field has focus"), which fixed the GL33 backend only.  The shared
// SDLEventWindow no longer starts text input on Attach; ControlsContainer::
// SetFocus arms it per focused control through IGraphicsEngine, so a backend
// that does not override these ends up with dead edit fields.
//
// Kept in its own translation unit rather than appended to EngineWgpu.cpp:
// that file is large and hot, and the class is already partitioned across TUs
// elsewhere in the engine.

#include "EngineWgpu.hpp"

#include <SDL3/SDL.h>

namespace Poseidon
{

void EngineWgpu::StartTextInput()
{
    if (_window)
        SDL_StartTextInput(_window);
}

void EngineWgpu::StopTextInput()
{
    if (_window)
        SDL_StopTextInput(_window);
}

bool EngineWgpu::IsTextInputActive() const
{
    return _window && SDL_TextInputActive(_window);
}

} // namespace Poseidon
