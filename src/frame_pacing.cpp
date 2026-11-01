// Frame rate: above 30 the title runs its variable-step mode (its 1/60 s, 16 ms frame path) with a step of one
// frame of the selected rate instead of the fixed 1/30 s step it ships with. 30 is the shipped timing.

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <random>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "frame_pacing.h"
#include "frame_timing.h"
#include "free_camera.h"
#include "glyph_overlay.h"
#include "guest_hooks.h"
#include "guest_layout.h"
#include "hang_watchdog.h"
#include "settings.h"

REXCVAR_DEFINE_INT32(crackdown_debug_frame_dips, 0, "Crackdown",
                     "Developer test of uneven frame rates: 0 off, else the maximum extra delay in ms (up to 100) added "
                     "before frames, sweeping from none to full every 6 s with random jitter and rare stalls of twice it");

namespace {

using crackdown::FrameStep;

constexpr uint32_t kOriginalPresentInterval = crackdown::PresentIntervalFor(crackdown::kOriginalFrameRate);

constexpr uint32_t kFixedStepOffset = 13;
constexpr uint32_t kSuspendedOffset = 14;
constexpr uint32_t kLogicalTickOffset = 16;
constexpr uint32_t kDevicePresentIntervalOffset = 13220;
// The swap word: front buffer | present interval << 8 | immediate threshold. A swap whose vblank has passed is
// released at once below the threshold percentage of a frame.
constexpr uint32_t kSwapImmediateThresholdMask = 0xFF;
constexpr uint32_t kSwapPresentIntervalMask = 0xF00;
constexpr uint32_t kSwapImmediateAlways = 100;
// The variable-step frame time the title's inline sites use without the replay branch. The frame timer publishes a
// frame as a byte set once it has measured one and that frame's milliseconds; with the fixed step off, the inline sites
// read `[valid] ? [ms] : 16`.
constexpr uint32_t kVariableStepFrameMs = 16;

std::atomic<uint32_t> g_step_bits{std::bit_cast<uint32_t>(1.0f / 60.0f)};
std::atomic<uint32_t> g_frame_ms{kVariableStepFrameMs};
std::atomic<double> g_original_steps{crackdown::FixedFrameStep(crackdown::kDefaultFrameRate).original_steps};

// Only the main thread runs frames.
FrameStep g_step = crackdown::FixedFrameStep(crackdown::kDefaultFrameRate);
crackdown::FramePacer g_pacer;
crackdown::CompensatedStepClock g_console_clock;
int g_console_rate = 0;
int64_t g_console_last_ns = 0;
double g_logical_tick_fraction = 0.0;
double g_frame_ms_fraction = 0.0;
int g_logged_rate = -1;

// 30 and 60 both present at the guest vblank (no host pacing), so a slow frame already shows up as a longer
// measured interval here; resets the clock on a rate change so switching rates never carries over stale debt.
crackdown::FrameStep ConsolePacedStep(int rate, double interval) {
  if (rate != g_console_rate) {
    g_console_rate = rate;
    g_console_clock.Reset();
    return crackdown::FixedFrameStep(rate);
  }
  return crackdown::StepFromSeconds(g_console_clock.Take(interval, 1.0 / rate).seconds);
}

// Set by the launcher before the title starts.
int FrameRate() {
  return crackdown::NormalizeFrameRate(REXCVAR_GET(crackdown_frame_rate));
}

bool HighFrameRate(int rate) {
  return rate != crackdown::kOriginalFrameRate;
}

uint32_t LoadBE32(uint8_t* base, uint32_t addr) {
  uint32_t value = 0;
  std::memcpy(&value, rex::memory::GuestPtr<const uint8_t*>(base, addr), sizeof(value));
  return std::byteswap(value);
}

void StoreBE32(uint8_t* base, uint32_t addr, uint32_t value) {
  value = std::byteswap(value);
  std::memcpy(rex::memory::GuestPtr<uint8_t*>(base, addr), &value, sizeof(value));
}

float LoadBEFloat(uint8_t* base, uint32_t addr) {
  return std::bit_cast<float>(LoadBE32(base, addr));
}

uint8_t* GuestByte(uint8_t* base, uint32_t addr) {
  return rex::memory::GuestPtr<uint8_t*>(base, addr);
}

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Sleeps until shortly before the deadline, then yields until it, so a coarse timer cannot make the frame late.
void WaitUntil(int64_t deadline_ns) {
  constexpr int64_t kYieldNs = 500'000;
  const int64_t remaining = deadline_ns - NowNs();
  if (remaining > kYieldNs) {
#if defined(_WIN32)
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                       TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -(remaining - kYieldNs) / 100;
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer, INFINITE);
    } else {
      Sleep(DWORD((remaining - kYieldNs) / 1'000'000));
    }
#else
    std::this_thread::sleep_until(std::chrono::steady_clock::time_point(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::nanoseconds(deadline_ns - kYieldNs))));
