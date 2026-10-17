#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace steam {
  
struct PadState {
  std::array<bool, 16> buttons{};
  std::array<float, 4> sticks{};
  std::array<float, 2> triggers{};
};

bool initialize_input(const std::string& manifest_path);
bool input_active();
std::vector<uint64_t> connected_pads();
std::optional<PadState> read_pad(uint64_t pad);
bool pad_uses_full_layout(uint64_t pad);
void show_layout_panel(uint64_t pad);
void vibrate_pad(uint64_t pad, uint16_t left, uint16_t right);
std::string pad_name(uint64_t pad);
bool pad_has_led(uint64_t pad);
void set_pad_led(uint64_t pad, uint8_t red, uint8_t green, uint8_t blue);

}  // namespace steam
