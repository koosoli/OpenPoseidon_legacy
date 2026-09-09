#pragma once
#include <Poseidon/AI/AIUnit.hpp>

namespace Poseidon
{
inline bool PlayerOrderBypassesSpeech(Command::Context context, bool networked)
{
    return !networked && (context == Command::CtxUI || context == Command::CtxUIWithJoin);
}
}
