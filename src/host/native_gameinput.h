#pragma once

#include "host_log.h"
#include "../trt/input.h"

#include <GameInput.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

extern "C" u32 TRT_STDCALL trt_MapVirtualKeyW(u32 code, u32 type);
extern "C" u32 trt_vk_to_char_ex(u32 vk, const u8* ks);
extern "C" u64 trt_mem_budget_committed(void);

namespace longan {

// GameInput belongs to the host. Its readings feed the same bridge as
// CoreWindow events, so a guest never needs to load the console's DLL.
class native_gameinput {
 public:
  void open() {
    HMODULE module = GetModuleHandleW(L"GameInput.dll");
    if (!module) module = LoadLibraryW(L"GameInput.dll");
    if (!module) {
      log_line("input: GameInput.dll unavailable");
      return;
    }
    const bool has_init = GetProcAddress(module, "GameInputInitialize") != nullptr;
    using create_fn = HRESULT (WINAPI*)(IGameInput**);
    const auto create = reinterpret_cast<create_fn>(GetProcAddress(module, "GameInputCreate"));
    if (create && SUCCEEDED(create(input_.put())) && input_) {
      // Request exclusive foreground focus policy so Xbox GameInput delivers
      // raw USB mouse & keyboard readings to this foreground window.
      input_->SetFocusPolicy(GameInputExclusiveForegroundInput);
      char rline[96];
      std::snprintf(rline, sizeof(rline),
                    "input: native GameInput ready (hasInit=%u)", has_init ? 1u : 0u);
      log_line(rline);

      // Register individual device callbacks: some GameInput runtimes treat
      // combined masks strictly or require GameInputKindUnknown for all devices.
      const struct { GameInputKind kind; const char* tag; } probes[] = {
          {GameInputKindMouse, "mouse"},
          {GameInputKindKeyboard, "keyboard"},
          {GameInputKindUnknown, "all"},
      };
      for (const auto& p : probes) {
        GameInputCallbackToken tok = GAMEINPUT_INVALID_CALLBACK_TOKEN_VALUE;
        const HRESULT dev_hr = input_->RegisterDeviceCallback(
            nullptr, p.kind, GameInputDeviceAnyStatus,
            GameInputBlockingEnumeration, this, &native_gameinput::device_callback,
            &tok);
        if (p.kind == GameInputKindMouse) device_callback_token_ = tok;
        char line[96];
        std::snprintf(line, sizeof(line),
                      "input: GameInput dev enum(%s) hr=0x%08lx",
                      p.tag, static_cast<unsigned long>(dev_hr));
        log_line(line);
      }

      const HRESULT mouse_callback_hr = input_->RegisterReadingCallback(
          nullptr, GameInputKindMouse, 0.0f, this, &native_gameinput::mouse_reading_callback,
          &mouse_callback_token_);
      if (SUCCEEDED(mouse_callback_hr) &&
          mouse_callback_token_ != GAMEINPUT_INVALID_CALLBACK_TOKEN_VALUE) {
        mouse_callback_active_.store(true);
        log_line("input: native GameInput mouse callback registered");
      } else {
        char line[96];
        std::snprintf(line, sizeof(line),
                      "input: GameInput mouse callback failed hr=0x%08lx",
                      static_cast<unsigned long>(mouse_callback_hr));
        log_line(line);
      }
    } else {
      log_line("input: GameInputCreate failed");
    }
  }

  bool keyboard_active() const { return keyboard_active_.load(); }
  bool mouse_active() const { return mouse_active_.load(); }
  // True only while GameInput actually produced mouse data this recently.
  // mouse_active() latches on the first delta and then stays true even if the
  // device stops reporting (or the reading callback never fires again), which
  // used to be read as "GameInput owns the mouse" and silently discarded every
  // CoreWindow pointer event for the rest of the session.
  bool mouse_active_recent(ULONGLONG within_ms) const {
    if (!mouse_active_.load()) return false;
    const ULONGLONG last = last_mouse_ms_.load();
    return last != 0 && (GetTickCount64() - last) < within_ms;
  }
  bool mouse_connected() const { return mouse_connected_.load(); }
  bool wheel_active_recent() const {
    const ULONGLONG last = last_wheel_ms_.load();
    return last != 0 && GetTickCount64() - last < 120;
  }
  bool keyboard_connected() const { return keyboard_connected_.load(); }

