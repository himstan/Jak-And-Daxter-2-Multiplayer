#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "common/common_types.h"

#include "game/external/steam/steam_input.h"
#include "game/system/hid/input_bindings.h"

class SteamPadDevice {
 public:
  SteamPadDevice() = default;
  SteamPadDevice(const SteamPadDevice&) = delete;
  SteamPadDevice& operator=(const SteamPadDevice&) = delete;
  ~SteamPadDevice();

  void refresh_pads();
  void poll_state(const std::shared_ptr<PadData>& data, float axis_scale, bool focused);
  void clear_actions(const std::shared_ptr<PadData>& data);
  void release(const std::shared_ptr<PadData>& data);
  void select(uint64_t pad) { m_requested_pad = pad; }
  const std::vector<uint64_t>& pads() const { return m_pads; }
  std::optional<uint64_t> pad() const { return m_pad; }
  bool connected() const { return m_pad.has_value(); }
  bool layout_mismatch() const { return m_layout_mismatch; }
  bool has_led() const { return m_pad && steam::pad_has_led(*m_pad); }
  void set_led(u8 red, u8 green, u8 blue);
  int send_rumble(u8 low_intensity, u8 high_intensity) const;

 private:
  void claim_requested_pad(const std::shared_ptr<PadData>& data);
  void claim_pad();
  void assign_pad(uint64_t pad, void* claim);
  void release_pad();
  void check_layout(bool focused);
  void apply_buttons(const std::shared_ptr<PadData>& data, const steam::PadState& state) const;

  std::vector<uint64_t> m_pads;
  std::atomic<uint64_t> m_requested_pad = 0;
  std::optional<uint64_t> m_pad;
  void* m_claim = nullptr;
  std::optional<steam::PadState> m_state;
  std::optional<std::array<u8, 3>> m_led;
  std::chrono::steady_clock::time_point m_next_layout_check{};
  bool m_layout_mismatch = false;
  bool m_layout_panel_shown = false;
};