#endif
  }
  while (NowNs() < deadline_ns) {
    std::this_thread::yield();
  }
}

// The timing object can be replaced while the title runs, and every new one starts in fixed-step mode.
void SelectVariableStep(uint8_t* base, int rate) {
  if (const uint32_t timing = LoadBE32(base, crackdown::Layout().timing_object)) {
    *GuestByte(base, timing + kFixedStepOffset) = 0;
  }
  const uint32_t limiter = crackdown::LimiterFrameRateFor(rate);
  if (LoadBE32(base, crackdown::Layout().limiter_frame_rate) != limiter) {
    StoreBE32(base, crackdown::Layout().limiter_frame_rate, limiter);
  }
}

// crackdown_debug_frame_dips: delays the frame's start, so the pacing below measures it as a late frame exactly like
// one the GPU could not finish in time.
void DebugFrameDip() {
  const int max_ms = REXCVAR_GET(crackdown_debug_frame_dips);
  static int logged_ms = 0;
  if (max_ms != logged_ms) {
    logged_ms = max_ms;
    if (max_ms > 0) {
      REXLOG_WARN("Debug frame dips: up to {} ms extra per frame", std::min(max_ms, crackdown::kMaxDebugDipMs));
    }
  }
  if (max_ms <= 0) {
    return;
  }
  static std::mt19937 random{std::random_device{}()};
  static const int64_t start_ns = NowNs();
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  const int64_t now = NowNs();
  const double delay =
      crackdown::DebugFrameDipSeconds(max_ms, double(now - start_ns) * 1e-9, unit(random), unit(random));
  if (delay > 0.0) {
    WaitUntil(now + int64_t(delay * 1e9));
  }
}

// Picks this frame's step; host-paced rates first wait for their release time, 30 and 60 measure the real
// interval since they are already paced by the guest vblank.
void BeginFrame(int rate) {
  DebugFrameDip();
  const int64_t now = NowNs();
  if (crackdown::HostPacedFrameRate(rate)) {
    const crackdown::FramePacer::Frame frame = g_pacer.Next(rate, now);
    WaitUntil(frame.release_ns);
    g_step = frame.step;
  } else {
    const double interval = g_console_last_ns ? double(now - g_console_last_ns) * 1e-9 : 0.0;
    g_console_last_ns = now;
    g_step = ConsolePacedStep(rate, interval);
  }
  g_frame_ms_fraction += g_step.seconds * 1000.0;
  g_frame_ms.store(crackdown::TakeFrameMs(g_frame_ms_fraction), std::memory_order_relaxed);
  g_step_bits.store(std::bit_cast<uint32_t>(g_step.seconds_f), std::memory_order_relaxed);
  g_original_steps.store(g_step.original_steps, std::memory_order_relaxed);
}

// +16 counts the title's 1/30 s steps. The original advances it unconditionally while not suspended; this tops
// that up to the compensated step so a missed frame's extra elapsed time still reaches it, without double-counting
// what the original already added. The fraction never goes negative: a tick already granted is never clawed back.
void SyncLogicalTick(uint8_t* base, uint32_t timing, uint32_t applied) {
  g_logical_tick_fraction = std::max(0.0, g_logical_tick_fraction + g_step.original_steps - double(applied));
  if (const uint32_t whole = crackdown::TakeWhole(g_logical_tick_fraction)) {
    StoreBE32(base, timing + kLogicalTickOffset, LoadBE32(base, timing + kLogicalTickOffset) + whole);
  }
}

uint32_t TitlePresentInterval(uint32_t interval) {
  return HighFrameRate(FrameRate()) && interval == kOriginalPresentInterval ? 1 : interval;
}

void LogFrameRate(int rate) {
  if (rate == g_logged_rate) {
    return;
  }
  g_logged_rate = rate;
  if (rate == crackdown::kFrameRateUnlimited) {
    REXLOG_INFO("Frame rate: unlimited, game time follows the measured frame time");
  } else if (crackdown::HostPacedFrameRate(rate)) {
    REXLOG_INFO("Frame rate: {} FPS, paced on the host", rate);
  } else {
    REXLOG_INFO("Frame rate: {} FPS, {} vblank(s) per frame", rate, crackdown::PresentIntervalFor(rate));
  }
}

}  // namespace

// One main-loop frame. It reads the fixed-step byte for the frame time, the frame limiter and the logical tick.
void crackdown::hooks::FrameLoop(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const int rate = FrameRate();
  LogFrameRate(rate);
  BeginFrame(rate);
  if (HighFrameRate(rate)) {
    SelectVariableStep(base, rate);
  }
  crackdown::UpdateButtonGlyphs(base);
  crackdown::NoteGuestFrame();
  original(ctx, base);
}