  using host_key_fn = bool (*)(u16 vk, u16 scan, bool ext, bool down);

  void set_host_key_handler(host_key_fn fn) { host_key_fn_ = fn; }

  void notify_interceptor_key(u16 vk) {
    interceptor_seen_.store(true);
    if (vk < interceptor_last_ms_.size())
      interceptor_last_ms_[vk].store(GetTickCount64());
  }

  void suspend() {
    // The CoreWindow focus handler releases both native and guest input.
    keyboard_active_.store(false);
    mouse_active_.store(false);
    last_wheel_ms_.store(0);
    keyboard_reading_ = nullptr;
    mouse_reading_ = nullptr;
    keys_ = {};
    {
      std::lock_guard<std::mutex> lock(mouse_mu_);
      mouse_ = {};
      mouse_baseline_ = false;
      dev_mouse_ = {};
      dev_mouse_baseline_ = false;
    }
    mouse_click_pending_.store(false);
    keyboard_missing_since_ = 0;
    mouse_missing_since_ = 0;
  }

  // Return a left release so the UI can handle a click on the launcher.
  bool poll() {
    if (!input_) return false;
    drain(GameInputKindKeyboard, keyboard_reading_, [&](IGameInputReading& reading) {
      update_keyboard(reading);
    });
    if (!interceptor_seen_.load()) apply_keyboard_repeat();

    bool clicked = mouse_click_pending_.exchange(false);
    if (!mouse_callback_active_.load()) {
      drain(GameInputKindMouse, mouse_reading_, [&](IGameInputReading& reading) {
        clicked |= update_mouse(reading);
      });
      if (!mouse_active_recent(300)) clicked |= poll_device_mouse();
    }
    return clicked;
  }

 private:
  static void CALLBACK device_callback(
      GameInputCallbackToken, void* context, IGameInputDevice* device,
      uint64_t, GameInputDeviceStatus currentStatus,
      GameInputDeviceStatus) {
    auto* self = static_cast<native_gameinput*>(context);
    if (!self || !device) return;
    const GameInputDeviceInfo* info = device->GetDeviceInfo();
    if (!info) return;
    const uint64_t sig = (static_cast<uint64_t>(info->usage.page) << 48) |
                         (static_cast<uint64_t>(info->usage.id) << 32) |
                         static_cast<uint32_t>(currentStatus);
    bool should_log = false;
    {
      std::lock_guard<std::mutex> lock(self->dev_mu_);
      if (self->logged_dev_sigs_.size() < 16 &&
          std::find(self->logged_dev_sigs_.begin(), self->logged_dev_sigs_.end(), sig) ==
              self->logged_dev_sigs_.end()) {
        self->logged_dev_sigs_.push_back(sig);
        should_log = true;
      }
    }
    if (should_log) {
      const char* name = (info->displayName && info->displayName->data)
          ? info->displayName->data : "(unnamed)";
      char line[320];
      std::snprintf(line, sizeof(line),
                    "input: GameInput dev vid=%04x pid=%04x fam=%d sup=0x%x "
                    "caps=0x%x st=0x%x usage=%04x:%04x rpts=%u name=%s",
                    info->vendorId, info->productId,
                    static_cast<int>(info->deviceFamily),
                    static_cast<unsigned>(info->supportedInput),
                    static_cast<unsigned>(info->capabilities),
                    static_cast<unsigned>(currentStatus),
                    info->usage.page, info->usage.id,
                    static_cast<unsigned>(info->inputReportCount), name);
      log_line(line);
      if (info->mouseInfo) {
        std::snprintf(line, sizeof(line),
                      "input: GameInput dev mouseInfo btns=0x%x rate=%u dpi=%u wx=%u wy=%u",
                      static_cast<unsigned>(info->mouseInfo->supportedButtons),
                      static_cast<unsigned>(info->mouseInfo->sampleRate),
                      static_cast<unsigned>(info->mouseInfo->sensorDpi),
                      info->mouseInfo->hasWheelX ? 1u : 0u,
                      info->mouseInfo->hasWheelY ? 1u : 0u);
        log_line(line);
      }
    }
    const bool is_mouse =
        (info->supportedInput & GameInputKindMouse) != 0 ||
        (info->usage.page == 0x0001u && info->usage.id == 0x0002u) ||
        info->mouseInfo != nullptr;
    const bool is_kbd =
        (info->supportedInput & GameInputKindKeyboard) != 0 ||
        (info->usage.page == 0x0001u && info->usage.id == 0x0006u);
    if (is_kbd) {
      self->keyboard_connected_.store((currentStatus & GameInputDeviceConnected) != 0);
    }
    if (is_mouse) {
      self->mouse_connected_.store((currentStatus & GameInputDeviceConnected) != 0);
    }
    if (is_mouse && (currentStatus & GameInputDeviceConnected)) {
      std::lock_guard<std::mutex> lock(self->dev_mu_);
      bool dup = false;
      for (const auto& d : self->mouse_devices_) {
        if (d.get() == device) { dup = true; break; }
      }
      if (!dup && self->mouse_devices_.size() < 4) {
        winrt::com_ptr<IGameInputDevice> ref;
        ref.copy_from(device);
        self->mouse_devices_.push_back(std::move(ref));
      }
    }
  }

