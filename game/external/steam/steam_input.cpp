#include "game/external/steam/steam_input.h"

#include <limits>

#include "common/log/log.h"

#include "game/external/steam/steam_bindings.h"

namespace steam {
namespace {
using namespace detail;

constexpr uint64_t kAllControllers = std::numeric_limits<uint64_t>::max();
constexpr const char* kActionSet = "Gameplay";
constexpr std::array<const char*, 16> kButtonActions = {
    "Select", "L3",    "R3", "Start", "DpadUp",   "DpadRight", "DpadDown", "DpadLeft",
    nullptr,  nullptr, "L1", "R1",    "Triangle", "Circle",    "Cross",    "Square"};
constexpr std::array<const char*, 2> kStickActions = {"LeftStick", "RightStick"};
constexpr std::array<const char*, 2> kTriggerActions = {"L2", "R2"};
constexpr size_t kFirstTriggerButton = 8;
constexpr float kTriggerPressThreshold = 0.25f;
constexpr std::array<const char*, 15> kPadTypeNames = {"Controller",
                                                       "Steam Controller",
                                                       "Xbox 360 Controller",
                                                       "Xbox One Controller",
                                                       "Gamepad",
                                                       "PS4 Controller",
                                                       "MFi Controller",
                                                       "Android Controller",
                                                       "Joy-Con Pair",
                                                       "Joy-Con",
                                                       "Switch Pro Controller",
                                                       "Touch Controller",
                                                       "PS3 Controller",
                                                       "PS5 Controller",
                                                       "Steam Deck"};
constexpr int kPs4Controller = 5;
constexpr int kPs5Controller = 13;
constexpr unsigned int kSetLedColor = 0;

struct ActionHandles {
  uint64_t action_set = 0;
  std::array<uint64_t, 16> buttons{};
  std::array<uint64_t, 2> sticks{};
  std::array<uint64_t, 2> triggers{};
};

bool g_initialized = false;
ActionHandles g_handles;

bool resolve_handles(const InputApi& api) {
  if (g_handles.action_set != 0)
    return true;
  const auto action_set = api.action_set(api.input, kActionSet);
  if (action_set == 0)
    return false;
  for (size_t index = 0; index < kButtonActions.size(); ++index) {
    if (kButtonActions[index])
      g_handles.buttons[index] = api.digital_action(api.input, kButtonActions[index]);
  }
  for (size_t index = 0; index < kStickActions.size(); ++index)
    g_handles.sticks[index] = api.analog_action(api.input, kStickActions[index]);
  for (size_t index = 0; index < kTriggerActions.size(); ++index)
    g_handles.triggers[index] = api.analog_action(api.input, kTriggerActions[index]);
  g_handles.action_set = action_set;
  return true;
}

const InputApi* ready_input_api() {
  const auto* api = input_api();
  return api && g_initialized && g_handles.action_set != 0 ? api : nullptr;
}

bool action_bound(const InputApi& api,
                  const uint64_t pad,
                  const uint64_t action,
                  int (*origins)(void*, uint64_t, uint64_t, uint64_t, int*)) {
  std::array<int, kMaximumActionOrigins> found{};
  return action != 0 && origins(api.input, pad, g_handles.action_set, action, found.data()) > 0;
}
}  // namespace

bool initialize_input(const std::string& manifest_path) {
  const auto* api = input_api();
  if (!api || g_initialized)
    return g_initialized;
  if (!api->init(api->input, true)) {
    lg::warn("[Steam] Steam Input could not start.");
    return false;
  }
  if (!api->set_manifest(api->input, manifest_path.c_str()))
    lg::warn("[Steam] Steam Input rejected the action manifest at {}.", manifest_path);
  g_initialized = true;
  lg::info("[Steam] Steam Input is available for controllers.");
  return true;
}

bool input_active() {
  return g_initialized;
}

void detail::shutdown_input() {
  if (const auto* api = input_api(); api && g_initialized)
    api->shutdown(api->input);
  g_initialized = false;
  g_handles = {};
}

std::vector<uint64_t> connected_pads() {
  const auto* api = input_api();
  if (!api || !g_initialized)
    return {};
  api->run_frame(api->input, false);
  std::array<uint64_t, kMaximumInputControllers> pads{};
  const int count = api->connected_controllers(api->input, pads.data());
  if (count <= 0 || !resolve_handles(*api))
    return {};
  api->activate_action_set(api->input, kAllControllers, g_handles.action_set);
  return {pads.begin(), pads.begin() + count};
}

std::optional<PadState> read_pad(const uint64_t pad) {
  const auto* api = ready_input_api();
  if (!api)
    return std::nullopt;
  PadState state;
  for (size_t index = 0; index < kButtonActions.size(); ++index) {
    if (g_handles.buttons[index] != 0)
      state.buttons[index] = api->digital_data(api->input, pad, g_handles.buttons[index]).state;
  }
  for (size_t index = 0; index < kStickActions.size(); ++index) {
    const auto stick = api->analog_data(api->input, pad, g_handles.sticks[index]);
    state.sticks[index * 2] = stick.x;
    state.sticks[index * 2 + 1] = stick.y;
  }
  for (size_t index = 0; index < kTriggerActions.size(); ++index) {
    state.triggers[index] = api->analog_data(api->input, pad, g_handles.triggers[index]).x;
    state.buttons[kFirstTriggerButton + index] = state.triggers[index] > kTriggerPressThreshold;
  }
  return state;
}

bool pad_uses_full_layout(const uint64_t pad) {
  const auto* api = ready_input_api();
  if (!api)
    return true;
  for (size_t index = 0; index < kButtonActions.size(); ++index) {
    if (kButtonActions[index] &&
        !action_bound(*api, pad, g_handles.buttons[index], api->digital_origins)) {
      return false;
    }
  }
  for (const auto action : g_handles.sticks) {
    if (!action_bound(*api, pad, action, api->analog_origins))
      return false;
  }
  for (const auto action : g_handles.triggers) {
    if (!action_bound(*api, pad, action, api->analog_origins))
      return false;
  }
  return true;
}

void show_layout_panel(const uint64_t pad) {
  if (const auto* api = ready_input_api())
    api->show_binding_panel(api->input, pad);
}

void vibrate_pad(const uint64_t pad, const uint16_t left, const uint16_t right) {
  if (const auto* api = ready_input_api())
    api->vibrate(api->input, pad, left, right);
}

std::string pad_name(const uint64_t pad) {
  const auto* api = ready_input_api();
  const int type = api ? api->input_type(api->input, pad) : 0;
  const auto* name =
      type > 0 && type < (int)kPadTypeNames.size() ? kPadTypeNames[type] : kPadTypeNames[0];
  return std::string(name) + " (Steam Input)";
}

bool pad_has_led(const uint64_t pad) {
  const auto* api = ready_input_api();
  const int type = api ? api->input_type(api->input, pad) : 0;
  return type == kPs4Controller || type == kPs5Controller;
}

void set_pad_led(const uint64_t pad, const uint8_t red, const uint8_t green, const uint8_t blue) {
  if (const auto* api = ready_input_api())
    api->set_led(api->input, pad, red, green, blue, kSetLedColor);
}

}  // namespace steam
