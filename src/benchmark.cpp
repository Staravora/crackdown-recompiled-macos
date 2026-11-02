#include "benchmark.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ui/virtual_key.h>

#include "cheat_hooks.h"
#include "free_camera.h"
#include "d3d_census.h"
#include "native/native_probe.h"
#include "native/shadow.h"
#include "native/texture_probe.h"

REXCVAR_DEFINE_BOOL(crackdown_benchmark, false, "Crackdown",
                    "Continue the saved game with a virtual pad, record frame times, write them and quit");
REXCVAR_DEFINE_INT32(crackdown_benchmark_picker_after_seconds, 0, "Crackdown",
                     "Seconds after the character loads to take the supply point picker as up. 0 finds it by its frame "
                     "rate, which only works while the emulator draws");
REXCVAR_DEFINE_INT32(crackdown_benchmark_district, 1, "Crackdown",
                     "District the benchmark spawns in: 0 Agency, 1 Los Muertos, 2 Volk, 3 Shai-Gen");
REXCVAR_DEFINE_INT32(crackdown_benchmark_supply_point, 0, "Crackdown",
                     "Supply point the benchmark picks in its district, counted from the first");
REXCVAR_DEFINE_INT32(crackdown_benchmark_warmup_seconds, 15, "Crackdown",
                     "Seconds in gameplay before the benchmark starts recording");
REXCVAR_DEFINE_INT32(crackdown_benchmark_seconds, 20, "Crackdown", "Seconds of gameplay the benchmark records");
REXCVAR_DEFINE_INT32(crackdown_benchmark_timeout_seconds, 150, "Crackdown",
                     "Seconds to wait for gameplay before the benchmark gives up");
REXCVAR_DEFINE_STRING(crackdown_benchmark_label, "", "Crackdown", "Label stored with the benchmark result");

namespace crackdown::benchmark {
namespace {

// The X_ERROR_ and X_STATUS_ macros name these unqualified.
using rex::X_RESULT;
using rex::X_STATUS;
using Clock = std::chrono::steady_clock;

constexpr auto kPressLength = std::chrono::milliseconds(150);
constexpr auto kPressGap = std::chrono::milliseconds(750);
constexpr uint32_t kLoadingScreenFps = 150;
constexpr uint32_t kPickerFps = 60;
constexpr rex::input::DeviceId kBenchmarkPad = static_cast<rex::input::DeviceId>(0x42454E43);

// The button the virtual pad holds, and a packet number that changes with it.
std::atomic<uint16_t> g_held{0};
std::atomic<uint32_t> g_packet{0};
std::atomic<uint32_t> g_frame_count{0};
std::atomic<bool> g_recording{false};
Clock::time_point g_start;
std::mutex g_frames_mutex;
std::vector<Clock::time_point> g_frames;

void Hold(uint16_t button) {
  g_held.store(button, std::memory_order_relaxed);
  g_packet.fetch_add(1, std::memory_order_relaxed);
}

void Press(uint16_t button) {
  Hold(button);
  std::this_thread::sleep_for(kPressLength);
  Hold(0);
  std::this_thread::sleep_for(kPressGap);
}

rex::ui::VirtualKey KeyFor(uint16_t button) {
  switch (button) {
    case rex::input::X_INPUT_GAMEPAD_START:
      return rex::ui::VirtualKey::kXInputPadStart;
    case rex::input::X_INPUT_GAMEPAD_DPAD_DOWN:
      return rex::ui::VirtualKey::kXInputPadDpadDown;
    default:
      return rex::ui::VirtualKey::kXInputPadA;
  }
}

// The front end reads presses as keystrokes, so every pulse is also a key down and a key up.
class BenchmarkInputDriver : public rex::input::InputDriver {
 public:
  BenchmarkInputDriver() : InputDriver(nullptr, 0) {}

  rex::X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    rex::input::DeviceInfo info;
    info.id = kBenchmarkPad;
    info.name = "Benchmark";
    info.synthetic = true;
    out.push_back(info);
  }

  rex::X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out_state) override {
    if (id != kBenchmarkPad) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    std::memset(out_state, 0, sizeof(*out_state));
    out_state->gamepad.buttons = g_held.load(std::memory_order_relaxed);
    out_state->packet_number = g_packet.load(std::memory_order_relaxed);
    return X_ERROR_SUCCESS;
  }

