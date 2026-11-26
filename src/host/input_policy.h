#pragma once

#include <string_view>

namespace longan {

enum class guest_input_mode { automatic, controller, keyboard_mouse, pad_as_mouse };
enum class guest_pad_route { physical, keyboard, disabled };

constexpr guest_input_mode parse_input_mode(std::string_view name) {
  if (name == "controller") return guest_input_mode::controller;
  if (name == "kbm") return guest_input_mode::keyboard_mouse;
  if (name == "pad2kbm") return guest_input_mode::pad_as_mouse;
  return guest_input_mode::automatic;
}

constexpr guest_input_mode resolve_input_mode(std::string_view defaults,
                                               std::string_view override) {
  const auto mode = parse_input_mode(override);
  return mode == guest_input_mode::automatic ? parse_input_mode(defaults) : mode;
}

constexpr guest_pad_route route_gamepads(guest_input_mode mode, bool any_physical,
                                          bool pad_as_mouse) {
  if (mode == guest_input_mode::keyboard_mouse || pad_as_mouse)
    return guest_pad_route::disabled;
  if (mode == guest_input_mode::controller && !any_physical)
    return guest_pad_route::keyboard;
  return guest_pad_route::physical;
}

}  // namespace longan