  void probe_raw_report(IGameInputReading& reading, const char* tag) {
    winrt::com_ptr<IGameInputRawDeviceReport> rep;
    if (!reading.GetRawReport(rep.put()) || !rep) return;
    const size_t sz = rep->GetRawDataSize();
    if (sz == 0) return;
    std::array<uint8_t, 64> buf{};
    const size_t got = rep->GetRawData(std::min(sz, buf.size()), buf.data());
    if (raw_log_count_ >= 8) return;
    if (got == last_raw_len_ && std::memcmp(buf.data(), last_raw_.data(), got) == 0)
      return;
    last_raw_len_ = got;
    std::memcpy(last_raw_.data(), buf.data(), got);
    ++raw_log_count_;
    char line[256];
    int pos = std::snprintf(line, sizeof(line),
                            "input: GameInput %s raw len=%zu:", tag, got);
    for (size_t i = 0; i < got && i < 16 && pos + 4 < (int)sizeof(line); ++i)
      pos += std::snprintf(line + pos, sizeof(line) - pos, " %02x", buf[i]);
    log_line(line);
  }

  bool poll_device_mouse() {
    std::vector<winrt::com_ptr<IGameInputDevice>> devs;
    {
      std::lock_guard<std::mutex> lock(dev_mu_);
      devs = mouse_devices_;
    }
    bool clicked = false;
    for (size_t i = 0; i < devs.size(); ++i) {
      winrt::com_ptr<IGameInputReading> mread;
      const HRESULT hr_m = input_->GetCurrentReading(
          GameInputKindMouse, devs[i].get(), mread.put());
      if (SUCCEEDED(hr_m) && mread) {
        probe_raw_report(*mread, "dev-mouse");
        GameInputMouseState st{};
        if (mread->GetMouseState(&st) &&
            (st.positionX || st.positionY || st.wheelX || st.wheelY || st.buttons)) {
          if (!dev_mouse_hit_logged_) {
            dev_mouse_hit_logged_ = true;
            log_line("input: per-device GameInput mouse state non-zero!");
          }
          std::lock_guard<std::mutex> lock(mouse_mu_);
          clicked |= apply_mouse_state(st, dev_mouse_, dev_mouse_baseline_);
        }
      }
      winrt::com_ptr<IGameInputReading> rread;
      const HRESULT hr_r = input_->GetCurrentReading(
          GameInputKindRawDeviceReport, devs[i].get(), rread.put());
      if (SUCCEEDED(hr_r) && rread) probe_raw_report(*rread, "dev-raw");
    }
    return clicked;
  }

