#include "problem_report.h"

#import <AppKit/AppKit.h>

#include <sys/sysctl.h>

#include <ctime>
#include <deque>
#include <fstream>
#include <string>
#include <system_error>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include "active_log.h"
#include "native/shadow.h"
#include "native/thermal_state.h"

namespace crackdown::problem_report {
namespace {

constexpr size_t kLogTailLines = 4000;

std::string SystemString(const char* name) {
  size_t size = 0;
  if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) {
    return "unknown";
  }
  std::string value(size, '\0');
  if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) {
    return "unknown";
  }
  value.resize(value.find('\0') == std::string::npos ? size : value.find('\0'));
  return value;
}

uint64_t MemoryBytes() {
  uint64_t bytes = 0;
  size_t size = sizeof(bytes);
  return sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0 ? bytes : 0;
}

std::string Setting(const char* name) {
  const std::string value = rex::cvar::GetFlagByName(name);
  return value.empty() ? "(not set)" : value;
}

bool WriteLogTail(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::ifstream in(from);
  if (!in) {
    return false;
  }
  std::deque<std::string> lines;
  for (std::string line; std::getline(in, line);) {
    lines.push_back(std::move(line));
    if (lines.size() > kLogTailLines) {
      lines.pop_front();
    }
  }
  std::ofstream out(to);
  for (const std::string& line : lines) {
    out << line << '\n';
  }
  return bool(out);
}

}  // namespace

std::filesystem::path Save(const std::filesystem::path& logs_folder) {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d-%H%M%S", &local);
  const std::filesystem::path folder = logs_folder / "reports" / fmt::format("report-{}", stamp);
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  if (ec) {
    REXLOG_ERROR("Problem report: couldn't make {}: {}", folder.string(), ec.message());
    return {};
  }

  const bool metal = native::shadow::RequestReport(folder);
  rex::FlushLogging();
  const bool log = WriteLogTail(ActiveLogFile(), folder / "log-tail.txt");

  std::ofstream about(folder / "about.txt");
  char saved[64];
  std::strftime(saved, sizeof(saved), "%Y-%m-%d %H:%M:%S", &local);
  about << fmt::format("Crackdown Recompiled {}+macos.{}\n", CRACKDOWN_PORT_VERSION, CRACKDOWN_MAC_BUILD)
        << fmt::format("Saved: {}\n", saved)
        << fmt::format("Mac: {}, macOS {}, {} GB\n", SystemString("hw.model"), SystemString("kern.osproductversion"),
                       MemoryBytes() >> 30)
        << fmt::format("Thermal state: {}\n", native::ThermalState())
        << fmt::format("Renderer: {}\n", native::shadow::DrawsAlone() ? "Metal" : "emulated")
        << fmt::format("Metal resolution (crackdown_render_scale): {}\n", Setting("crackdown_render_scale"))
        << fmt::format("Texture filtering (anisotropic_override): {}\n", Setting("anisotropic_override")) << '\n'
        << (metal ? "metal-frame.png and draws.txt are the frame Metal drew next and its draw list. renderer.json holds the "
                    "renderer's counters for the last ten seconds, once the game has run that long. textures.txt lists the textures "
                    "whose memory has changed since Metal loaded them.\n"
                  : "The Metal renderer wasn't drawing, so there's no frame or draw list.\n")
        << (log ? "log-tail.txt is the end of this session's log.\n" : "This session's log couldn't be read.\n");
  about.close();

  REXLOG_INFO("Problem report saved to {}", folder.string());
  @autoreleasepool {
    [[NSSound soundNamed:@"Glass"] play];
  }
  return folder;
}

}  // namespace crackdown::problem_report
