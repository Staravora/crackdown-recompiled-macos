#pragma once

#include <array>
#include <cstddef>
#include <cstdio>
#include <string>

namespace crackdown {

// Spots the Metal glitch by its frame rate: when it comes on, a steady 30 falls below 20 within a second and stays down,
// which heat doesn't do that fast. Opening the map or the pause menu dips as low but is back within two seconds, so the
// drop has to last. Fed one sample a second; says when to save a report, with why. Limited to a few a session, spaced
// out, so a bad stretch can't fill the disk.
class FrameDropWatch {
 public:
  static constexpr size_t kSteadySeconds = 5;
  static constexpr size_t kRampSeconds = 3;  // The glitch can slide down for a few seconds before falling under 20.
  static constexpr double kSteadyFps = 27.0;
  static constexpr double kDroppedFps = 20.0;
  static constexpr double kStillDownFps = 25.0;
  static constexpr int kStillDownSeconds = 2;  // After the drop, all under kStillDownFps.
  static constexpr int kCooldownSeconds = 120;
  static constexpr int kMaxReports = 3;

  // reason is set when it returns true.
  bool Sample(double fps, std::string& reason) {
    ++seconds_;
    if (confirming_) {
      lowest_drop_ = fps < lowest_drop_ ? fps : lowest_drop_;
      if (fps >= kStillDownFps) {
        confirming_ = false;  // A menu's dip: back up already.
        Remember(fps);
        return false;
      }
      if (++still_down_ < kStillDownSeconds) {
        return false;
      }
      confirming_ = false;
      if (reports_ >= kMaxReports || (reports_ > 0 && seconds_ - last_report_ < kCooldownSeconds)) {
        return false;
      }
      char text[128];
      std::snprintf(text, sizeof(text), "the frame rate fell from a steady %.0f to %.1f FPS and stayed under %.0f", steady_from_,
                    lowest_drop_, kStillDownFps);
      reason = text;
      ++reports_;
      last_report_ = seconds_;
      return true;
    }
    if (filled_ == kHistory && SteadyBeforeRamp() >= kSteadyFps && fps < kDroppedFps) {
      confirming_ = true;
      still_down_ = 0;
      steady_from_ = SteadyBeforeRamp();
      lowest_drop_ = fps;
      filled_ = 0;  // The drop isn't steady, so whatever follows needs a steady stretch again.
      return false;
    }
    Remember(fps);
    return false;
  }

 private:
  static constexpr size_t kHistory = kSteadySeconds + kRampSeconds;

  void Remember(double fps) {
    recent_[next_] = fps;
    next_ = (next_ + 1) % kHistory;
    filled_ = filled_ < kHistory ? filled_ + 1 : filled_;
  }

  // The lowest of the kSteadySeconds before the last kRampSeconds.
  double SteadyBeforeRamp() const {
    double lowest = 1e9;
    for (size_t i = 0; i < kSteadySeconds; ++i) {
      const double fps = recent_[(next_ + i) % kHistory];  // next_ is the oldest.
      lowest = fps < lowest ? fps : lowest;
    }
    return lowest;
  }

  std::array<double, kHistory> recent_{};
  size_t next_ = 0;
  size_t filled_ = 0;
  int seconds_ = 0;
  int last_report_ = 0;
  int reports_ = 0;
  bool confirming_ = false;
  int still_down_ = 0;
  double steady_from_ = 0;
  double lowest_drop_ = 0;
};

}  // namespace crackdown
