#pragma once

#include <array>
#include <cstddef>
#include <cstdio>
#include <string>

namespace crackdown {

// Spots the Metal glitch by its frame rate: when it comes on, a steady 30 falls below 20 within a second, which heat or a
// busy street doesn't do that fast. Fed one sample a second; says when to save a report, with why. Limited to a few a
// session, spaced out, so a bad stretch can't fill the disk.
class FrameDropWatch {
 public:
  static constexpr size_t kSteadySeconds = 5;
  static constexpr double kSteadyFps = 27.0;
  static constexpr double kDroppedFps = 20.0;
  static constexpr int kCooldownSeconds = 120;
  static constexpr int kMaxReports = 3;

  // reason is set when it returns true.
  bool Sample(double fps, std::string& reason) {
    ++seconds_;
    const bool steady = filled_ == kSteadySeconds && Lowest() >= kSteadyFps;
    const bool ready = reports_ < kMaxReports && (reports_ == 0 || seconds_ - last_report_ >= kCooldownSeconds);
    const bool fire = steady && ready && fps < kDroppedFps;
    if (fire) {
      char text[96];
      std::snprintf(text, sizeof(text), "the frame rate fell from a steady %.0f to %.1f FPS", Lowest(), fps);
      reason = text;
      ++reports_;
      last_report_ = seconds_;
      filled_ = 0;  // The drop itself isn't steady, so a later report needs a steady stretch again.
      return true;
    }
    recent_[next_] = fps;
    next_ = (next_ + 1) % kSteadySeconds;
    filled_ = filled_ < kSteadySeconds ? filled_ + 1 : filled_;
    return false;
  }

 private:
  double Lowest() const {
    double lowest = recent_[0];
    for (size_t i = 1; i < kSteadySeconds; ++i) {
      lowest = recent_[i] < lowest ? recent_[i] : lowest;
    }
    return lowest;
  }

  std::array<double, kSteadySeconds> recent_{};
  size_t next_ = 0;
  size_t filled_ = 0;
  int seconds_ = 0;
  int last_report_ = 0;
  int reports_ = 0;
};

}  // namespace crackdown