  void log_reading_status(GameInputKind kind, HRESULT result, IGameInputReading* reading) {
    auto& last_logged = kind == GameInputKindKeyboard
        ? keyboard_status_log_at_ : mouse_status_log_at_;
    auto& log_count = kind == GameInputKindKeyboard
        ? keyboard_status_logs_ : mouse_status_logs_;
    auto& prev_hr = kind == GameInputKindKeyboard
        ? keyboard_prev_hr_ : mouse_prev_hr_;
    const ULONGLONG now = GetTickCount64();
    const bool hr_changed = (result != prev_hr);
    if (!hr_changed && (now - last_logged < 15000 || log_count >= 4)) return;
    prev_hr = result;
    last_logged = now;
    ++log_count;
    char line[224]{};
    const auto sequence = reading ? reading->GetSequenceNumber(kind) : 0;
    const auto kind_actual = reading ? static_cast<unsigned>(reading->GetInputKind()) : 0u;
    if (kind == GameInputKindKeyboard) {
      std::array<GameInputKeyState, 256> states{};
      const auto count = reading ? std::min<size_t>(states.size(),
          reading->GetKeyState(static_cast<uint32_t>(states.size()), states.data())) : 0;
      std::snprintf(line, sizeof(line),
          "input: GameInput keyboard hr=0x%08lx seq=%llu kind=0x%x active=%zu",
          static_cast<unsigned long>(result), static_cast<unsigned long long>(sequence),
          kind_actual, count);
    } else {
      GameInputMouseState state{};
      const bool available = reading && reading->GetMouseState(&state);
      if (reading) probe_raw_report(*reading, "agg-mouse");
      std::snprintf(line, sizeof(line),
          "input: GameInput mouse hr=0x%08lx seq=%llu kind=0x%x state=%u x=%lld y=%lld wx=%lld wy=%lld buttons=0x%02x",
          static_cast<unsigned long>(result), static_cast<unsigned long long>(sequence),
          kind_actual, available ? 1u : 0u,
          static_cast<long long>(available ? state.positionX : 0),
          static_cast<long long>(available ? state.positionY : 0),
          static_cast<long long>(available ? state.wheelX : 0),
          static_cast<long long>(available ? state.wheelY : 0),
          available ? state.buttons : 0u);
    }
    log_line(line);
    // .211 DIAG: log the commit level at each input-read log so the
    // post-input-init death can be attributed (memory climbing to the
    // 5120MB cap vs a host crash vs a guest fault that never logs).
    {
      char mline[128];
      std::snprintf(mline, sizeof(mline),
          "input: mem committed=%lluMB budget=5120MB",
          static_cast<unsigned long long>(trt_mem_budget_committed() / (1024ULL * 1024ULL)));
      log_line(mline);
    }
  }

  static void CALLBACK mouse_reading_callback(
      GameInputCallbackToken, void* context, IGameInputReading* reading,
      bool /*hasOverrunOccurred*/) {
    auto* self = static_cast<native_gameinput*>(context);
    if (!self || !reading) return;
    std::lock_guard<std::mutex> lock(self->mouse_mu_);
    if (self->update_mouse(*reading)) self->mouse_click_pending_.store(true);
  }

  struct key {
    u16 scan{};
    bool extended{};
    bool down{};
    ULONGLONG pressed_at{};
    ULONGLONG repeated_at{};
  };

  void release_keyboard() {
    for (unsigned vk = 8; vk < keys_.size(); ++vk) {
      if (keys_[vk].down && trt_key_down(static_cast<u16>(vk)))
        trt_input_key_event2(static_cast<u16>(vk), false, keys_[vk].scan,
                             keys_[vk].extended, false);
    }
    keys_ = {};
    keyboard_active_.store(false);
  }