  rex::X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t,
                                 rex::input::X_INPUT_CAPABILITIES* out_caps) override {
    if (id != kBenchmarkPad) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;
    out_caps->sub_type = 0x01;
    out_caps->gamepad.buttons = 0xFFFF;
    return X_ERROR_SUCCESS;
  }

  rex::X_RESULT SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION*) override {
    return id == kBenchmarkPad ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  rex::X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t,
                                   rex::input::X_INPUT_KEYSTROKE* out_keystroke) override {
    if (id != kBenchmarkPad) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    std::lock_guard lock(keystroke_mutex_);
    const uint16_t held = g_held.load(std::memory_order_relaxed);
    if (held == reported_) {
      return X_ERROR_EMPTY;
    }
    std::memset(out_keystroke, 0, sizeof(*out_keystroke));
    // Release the reported key before pressing the next one.
    if (reported_) {
      out_keystroke->virtual_key = uint16_t(KeyFor(reported_));
      out_keystroke->flags = rex::input::X_INPUT_KEYSTROKE_KEYUP;
      reported_ = 0;
    } else {
      out_keystroke->virtual_key = uint16_t(KeyFor(held));
      out_keystroke->flags = rex::input::X_INPUT_KEYSTROKE_KEYDOWN;
      reported_ = held;
    }
    return X_ERROR_SUCCESS;
  }

 private:
  std::mutex keystroke_mutex_;
  uint16_t reported_ = 0;
};

double Percentile(std::vector<double> values, double fraction) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  return values[std::min(values.size() - 1, size_t(fraction * double(values.size())))];
}

[[noreturn]] void Finish(const std::filesystem::path& output_dir, const std::string& json, int exit_code) {
  std::error_code ec;
  std::filesystem::create_directories(output_dir, ec);
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto path = output_dir / fmt::format("benchmark-{}.json",
                                             std::chrono::duration_cast<std::chrono::seconds>(now).count());
  std::ofstream(path) << json << "\n";
  REXLOG_INFO("Benchmark: {} -> {}", json, path.string());
  rex::FlushLogging();
  std::_Exit(exit_code);
}

