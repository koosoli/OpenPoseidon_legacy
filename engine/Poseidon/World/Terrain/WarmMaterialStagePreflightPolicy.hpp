#pragma once
#include <cstddef>
namespace Poseidon::Streaming
{
struct WarmMaterialStagePreflightPolicy
{
    // Numeric shared recentre budget, never a time/RSS guarantee. Caller initializes
    // two; bad oversized budgets refuse rather than authorize extra source loads.
    static bool ReserveMissingSource(bool enabled, bool existingBankEntry,
        size_t& attemptsRemaining, size_t& metadataWorkRemaining,
        bool provenRetainedOnlyStageUnused = false)
    {
        if (!enabled || existingBankEntry || provenRetainedOnlyStageUnused ||
            !attemptsRemaining || attemptsRemaining > 2 ||
            metadataWorkRemaining < 2) return false;
        --attemptsRemaining; --metadataWorkRemaining;
        return true;
    }
};
}
