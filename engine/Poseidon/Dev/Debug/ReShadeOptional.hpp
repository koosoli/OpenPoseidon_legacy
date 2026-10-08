#pragma once

// Queries an already loaded optional add-on. Never loads or installs an injector.
void DrawOptionalReShadeControls();

// Opt-in process-local Vulkan layer discovery, before renderer creation.
// Missing or mismatched external runtime leaves the normal renderer untouched.
void PrepareOptionalReShadeEnvironment(bool enabled);

namespace Poseidon::Dev { class HarnessServer; }
void RegisterOptionalReShadeHarness(Poseidon::Dev::HarnessServer& harness);