  void release_mouse() {
    for (unsigned button = 0; button < 5; ++button) {
      if (mouse_.buttons & (1u << button)) trt_input_mouse_button(button, false);
    }
    mouse_ = {};
    mouse_baseline_ = false;
    dev_mouse_ = {};
    dev_mouse_baseline_ = false;
    mouse_active_.store(false);
    last_mouse_ms_.store(0);
    last_wheel_ms_.store(0);
  }

  template<class Apply>
  void drain(GameInputKind kind, winrt::com_ptr<IGameInputReading>& last, Apply apply) {
    auto& missing_since = kind == GameInputKindKeyboard
        ? keyboard_missing_since_ : mouse_missing_since_;
    winrt::com_ptr<IGameInputReading> current;
    const HRESULT result = input_->GetCurrentReading(kind, nullptr, current.put());
    log_reading_status(kind, result, current.get());
    if (FAILED(result) || !current) {
      const auto now = GetTickCount64();
      if (!missing_since) missing_since = now;
      if (last && now - missing_since >= 500) {
        if (kind == GameInputKindKeyboard) release_keyboard();
        else release_mouse();
        last = nullptr;
      }
      return;
    }
    missing_since = 0;
    if (!last) {
      apply(*current);
      last = std::move(current);
      return;
    }

    const auto current_sequence = current->GetSequenceNumber(kind);
    bool advanced = false;
    for (unsigned i = 0; i < 64; ++i) {
      const auto last_sequence = last->GetSequenceNumber(kind);
      if (current_sequence && current_sequence <= last_sequence) break;
      winrt::com_ptr<IGameInputReading> next;
      if (FAILED(input_->GetNextReading(last.get(), kind, nullptr, next.put())) ||
          !next || next.get() == last.get()) break;
      const auto next_sequence = next->GetSequenceNumber(kind);
      if (next_sequence && next_sequence <= last_sequence) break;
      apply(*next);
      last = std::move(next);
      advanced = true;
    }
    const auto last_sequence = last->GetSequenceNumber(kind);
    const bool sequence_missing = current_sequence == 0 || last_sequence == 0;
    if (!advanced || sequence_missing || current_sequence > last_sequence) {
      if (!advanced && (sequence_missing || current_sequence > last_sequence)) {
        bool& reported = kind == GameInputKindKeyboard
            ? keyboard_snapshot_reported_ : mouse_snapshot_reported_;
        if (!reported) {
          reported = true;
          log_line(kind == GameInputKindKeyboard
                       ? "input: native keyboard uses latest readings"
                       : "input: native mouse uses sequence-less latest readings");
        }
      }
      apply(*current);
      last = std::move(current);
    }
  }

  void apply_keyboard_repeat() {
    const ULONGLONG now = GetTickCount64();
    const bool ctrl = keys_[0xa2].down || keys_[0xa3].down;
    const bool alt = keys_[0xa4].down || keys_[0xa5].down;
    for (unsigned vk = 8; vk < keys_.size(); ++vk) {
      auto& k = keys_[vk];
      if (!k.down || (vk >= 0xA0 && vk <= 0xA5) || vk == 0x10 || vk == 0x11 ||
          vk == 0x12 || vk == 0x14 || vk == 0x90 || vk == 0x91)
        continue;
      if (now - k.pressed_at < 450 || now - k.repeated_at < 35) continue;
      k.repeated_at = now;
      trt_input_key_event2(static_cast<u16>(vk), true, k.scan, k.extended, 1);
      if (!ctrl && !alt) {
        const u32 ch = trt_vk_to_char_ex(vk, nullptr);
        if (ch == '\b' || ch == '\t' || ch == '\r' || ch >= 32)
          trt_input_char_event(ch);
      }
    }
  }

