// The Direct3D GPU waits block on the word they poll instead of spinning; the command processor wakes them when it
// writes guest memory (patches 0033 and 0069).

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iterator>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <cerrno>
#include <ctime>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <cerrno>
#include <os/os_sync_wait_on_address.h>
#endif

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "guest_hooks.h"
#include "guest_layout.h"
#include "native/shadow.h"

#if defined(__APPLE__)
REXCVAR_DEFINE_BOOL(crackdown_native_gpu_waits, false, "Crackdown",
                    "Block GPU waits on the polled word instead of spinning (macOS)");
#endif

#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)

namespace {

// The poll is the function every Direct3D GPU wait calls: (wait): the wait's first word is the device, its word at +20 the
// time base when the wait began, and the caller checks its own condition after each poll. The callers that wait on a
// word of the GPU writeback block at device+10768 block on it; the others keep the guest's own polling.
constexpr uint32_t kDeviceWritebackOffset = 10768;
constexpr uint32_t kDeviceFenceOffset = 10780;
constexpr uint32_t kDeviceFlagsOffset = 10813;
constexpr uint8_t kDeviceStoppedFlag = 4;
constexpr uint32_t kWaitStartOffset = 20;
// Blocks that ended by timeout with the word already changed before a site's blocking is given up.
constexpr uint32_t kMissedWakeLimit = 8;

struct PollSite {
  const char* name;
  uint32_t return_address;
  bool blocks;
  uint32_t word_offset;
  // Upper bound on one block, for writers that do not wake the waiter.
  uint32_t timeout_ms;
};

// fence: the fence written by EVENT_WRITE_SHD. ring_space: the read pointer the command processor writes back
// (VdEnableRingBufferRPtrWriteBack gets block+60). ring_progress: the progress word at block+4. swap_throttle and
// resource wait on words the CPU writes. return_address is where each site's call to the poll returns.
constexpr size_t kSiteCount = 5;
const std::array<PollSite, kSiteCount>& Sites() {
  static const std::array<PollSite, kSiteCount> sites = {{
      {"fence", crackdown::Layout().gpu_wait_returns[0], true, 0, 2},
      {"ring_space", crackdown::Layout().gpu_wait_returns[1], true, 60, 1},
      {"ring_progress", crackdown::Layout().gpu_wait_returns[2], true, 4, 1},
      {"swap_throttle", crackdown::Layout().gpu_wait_returns[3], false, 0, 0},
      {"resource", crackdown::Layout().gpu_wait_returns[4], false, 0, 0},
  }};
  return sites;
}
constexpr size_t kOtherSite = kSiteCount;

struct alignas(64) SiteState {
  std::atomic<bool> blocking{true};
  std::atomic<uint32_t> missed_wakes{0};
};
SiteState g_site_state[kSiteCount];

struct WaitState {
  uint32_t object = 0;
  uint32_t device = 0;
  uint32_t start = 0;
  uint32_t block = 0;
  const uint32_t* word = nullptr;
  const uint32_t* guest_word = nullptr;
  uint32_t seen = 0;
};
thread_local WaitState t_wait;

size_t SiteIndex(uint32_t return_address) {
  for (size_t i = 0; i < kSiteCount; ++i) {
    if (Sites()[i].return_address == return_address) {
      return i;
    }
  }
  return kOtherSite;
}

uint32_t LoadGuest32(uint8_t* base, uint32_t address) {
  uint32_t value = 0;
  std::memcpy(&value, rex::memory::GuestPtr<const uint8_t*>(base, address), sizeof(value));
  return std::byteswap(value);
}

uint32_t LoadWord(const uint32_t* word) {
  return __atomic_load_n(word, __ATOMIC_ACQUIRE);
}

bool DeviceStopped(uint8_t* base, uint32_t device) {
  return *rex::memory::GuestPtr<const uint8_t*>(base, device + kDeviceFlagsOffset) & kDeviceStoppedFlag;
}

// Both views are the same pages; a difference that outlasts one re-read means the physical
// address is not the one the guest reads.
bool ViewsAgree(const uint32_t* physical_word, const uint32_t* guest_word) {
  const uint32_t physical = LoadWord(physical_word);
  return LoadWord(guest_word) == physical || LoadWord(physical_word) != physical;
}

void DisableBlocking(size_t site, const char* reason) {
  if (g_site_state[site].blocking.exchange(false, std::memory_order_relaxed)) {
    REXLOG_WARN("GPU wait {}: blocking disabled ({}), the guest polls again", Sites()[site].name, reason);
  }
}

// The command processor stores to and wakes physical_membase + (address & 0x1FFFFFFF), where address
// is the hardware form of the guest address that the guest emits into the packet.
bool ResolveWaitWord(uint8_t* base, uint32_t device, uint32_t word_offset, WaitState& wait) {
  if (!device || DeviceStopped(base, device)) {
    return false;
  }
  const uint32_t block = LoadGuest32(base, device + kDeviceWritebackOffset);
  if (!block || (block & 3)) {
    return false;
  }
  rex::system::KernelState* kernel = rex::system::kernel_state();
  rex::memory::Memory* memory = kernel ? kernel->memory() : nullptr;
  if (!memory) {
    return false;
  }
  const uint32_t address = block + word_offset;
  const uint32_t physical = (address & 0x1FFFFFFF) + ((((address >> 20) & 0xFFF) + 512) & 0x1000);
  wait.word = memory->TranslatePhysical<const uint32_t*>(physical);
  wait.guest_word = rex::memory::GuestPtr<const uint32_t*>(base, address);
  wait.device = device;
  wait.block = block;
  return ViewsAgree(wait.word, wait.guest_word);
}

enum class WaitOutcome { kReturned, kTimedOut, kFailed };

// Returns at once if the word no longer holds `undesired`; the comparison and the sleep are one
// atomic step, so a store between the caller's read and the sleep is never slept through.
WaitOutcome WaitWhileEqual(const uint32_t* word, uint32_t undesired, uint32_t timeout_ms) {
#if defined(_WIN32)
  if (WaitOnAddress(const_cast<uint32_t*>(word), &undesired, sizeof(undesired), timeout_ms)) {
    return WaitOutcome::kReturned;
  }
  return GetLastError() == ERROR_TIMEOUT ? WaitOutcome::kTimedOut : WaitOutcome::kFailed;
#elif defined(__APPLE__)
  if (os_sync_wait_on_address_with_timeout(const_cast<uint32_t*>(word), undesired,
                                           sizeof(undesired), OS_SYNC_WAIT_ON_ADDRESS_NONE,
                                           OS_CLOCK_MACH_ABSOLUTE_TIME,
                                           uint64_t(timeout_ms) * 1000000) >= 0) {
    return WaitOutcome::kReturned;
  }
  switch (errno) {
    case EINTR:
    case EFAULT:
      return WaitOutcome::kReturned;
    case ETIMEDOUT:
      return WaitOutcome::kTimedOut;
    default:
      return WaitOutcome::kFailed;
  }
#else
  const timespec timeout = {0, long(timeout_ms) * 1000000L};
  if (syscall(SYS_futex, word, FUTEX_WAIT_PRIVATE, undesired, &timeout, nullptr, 0) == 0) {
    return WaitOutcome::kReturned;
  }
  switch (errno) {
    case EAGAIN:
    case EINTR:
      return WaitOutcome::kReturned;
    case ETIMEDOUT:
      return WaitOutcome::kTimedOut;
    default:
      return WaitOutcome::kFailed;
  }
#endif
}

void BlockUntilWordWritten(size_t site, WaitState& wait) {
  const WaitOutcome outcome = WaitWhileEqual(wait.word, wait.seen, Sites()[site].timeout_ms);
  const uint32_t current = LoadWord(wait.word);
  if (outcome == WaitOutcome::kFailed) {
    DisableBlocking(site, "wait failed");
  } else if (!ViewsAgree(wait.word, wait.guest_word)) {
    DisableBlocking(site, "the guest reads another address");
  }
  SiteState& state = g_site_state[site];
  if (outcome == WaitOutcome::kTimedOut) {
    if (current != wait.seen &&
        state.missed_wakes.fetch_add(1, std::memory_order_relaxed) + 1 >= kMissedWakeLimit) {
      DisableBlocking(site, "its writer does not wake it");
    }
  } else if (outcome == WaitOutcome::kReturned && current != wait.seen) {
    state.missed_wakes.store(0, std::memory_order_relaxed);
  }
  wait.seen = current;
}

}  // namespace

