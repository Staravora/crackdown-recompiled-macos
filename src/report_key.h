#pragma once

#include <functional>
#include <utility>

#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

namespace crackdown {

// ⌘⇧R saves a problem report. A held key is one press, and the game never sees the R.
class ReportKey final : public rex::ui::WindowInputListener {
 public:
  // Above ReXApp's input listener (z-order 0), so the R is consumed before the guest sees it.
  static constexpr size_t kZOrder = 1000;

  ReportKey(rex::ui::Window* window, std::function<void()> save) : window_(window), save_(std::move(save)) {
    window_->AddInputListener(this, kZOrder);
  }

  ~ReportKey() override { window_->RemoveInputListener(this); }

  ReportKey(const ReportKey&) = delete;
  ReportKey& operator=(const ReportKey&) = delete;

  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (!IsReportKey(e)) {
      return;
    }
    e.set_handled(true);
    if (!e.prev_state()) {
      save_();
    }
  }

  void OnKeyUp(rex::ui::KeyEvent& e) override {
    if (IsReportKey(e)) {
      e.set_handled(true);
    }
  }

 private:
  bool IsReportKey(const rex::ui::KeyEvent& e) const {
    return e.virtual_key() == rex::ui::VirtualKey::kR && e.is_super_pressed() && e.is_shift_pressed() &&
           e.target() == window_;
  }

  rex::ui::Window* window_;
  std::function<void()> save_;
};

}  // namespace crackdown