void Run(uint8_t* membase, std::filesystem::path output_dir) {
  const auto timeout = std::chrono::seconds(REXCVAR_GET(crackdown_benchmark_timeout_seconds));
  const std::string label = REXCVAR_GET(crackdown_benchmark_label);
  auto give_up = [&](const char* status) {
    Finish(output_dir, fmt::format(R"({{"label":"{}","status":"{}"}})", label, status), 2);
  };

  // START leaves the title screen and A takes the highlighted Continue, which loads the character.
  for (uint32_t i = 0; !cheats::LocalPlayerCharacter(membase); ++i) {
    if (Clock::now() - g_start > timeout) {
      give_up("no_gameplay");
    }
    Press(i % 3 == 2 ? rex::input::X_INPUT_GAMEPAD_START : rex::input::X_INPUT_GAMEPAD_A);
  }
  const double gameplay_after = std::chrono::duration<double>(Clock::now() - g_start).count();
  REXLOG_INFO("Benchmark: player character after {:.1f} s", gameplay_after);

  // A loading screen that runs far above the game's rate comes before the supply point picker, which draws the city
  // behind it at the game's rate. Presses during the loading screen are lost, so wait for the drop.
  bool loading_seen = false;
  const auto picker_after = std::chrono::seconds(REXCVAR_GET(crackdown_benchmark_picker_after_seconds));
  const Clock::time_point character_found = Clock::now();
  for (uint32_t last = g_frame_count.load(std::memory_order_relaxed);;) {
    if (Clock::now() - g_start > timeout) {
      give_up("no_supply_point_picker");
    }
    if (picker_after.count() > 0) {
      std::this_thread::sleep_until(character_found + picker_after);
      break;
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const uint32_t count = g_frame_count.load(std::memory_order_relaxed);
    const uint32_t fps = count - last;
    last = count;
    loading_seen |= fps > kLoadingScreenFps;
    if (loading_seen && fps < kPickerFps) {
      break;
    }
  }
  for (int32_t i = 0; i < REXCVAR_GET(crackdown_benchmark_district); ++i) {
    Press(rex::input::X_INPUT_GAMEPAD_DPAD_DOWN);
  }
  Press(rex::input::X_INPUT_GAMEPAD_A);
  for (int32_t i = 0; i < REXCVAR_GET(crackdown_benchmark_supply_point); ++i) {
    Press(rex::input::X_INPUT_GAMEPAD_DPAD_DOWN);
  }
  // The supply point, then the loadout.
  for (int i = 0; i < 2; ++i) {
    Press(rex::input::X_INPUT_GAMEPAD_A);
  }
  REXLOG_INFO("Benchmark: warming up");
  std::this_thread::sleep_for(std::chrono::seconds(REXCVAR_GET(crackdown_benchmark_warmup_seconds)));
  census::Begin();
  native::probe::Begin();
  g_recording.store(true, std::memory_order_relaxed);
  std::this_thread::sleep_for(std::chrono::seconds(REXCVAR_GET(crackdown_benchmark_seconds)));
  g_recording.store(false, std::memory_order_relaxed);
  census::Write(output_dir / fmt::format("census-{}.json", label), label);
  native::probe::Write(output_dir / fmt::format("native-probe-{}.json", label), label);

  std::vector<Clock::time_point> frames;
  {
    std::lock_guard lock(g_frames_mutex);
    frames = g_frames;
  }
  std::vector<double> frame_ms;
  for (size_t i = 1; i < frames.size(); ++i) {
    frame_ms.push_back(std::chrono::duration<double, std::milli>(frames[i] - frames[i - 1]).count());
  }
  const double span_s =
      frames.size() > 1 ? std::chrono::duration<double>(frames.back() - frames.front()).count() : 0.0;
  const double average_fps = span_s > 0 ? double(frame_ms.size()) / span_s : 0.0;
  const double p99_ms = Percentile(frame_ms, 0.99);
  // A stable 30 FPS needs every frame inside 33.3 ms, so frames past it are what stutter costs.
  size_t over_33_ms = 0;
  size_t over_50_ms = 0;
  double slow_ms = 0.0;
  {
    std::ofstream frames_file(output_dir / fmt::format("frames-{}.txt", label));
    for (double ms : frame_ms) {
      frames_file << fmt::format("{:.2f}\n", ms);
      over_33_ms += ms > 33.4;
      over_50_ms += ms > 50.0;
      slow_ms += ms > 33.4 ? ms : 0.0;
    }
  }
  Finish(output_dir,
         fmt::format(R"({{"label":"{}","status":"ok","character_after_s":{:.1f},"frames":{},"average_fps":{:.2f},)"
                     R"("median_frame_ms":{:.2f},"p90_frame_ms":{:.2f},"p99_frame_ms":{:.2f},)"
                     R"("max_frame_ms":{:.2f},"one_percent_low_fps":{:.2f},"frames_over_33ms":{},)"
                     R"("frames_over_50ms":{},"share_of_time_in_slow_frames":{:.3f}}})",
                     label, gameplay_after, frame_ms.size(), average_fps, Percentile(frame_ms, 0.5),
                     Percentile(frame_ms, 0.9), p99_ms, Percentile(frame_ms, 1.0),
                     p99_ms > 0 ? 1000.0 / p99_ms : 0.0, over_33_ms, over_50_ms,
                     span_s > 0 ? slow_ms / (span_s * 1000.0) : 0.0),
         0);
}

}  // namespace

void Install(rex::input::InputSystem* input, uint8_t* membase, const std::filesystem::path& output_dir) {
  if (!REXCVAR_GET(crackdown_benchmark) || !input || !membase) {
    return;
  }
  g_start = Clock::now();
  // The input system re-enumerates its drivers on every poll.
  input->AddDriver(std::make_unique<BenchmarkInputDriver>());
  REXLOG_INFO("Benchmark: continuing the saved game with a virtual pad");
  std::thread(Run, membase, output_dir).detach();
}

void OnGuestFrame() {
  native::textures::OnGuestFrame();
  native::shadow::OnGuestFrame();
  free_camera::OnGuestFrame();
  g_frame_count.fetch_add(1, std::memory_order_relaxed);
  if (!g_recording.load(std::memory_order_relaxed)) {
    return;
  }
  census::OnGuestFrame();
  native::probe::OnGuestFrame();
  std::lock_guard lock(g_frames_mutex);
  g_frames.push_back(Clock::now());
}

}  // namespace crackdown::benchmark