// The simulation update. Its only caller passes 1/30 s times the time scale; g_step.original_steps is exactly 1.0
// on an on-time frame at every rate, so this is a no-op multiply unless a frame ran long or short of its target.
void crackdown::hooks::SimulationUpdate(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  ctx.f1.f64 = double(float(ctx.f1.f64 * g_step.original_steps));
  if (crackdown::free_camera::TimeFrozen()) {
    ctx.f1.f64 *= crackdown::free_camera::kFrozenTimeScale;
  }
  original(ctx, base);
}

// The frame delta getter. With the fixed step on (30), the original always returns timescale/30, the same value
// g_step gives on an on-time frame, so this can replace it outright instead of only overriding the variable-step
// rates; it is what keeps camera and animation on the same compensated clock as the simulation update. The time scale
// is a float, 1.0 unless the game scales time.
void crackdown::hooks::FrameDelta(PPCContext& ctx, uint8_t* base) {
  ctx.f1.f64 = double(LoadBEFloat(base, crackdown::Layout().time_scale) *
                      std::bit_cast<float>(g_step_bits.load(std::memory_order_relaxed)));
}

void crackdown::hooks::TimingUpdate(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const uint32_t timing = LoadBE32(base, crackdown::Layout().timing_object);
  const bool active = timing && !*GuestByte(base, timing + kSuspendedOffset);
  const uint32_t before = active ? LoadBE32(base, timing + kLogicalTickOffset) : 0;
  original(ctx, base);
  if (active) {
    SyncLogicalTick(base, timing, LoadBE32(base, timing + kLogicalTickOffset) - before);
  }
}

// The title's frame timer (slot 0x2C of the timing object's vtable) keeps the fixed step's 30 or 33 ms minimum frame, so
// with the fixed step off a shorter frame is published as 0 ms. The camera divides by its time step, and a ragdoll on a
// 0 ms frame turned the camera, the player and the audio listener NaN. With the fixed step off the published frame is
// the port's own frame time, never 0; at 30 the sites use the fixed 33 ms.
void crackdown::hooks::FrameTimer(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  original(ctx, base);
  if (HighFrameRate(FrameRate()) && *GuestByte(base, crackdown::Layout().measured_frame_valid)) {
    StoreBE32(base, crackdown::Layout().measured_frame_ms, g_frame_ms.load(std::memory_order_relaxed));
  }
}

// D3D present interval setter: r3 = device, r4 = D3DPRESENT_INTERVAL value.
void crackdown::hooks::PresentInterval(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  ctx.r4.u64 = TitlePresentInterval(ctx.r4.u32);
  original(ctx, base);
}

// Builds the swap packet from the device's interval; the interval from device creation never goes through the setter.
void crackdown::hooks::SwapPacket(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const uint32_t field = ctx.r3.u32 + kDevicePresentIntervalOffset;
  const uint32_t interval = LoadBE32(base, field);
  if (const uint32_t wanted = TitlePresentInterval(interval); wanted != interval) {
    StoreBE32(base, field, wanted);
  }
  original(ctx, base);
}

// The XDK swap callback: r3 = the swap word. The title passes threshold 0, so a frame finished just after its vblank
// waited a whole extra vblank; the host presents without tearing either way. Host-paced rates take interval 0.
void crackdown::hooks::SwapCallback(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const int rate = FrameRate();
  if (crackdown::HostPacedFrameRate(rate)) {
    ctx.r3.u64 = ctx.r3.u32 & ~kSwapPresentIntervalMask;
  } else if (HighFrameRate(rate)) {
    ctx.r3.u64 = (ctx.r3.u32 & ~kSwapImmediateThresholdMask) | kSwapImmediateAlways;
  }
  original(ctx, base);
}

// Codegen midasm hook after each inline `li rN,16`: the 16 ms frame time of the title's variable-step sites.
void crackdown_variable_step_frame_ms(PPCRegister& frame_ms) {
  if (crackdown::HostPacedFrameRate(FrameRate())) {
    frame_ms.u64 = g_frame_ms.load(std::memory_order_relaxed);
  }
}

// Codegen midasm hooks after the loads of per-update constants that ignore the frame time: the 1/30 of the crowd
// animation (time += rate * step) and the 0.033 of a cosmetic timer.
void crackdown_crowd_anim_step(PPCRegister& step) {
  step.f64 = double(crackdown::ScalePerOriginalStep(float(step.f64), g_original_steps.load(std::memory_order_relaxed)));
}

void crackdown_cosmetic_timer_step(PPCRegister& step) {
  step.f64 = double(crackdown::ScalePerOriginalStep(float(step.f64), g_original_steps.load(std::memory_order_relaxed)));
}

double crackdown::OriginalStepsPerUpdate() {
  return g_original_steps.load(std::memory_order_relaxed);
}
