#pragma once

// Queries an already loaded optional add-on. Never loads or installs an injector.
void DrawOptionalReShadeControls();

namespace Poseidon::Dev { class HarnessServer; }
void RegisterOptionalReShadeHarness(Poseidon::Dev::HarnessServer& harness);
