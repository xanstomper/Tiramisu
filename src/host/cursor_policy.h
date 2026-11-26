// Cursor ownership is independent of Xbox/WinRT types so it can be checked offline.
#pragma once
#include <cstdint>

namespace longan {

enum class cursor_owner { system, launcher, guest, hidden };

struct cursor_context {
  bool focused;
  bool loading;
  bool guest_running;
  bool quick_menu;
  bool launcher_can_draw;
  bool guest_can_draw;
  bool physical_input_seen;
  bool virtual_pointer;
  bool controller_active;
};

constexpr bool pointer_event_recent(uint64_t now, uint64_t event, uint64_t window) {
  return event && now >= event && now - event < window;
}

constexpr cursor_owner choose_cursor(cursor_context c) {
  if (!c.focused || c.loading || (!c.physical_input_seen && !c.virtual_pointer))
    return cursor_owner::system;
  const bool guest = c.guest_running && !c.quick_menu;
  if (guest ? !c.guest_can_draw : !c.launcher_can_draw)
    return cursor_owner::system;
  if (!guest && c.controller_active && !c.virtual_pointer)
    return cursor_owner::hidden;
  return guest ? cursor_owner::guest : cursor_owner::launcher;
}

}  // namespace longan
