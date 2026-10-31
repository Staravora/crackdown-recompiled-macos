#pragma once

#include <filesystem>
#include <string>

namespace crackdown::census {

// With crackdown_d3d_census on, the benchmark counts what the game asks of Direct3D during its recording window:
// draws by entry point, tiling, resolves, textures and where they came from, queries, locks and command buffers. Every
// hook still runs the guest function unchanged. The numbers scope native rendering (docs/native-rendering-survey.md);
// the addresses and device fields are the base guest's, so a Title Update 6 guest counts nothing.
bool Enabled();

void Begin();
void OnGuestFrame();
void Write(const std::filesystem::path& path, const std::string& label);

}  // namespace crackdown::census
