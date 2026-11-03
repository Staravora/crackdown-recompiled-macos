#pragma once

#include <filesystem>
#include <string>

namespace crackdown::problem_report {

// Saves a folder in <logs>/reports for something that looks wrong in play: about.txt (the build, the Mac, the renderer
// settings), log-tail.txt, and with the Metal renderer the next frame it draws, that frame's draw list and its counters.
// Plays a sound when it's saved, so the player knows without leaving the game. Returns the folder, or empty on failure.
std::filesystem::path Save(const std::filesystem::path& logs_folder);
// The game saving one by itself, with why. Silent, named auto-report-..., and skipped once kMaxAutomaticReports are in
// the folder, so they can't fill the disk unnoticed.
std::filesystem::path SaveAutomatic(const std::filesystem::path& logs_folder, const std::string& reason);
inline constexpr size_t kMaxAutomaticReports = 10;

}  // namespace crackdown::problem_report
