#include "update_config.h"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rex/logging.h>

#include "platform.h"
#include "updater/update_paths.h"
#include "updater/update_platform.h"

#if defined(__APPLE__)
#include <unistd.h>
#endif

namespace crackdown {
namespace {

constexpr const char* kReleasesUrl =
    "https://api.github.com/repos/saulob/crackdown-xbox360-recompiled/releases/latest";
constexpr const char* kDownloadUrlPrefix = "https://github.com/saulob/crackdown-xbox360-recompiled/releases/download/";

#if defined(CRACKDOWN_UPDATE_TEST_OVERRIDES)
std::optional<std::string> Environment(const char* name) {
#if defined(_WIN32)
  char* value = nullptr;
  size_t size = 0;
  if (_dupenv_s(&value, &size, name) != 0 || !value) {
    return std::nullopt;
  }
  std::string result(value);
  std::free(value);
  if (result.empty()) {
    return std::nullopt;
  }
  return result;
#else
  const char* value = std::getenv(name);
  if (!value || !*value) {
    return std::nullopt;
  }
  return std::string(value);
#endif
}

bool TestMode() {
  const std::optional<std::string> value = Environment("CD_UPDATE_TEST");
  return value && *value == "1";
}

std::optional<std::string> TestVariable(const char* name) {
  if (!TestMode()) {
    return std::nullopt;
  }
  return Environment(name);
}
#endif

std::vector<std::string> RestartArguments() {
  std::vector<std::string> arguments = trg::update::CurrentProcessArguments();
  arguments.erase(std::remove_if(arguments.begin(), arguments.end(),
                                 [](const std::string& argument) {
                                   return argument.rfind("--crackdown_skip_launcher", 0) == 0;
                                 }),
                  arguments.end());
  arguments.emplace_back("--crackdown_launcher=true");
  return arguments;
}

}  // namespace

trg::update::UpdateConfig MakeUpdateConfig(const std::filesystem::path& install_dir) {
  using trg::update::SemanticVersion;
  using trg::update::UpdatePackageKind;

  trg::update::UpdateConfig config;
  config.current_version = CRACKDOWN_PORT_VERSION;
  config.releases_url = kReleasesUrl;
  config.download_url_prefix = kDownloadUrlPrefix;
  config.user_agent = std::string("Crackdown-Updater/") + CRACKDOWN_PORT_VERSION;
  config.install_dir = install_dir;
  config.restart_arguments = RestartArguments();

#if defined(_WIN32)
  config.package_kind = UpdatePackageKind::kZip;
  config.asset_name = [](const SemanticVersion& version) {
    return "Crackdown-v" + version.CoreString() + "-windows-x64.zip";
  };
  config.checksum_asset_name = [](const SemanticVersion& version) {
    return "Crackdown-v" + version.CoreString() + "-windows-x64.sha256";
  };
  config.restart_executable = install_dir / "crackdown.exe";
  config.required_staged_files = {"crackdown.exe"};
  config.install_supported = std::filesystem::exists(install_dir / "trg-updater.exe");
  if (!config.install_supported) {
    config.install_unsupported_reason =
        "The update helper is missing from this installation. Reinstall this release to enable automatic updates.";
  }
#elif defined(__APPLE__)
  // The Mac app is released from its own repository, set at configure time; without one it never checks.
  const std::string repository = CRACKDOWN_MAC_UPDATE_REPOSITORY;
  if (repository.empty()) {
    config.releases_url.clear();
    config.download_url_prefix.clear();
  } else {
    config.releases_url = "https://api.github.com/repos/" + repository + "/releases/latest";
    config.download_url_prefix = "https://github.com/" + repository + "/releases/download/";
  }
  config.package_kind = UpdatePackageKind::kMacApp;
  config.asset_name = [](const SemanticVersion& version) {
    return "Crackdown-v" + version.CoreString() + "-macos-arm64.zip";
  };
  config.checksum_asset_name = [](const SemanticVersion& version) {
    return "Crackdown-v" + version.CoreString() + "-macos-arm64.sha256";
  };
  // <app>/Contents/MacOS/crackdown
  const std::filesystem::path app = trg::update::CurrentExecutablePath().parent_path().parent_path().parent_path();
  std::string team;
  std::string identifier;
  if (app.extension() != ".app") {
    config.install_unsupported_reason = "Updates install themselves in the app download.";
  } else if (trg::update::Utf8Path(app).find("/AppTranslocation/") != std::string::npos) {
    // Gatekeeper runs a downloaded app from a read-only copy until it has been moved.
    config.install_unsupported_reason =
        "Move Crackdown Recompiled into your Applications folder and open it from there to install updates.";
  } else if (access(app.parent_path().c_str(), W_OK) != 0 || access(app.c_str(), W_OK) != 0) {
    config.install_unsupported_reason = "This Mac account cannot change the folder the app is in.";
  } else if (!trg::update::RunningAppSigning(team, identifier) || team.empty()) {
    config.install_unsupported_reason = "This build is not signed with a Developer ID, so it cannot install updates.";
  } else {
    config.mac_app_path = app;
    config.restart_executable = app;
    config.install_supported = true;
  }
#else
  config.package_kind = UpdatePackageKind::kAppImage;
  config.asset_name = [](const SemanticVersion& version) {
    return "CrackdownRecompiled-v" + version.CoreString() + "-x86_64.AppImage";
  };
  config.checksum_asset_name = [](const SemanticVersion& version) {
    return "CrackdownRecompiled-v" + version.CoreString() + "-x86_64.sha256";
  };
  std::filesystem::path appimage;
  if (trg::update::HasOriginalAppImage(appimage)) {
    config.appimage_path = appimage;
    config.restart_executable = appimage;
    config.install_supported = true;
  } else {
    config.install_unsupported_reason = "Automatic installation is available in the AppImage build.";
  }
#endif

  config.log = [](std::string_view text) { REXLOG_INFO("Updater: {}", text); };

#if defined(CRACKDOWN_UPDATE_TEST_OVERRIDES)
  if (const std::optional<std::string> version = TestVariable("CD_UPDATE_TEST_CURRENT_VERSION"); version) {
    config.current_version = *version;
    REXLOG_WARN("Updater test mode: reporting version {}", config.current_version);
  }
  if (const std::optional<std::string> json = TestVariable("CD_UPDATE_TEST_RELEASE_JSON"); json) {
    config.release_json_override = PathFromUtf8(*json);
  }
  if (const std::optional<std::string> hash = TestVariable("CD_UPDATE_TEST_ASSET_SHA256"); hash) {
    config.sha256_override = *hash;
  }
#endif
  return config;
}

}  // namespace crackdown
