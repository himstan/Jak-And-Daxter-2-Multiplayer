#include "steam_pad.h"

#include <algorithm>
#include <string>

#include "common/log/log.h"

#include "game/system/hid/device_claim.h"

namespace {
constexpr auto kLayoutCheckGrace = std::chrono::seconds(5);
constexpr auto kLayoutCheckInterval = std::chrono::seconds(1);

std::string claim_key(const uint64_t pad) {
  return "steam-input-" + std::to_string(pad);
}

int stick_position(const float value, const float axis_scale) {
  return std::clamp(PadData::ANALOG_NEUTRAL + static_cast<int>(value * axis_scale * 128.0f), 0,
                    255);
}
}  // namespace

SteamPadDevice::~SteamPadDevice() {
  release_pad();
}

void SteamPadDevice::refresh_pads() {
  if (auto pads = steam::connected_pads(); pads != m_pads) {
    m_pads = std::move(pads);
  }
}

void SteamPadDevice::poll_state(const std::shared_ptr<PadData>& data,
                                const float axis_scale,
                                const bool focused) {
  if (m_pad && std::ranges::find(m_pads, *m_pad) == m_pads.end()) {
    release(data);
  }
  claim_requested_pad(data);
  if (!m_pad) {
    claim_pad();
  }
  const auto state = m_pad ? steam::read_pad(*m_pad) : std::nullopt;
  if (!state) {
    clear_actions(data);
    return;
  }
  check_layout(focused);
  apply_buttons(data, *state);
  if (!data->analogs_being_simulated()) {
    data->analog_data = {stick_position(state->sticks[0], axis_scale),
                         stick_position(-state->sticks[1], axis_scale),
                         stick_position(state->sticks[2], axis_scale),
                         stick_position(-state->sticks[3], axis_scale)};
  }
  m_state = state;
}

void SteamPadDevice::clear_actions(const std::shared_ptr<PadData>& data) {
  if (!m_state) {
    return;
  }
  apply_buttons(data, {});
  if (!data->analogs_being_simulated()) {
    data->analog_data.fill(PadData::ANALOG_NEUTRAL);
  }
  m_state.reset();
}

void SteamPadDevice::release(const std::shared_ptr<PadData>& data) {
  clear_actions(data);
  release_pad();
}

void SteamPadDevice::set_led(const u8 red, const u8 green, const u8 blue) {
  const std::array<u8, 3> color = {red, green, blue};
  if (!m_pad || m_led == color) {
    return;
  }
  steam::set_pad_led(*m_pad, red, green, blue);
  m_led = color;
}

int SteamPadDevice::send_rumble(const u8 low_intensity, const u8 high_intensity) const {
  if (m_pad) {
    steam::vibrate_pad(*m_pad, low_intensity * 257, high_intensity * 257);
  }
  return 0;
}

void SteamPadDevice::claim_requested_pad(const std::shared_ptr<PadData>& data) {
  const auto requested = m_requested_pad.exchange(0);
  if (requested == 0 || requested == m_pad ||
      std::ranges::find(m_pads, requested) == m_pads.end()) {
    return;
  }
  const auto claim = claim_device(claim_key(requested));
  if (!claim) {
    lg::warn("Steam Input controller {} is already claimed by another game process", requested);
    return;
  }
  release(data);
  assign_pad(requested, *claim);
}

void SteamPadDevice::claim_pad() {
  for (const auto pad : m_pads) {
    if (const auto claim = claim_device(claim_key(pad))) {
      assign_pad(pad, *claim);
      return;
    }
  }
}

void SteamPadDevice::assign_pad(const uint64_t pad, void* claim) {
  m_pad = pad;
  m_claim = claim;
  m_next_layout_check = std::chrono::steady_clock::now() + kLayoutCheckGrace;
  lg::info("Claimed Steam Input controller {} for port 0", pad);
}

void SteamPadDevice::release_pad() {
  if (!m_pad) {
    return;
  }
  release_device(m_claim);
  m_claim = nullptr;
  m_pad.reset();
  m_led.reset();
  m_layout_mismatch = false;
}

void SteamPadDevice::check_layout(const bool focused) {
  const auto now = std::chrono::steady_clock::now();
  // Steam Input applies the game layout only to the focused window; others see the desktop layout.
  if (!focused) {
    m_next_layout_check = now + kLayoutCheckGrace;
    return;
  }
  if (now < m_next_layout_check) {
    return;
  }
  m_next_layout_check = now + kLayoutCheckInterval;
  m_layout_mismatch = !steam::pad_uses_full_layout(*m_pad);
  if (m_layout_mismatch && !m_layout_panel_shown) {
    lg::warn("Steam Input controller {} is not using the OpenGOAL Gamepad layout", *m_pad);
    steam::show_layout_panel(*m_pad);
    m_layout_panel_shown = true;
  }
}

void SteamPadDevice::apply_buttons(const std::shared_ptr<PadData>& data,
                                   const steam::PadState& state) const {
  for (size_t index = 0; index < state.buttons.size(); ++index) {
    const bool pressed = state.buttons[index];
    if (m_state && m_state->buttons[index] == pressed) {
      continue;
    }
    data->button_data.at(index) = pressed;
    const auto pressure =
        data->button_index_to_pressure_index(static_cast<PadData::ButtonIndex>(index));
    if (pressure != PadData::PressureIndex::INVALID_PRESSURE) {
      data->pressure_data.at(pressure) = pressed ? 255 : 0;
    }
  }
}