void crackdown::hooks::GpuFence(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const uint32_t device = ctx.r3.u32, fence = ctx.r4.u32, caller = uint32_t(ctx.lr);
  const uint32_t current = LoadGuest32(base, device + kDeviceFenceOffset);
  const uint32_t passed = LoadGuest32(base, LoadGuest32(base, device + kDeviceWritebackOffset));
  const bool pending = current - fence < current - passed;
  const auto start = std::chrono::steady_clock::now();
  t_wait = {};
  original(ctx, base);
  t_wait = {};
  if (pending) {
    crackdown::native::shadow::OnFenceWait(caller, fence, uint64_t(std::chrono::nanoseconds(std::chrono::steady_clock::now() - start).count()));
  }
}

// Returns nonzero while the wait should go on. The first call for a wait only reads the word, so a block always
// follows a check of the word by the guest.
void crackdown::hooks::GpuPoll(PPCContext& ctx, uint8_t* base, GuestFunction original) {
#if defined(__APPLE__)
  if (!REXCVAR_GET(crackdown_native_gpu_waits)) {
    original(ctx, base);
    return;
  }
#endif
  const uint32_t object = ctx.r3.u32;
  const size_t site = SiteIndex(uint32_t(ctx.lr));
  original(ctx, base);
  if (site == kOtherSite || !Sites()[site].blocks || !ctx.r3.u32 ||
      !g_site_state[site].blocking.load(std::memory_order_relaxed)) {
    return;
  }
  WaitState& wait = t_wait;
  const uint32_t device = LoadGuest32(base, object);
  const uint32_t start = LoadGuest32(base, object + kWaitStartOffset);
  if (!wait.word || wait.object != object || wait.device != device || wait.start != start) {
    wait = {};
    if (!ResolveWaitWord(base, device, Sites()[site].word_offset, wait)) {
      wait = {};
      return;
    }
    wait.object = object;
    wait.start = start;
    wait.seen = LoadWord(wait.word);
    return;
  }
  if (DeviceStopped(base, device) || LoadGuest32(base, device + kDeviceWritebackOffset) != wait.block) {
    wait = {};
    return;
  }
  const uint32_t current = LoadWord(wait.word);
  if (current != wait.seen) {
    wait.seen = current;
    return;
  }
  BlockUntilWordWritten(site, wait);
}

#else

// Other hosts retain the original polling until a matching host wake implementation is validated.
void crackdown::hooks::GpuFence(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  original(ctx, base);
}

void crackdown::hooks::GpuPoll(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  original(ctx, base);
}

#endif  // _WIN32 || __linux__ || __APPLE__