  void update_keyboard(IGameInputReading& reading) {
    std::array<GameInputKeyState, 256> states{};
    const auto count = std::min<size_t>(
        states.size(), reading.GetKeyState(static_cast<uint32_t>(states.size()), states.data()));
    std::array<key, 256> next{};
    std::array<u32, 256> text{};
    const ULONGLONG now = GetTickCount64();
    for (size_t i = 0; i < count; ++i) {
      const auto& state = states[i];
      if (state.virtualKey >= 1 && state.virtualKey <= 6 && !low_vk_logged_) {
        low_vk_logged_ = true;
        char line[96];
        std::snprintf(line, sizeof(line),
                      "input: GameInput key reading saw low vk=%u", state.virtualKey);
        log_line(line);
      }
      const u32 scan = state.scanCode ? state.scanCode
                                      : trt_MapVirtualKeyW(state.virtualKey, 4);
      const bool extended = (scan & 0xff00u) == 0xe000u;
      u16 vk = state.virtualKey;
      if (vk == 0x10 || vk == 0x11 || vk == 0x12) {
        const u32 mapped = trt_MapVirtualKeyW(scan, 3);
        if (mapped) vk = static_cast<u16>(mapped);
      }
      if (vk < 8 || vk >= next.size() || (vk >= 195 && vk <= 218)) continue;
      const auto& prev = keys_[vk];
      next[vk] = {static_cast<u16>(scan & 0xffu), extended, true,
                  prev.down ? prev.pressed_at : now,
                  prev.down ? prev.repeated_at : now};
      if (!state.isDeadKey && state.codePoint >= 32) text[vk] = state.codePoint;
    }
    const bool shortcut = next[0xa2].down || next[0xa3].down ||
                          next[0xa4].down || next[0xa5].down;
    const bool interceptor_on = interceptor_seen_.load();
    for (unsigned vk = 8; vk < next.size(); ++vk) {
      if (keys_[vk].down == next[vk].down) continue;
      const auto& state = next[vk].down ? next[vk] : keys_[vk];
      const u16 vk16 = static_cast<u16>(vk);
      if (host_key_fn_ &&
          host_key_fn_(vk16, state.scan, state.extended, next[vk].down)) {
        if (!next[vk].down && trt_key_down(vk16)) {
          trt_input_key_event2(vk16, false, state.scan, state.extended ? 1 : 0, 0);
        }
        continue;
      }
      if (!keyboard_active_.exchange(true) && !keyboard_seen_) {
        keyboard_seen_ = true;
        log_line("input: native GameInput keyboard transition applied");
      }
      // If KeyboardDeliveryInterceptor already applied this key recently, skip
      // duplicate injection; still act as a stuck-key safety net after 60ms.
      const ULONGLONG last_ic = interceptor_last_ms_[vk].load();
      const bool recently_handled = interceptor_on && (now - last_ic < 60);
      if (!recently_handled && trt_key_down(static_cast<u16>(vk)) != next[vk].down) {
        trt_input_key_event2(static_cast<u16>(vk), next[vk].down, state.scan,
                             state.extended, false);
        if (next[vk].down && !shortcut) {
          u32 ch = text[vk];
          if (!ch) ch = trt_vk_to_char_ex(vk, nullptr);
          if (ch == '\b' || ch == '\t' || ch == '\r' || ch >= 32)
            trt_input_char_event(ch);
        }
      }
    }
    keys_ = next;
  }

