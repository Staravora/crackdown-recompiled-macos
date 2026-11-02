#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace trg::update {

unsigned long CurrentProcessId();
std::vector<std::string> CurrentProcessArguments();
std::filesystem::path CurrentExecutablePath();

bool HasOriginalAppImage(std::filesystem::path& path);
bool LaunchApplication(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
                       const std::filesystem::path& working_directory, std::string& error,
                       bool clear_update_test_environment = false);
bool LaunchUpdateHelper(const std::filesystem::path& helper, const std::filesystem::path& job_file,
                        std::string& error);
bool ApplyAppImageUpdate(const std::filesystem::path& downloaded, const std::filesystem::path& appimage,
                         const std::vector<std::string>& arguments, std::string& error);
bool WaitForProcess(unsigned long pid, int timeout_ms, std::string& error);

#if defined(__APPLE__)
// Unpacks a Mac release zip with ditto, which keeps the bundle's links, modes and signatures; app is its one .app.
bool ExtractMacApp(const std::filesystem::path& zip, const std::filesystem::path& folder, std::filesystem::path& app,
                   std::string& error);
// Puts staged in installed's place: staged is moved next to it under a hidden name, installed becomes the hidden
// backup and the new copy takes its name. On failure installed is left as it was.
bool SwapMacApp(const std::filesystem::path& staged, const std::filesystem::path& installed,
                std::filesystem::path& backup, std::string& error);
// Swaps, then leaves a shell waiting for this process to end; it deletes the backup and opens the new app.
bool ApplyMacAppUpdate(const std::filesystem::path& staged, const std::filesystem::path& installed,
                       const std::vector<std::string>& arguments, std::string& error);
// The running program's Developer ID team and signing identifier; empty when it is ad hoc or unsigned.
bool RunningAppSigning(std::string& team, std::string& identifier);
// The app is valid, sealed throughout, and signed by the running program's team under its identifier.
bool VerifyMacApp(const std::filesystem::path& app, std::string& error);
#endif

}  // namespace trg::update
