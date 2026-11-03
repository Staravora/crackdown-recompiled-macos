#pragma once

namespace crackdown::native {

// macOS's own view of how hot the Mac is: "nominal", "fair", "serious" or "critical". A fanless Mac slows its CPU and GPU
// as it climbs, and may already be slowing at "fair". Any thread.
const char* ThermalState();
// Logs the state now and each time macOS changes it, so a session's log shows when the Mac heated up.
void WatchThermalState();

}  // namespace crackdown::native