  bool apply_mouse_state(const GameInputMouseState& next, GameInputMouseState& prev,
                         bool& baseline) {
    if (baseline) {
      const auto delta = [](int64_t after, int64_t before) {
        return static_cast<i32>(std::clamp(after - before, int64_t(-32767), int64_t(32767)));
      };
      const auto dx = delta(next.positionX, prev.positionX);
      const auto dy = delta(next.positionY, prev.positionY);
      if (dx || dy) {
        mouse_active_.store(true);
        last_mouse_ms_.store(GetTickCount64());
        if (!mouse_motion_seen_) {
          mouse_motion_seen_ = true;
          log_line("input: native GameInput mouse movement applied");
        }
        trt_input_mouse_delta_raw(dx, dy);
      }
      const auto wheel = delta(next.wheelY, prev.wheelY);
      if (wheel) {
        mouse_active_.store(true);
        const auto now = GetTickCount64();
        last_mouse_ms_.store(now);
        last_wheel_ms_.store(now);
        trt_input_wheel(wheel);
      }
    }
    const unsigned changed = next.buttons ^ prev.buttons;
    bool clicked = false;
    for (unsigned button = 0; button < 5; ++button) {
      const unsigned mask = 1u << button;
      if (!(changed & mask)) continue;
      mouse_active_.store(true);
      last_mouse_ms_.store(GetTickCount64());
      if (!mouse_button_seen_) {
        mouse_button_seen_ = true;
        log_line("input: native GameInput mouse button applied");
      }
      const bool down = (next.buttons & mask) != 0;
      const bool transitioned = trt_input_mouse_button(button, down);
      clicked |= transitioned && button == 0 && !down;
    }
    prev = next;
    baseline = true;
    return clicked;
  }

  bool update_mouse(IGameInputReading& reading) {
    GameInputMouseState next{};
    if (!reading.GetMouseState(&next)) return false;
    return apply_mouse_state(next, mouse_, mouse_baseline_);
  }

  winrt::com_ptr<IGameInput> input_;
  winrt::com_ptr<IGameInputReading> keyboard_reading_;
  winrt::com_ptr<IGameInputReading> mouse_reading_;
  std::mutex dev_mu_;
  std::vector<winrt::com_ptr<IGameInputDevice>> mouse_devices_;
  std::vector<uint64_t> logged_dev_sigs_;
  std::array<key, 256> keys_{};
  std::array<std::atomic<ULONGLONG>, 256> interceptor_last_ms_{};
  std::atomic<bool> interceptor_seen_{false};
  GameInputMouseState mouse_{};
  bool mouse_baseline_ = false;
  GameInputMouseState dev_mouse_{};
  bool dev_mouse_baseline_ = false;
  bool keyboard_seen_ = false;
  bool mouse_motion_seen_ = false;
  bool mouse_button_seen_ = false;
  bool dev_mouse_hit_logged_ = false;
  bool low_vk_logged_ = false;
  bool keyboard_snapshot_reported_ = false;
  bool mouse_snapshot_reported_ = false;
  ULONGLONG keyboard_status_log_at_ = 0;
  ULONGLONG mouse_status_log_at_ = 0;
  unsigned keyboard_status_logs_ = 0;
  unsigned mouse_status_logs_ = 0;
  HRESULT keyboard_prev_hr_ = E_FAIL;
  HRESULT mouse_prev_hr_ = E_FAIL;
  ULONGLONG keyboard_missing_since_ = 0;
  ULONGLONG mouse_missing_since_ = 0;
  std::array<uint8_t, 64> last_raw_{};
  size_t last_raw_len_ = 0;
  unsigned raw_log_count_ = 0;
  GameInputCallbackToken mouse_callback_token_ = GAMEINPUT_INVALID_CALLBACK_TOKEN_VALUE;
  GameInputCallbackToken device_callback_token_ = GAMEINPUT_INVALID_CALLBACK_TOKEN_VALUE;
  std::mutex mouse_mu_;
  std::atomic<bool> mouse_callback_active_{false};
  std::atomic<bool> mouse_click_pending_{false};
  host_key_fn host_key_fn_ = nullptr;
  std::atomic<bool> keyboard_active_{false};
  std::atomic<bool> mouse_active_{false};
  std::atomic<ULONGLONG> last_mouse_ms_{0};
  std::atomic<ULONGLONG> last_wheel_ms_{0};
  std::atomic<bool> mouse_connected_{false};
  std::atomic<bool> keyboard_connected_{false};
};

}  // namespace longan
