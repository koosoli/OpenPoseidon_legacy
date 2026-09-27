#pragma once

namespace Poseidon
{
// Diagnostic-only: first successful joined-owner capture arm gets a fresh bounded
//128-row slow-chain window (total <=256 rows). Exact WarmProfile1 and no active
//read scope required; never changes texture reading, decoding or caching policy.
enum class WarmMipDiagnosticCaptureReset { Disabled, Accepted, Refused };
WarmMipDiagnosticCaptureReset ResetWarmMipChainDiagnosticsForCapture();
}
