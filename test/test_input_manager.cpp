#include <chrono>
#include <cstddef>
#include <thread>

#include "common/util/FileUtil.h"

#include "game/external/steam/steam_bindings.h"
#include "game/external/steam/steam_client.h"
#include "game/system/hid/device_claim.h"
#include "game/system/hid/input_manager.h"
#include "gtest/gtest.h"

TEST(SteamControllerLayouts, ConfigurationCallbacksTrackEmulationAndRejectInvalidPayloads) {
  using namespace steam::detail;
#if defined(VALVE_CALLBACK_PACK_SMALL)
  static_assert(sizeof(InputConfigurationLoaded) == 32);
  static_assert(offsetof(InputConfigurationLoaded, uses_gamepad) == 29);
#else
  static_assert(sizeof(InputConfigurationLoaded) == 40);
  static_assert(offsetof(InputConfigurationLoaded, uses_gamepad) == 33);
#endif
  InputConfigurationLoaded configuration = {
      .app_id = steam::kSpacewarAppId,
      .controller = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()),
      .uses_actions = true,
      .uses_gamepad = false,
  };
  CallbackMessage message = {.id = InputConfigurationLoaded::kId,
                             .data = reinterpret_cast<uint8_t*>(&configuration),
                             .size = sizeof(configuration)};
  EXPECT_FALSE(controller_layout_uses_gamepad(configuration.controller).has_value());
  dispatch_callback(message);
  EXPECT_EQ(controller_layout_uses_gamepad(configuration.controller), false);
  configuration.uses_gamepad = true;
  dispatch_callback(message);
  EXPECT_EQ(controller_layout_uses_gamepad(configuration.controller), true);

  configuration.uses_gamepad = false;
  --message.size;
  dispatch_callback(message);
  EXPECT_EQ(controller_layout_uses_gamepad(configuration.controller), true);
  message.size = sizeof(configuration);
  ++message.id;
  dispatch_callback(message);
  EXPECT_EQ(controller_layout_uses_gamepad(configuration.controller), true);
  message.id = InputConfigurationLoaded::kId;
  message.data = nullptr;
  dispatch_callback(message);
  EXPECT_EQ(controller_layout_uses_gamepad(configuration.controller), true);
  message.data = reinterpret_cast<uint8_t*>(&configuration);
  configuration.app_id = 1;
  dispatch_callback(message);
  EXPECT_EQ(controller_layout_uses_gamepad(configuration.controller), true);
  configuration.app_id = steam::kSpacewarAppId;
  configuration.uses_gamepad = true;
  dispatch_callback(message);
}

TEST(DeviceClaim, ExcludesOtherOwnersAndReleasesAcrossThreads) {
  const auto key = "controller-test-" +
                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  auto first = DeviceClaim::acquire(key);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(DeviceClaim::acquire(key), nullptr);
  EXPECT_NE(DeviceClaim::acquire(key + "-other"), nullptr);
  std::thread releaser([claim = std::move(first)]() mutable { claim.reset(); });
  releaser.join();
  EXPECT_NE(DeviceClaim::acquire(key), nullptr);
}

class InputManagerControllers : public testing::Test {
 protected:
  void SetUp() override {
    directory = fs::temp_directory_path() /
                ("opengoal-controller-test-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    file_util::override_user_config_dir(directory, true);
    const char* driver = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    previous_driver = driver ? driver : "";
    const char* filter = SDL_GetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT);
    previous_filter = filter ? filter : "";
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0x1234/0x4321");
    ASSERT_TRUE(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD));
    window = SDL_CreateWindow("controller-test", 1, 1, SDL_WINDOW_HIDDEN);
    ASSERT_NE(window, nullptr);
    first = attach();
    second = attach();
    ASSERT_NE(first, 0u);
    ASSERT_NE(second, 0u);
  }

  void TearDown() override {
    for (const auto id : devices) {
      if (SDL_IsJoystickVirtual(id)) {
        SDL_DetachVirtualJoystick(id);
      }
    }
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
    if (previous_driver.empty()) {
      SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
    } else {
      SDL_SetHint(SDL_HINT_VIDEO_DRIVER, previous_driver.c_str());
    }
    if (previous_filter.empty()) {
      SDL_ResetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT);
    } else {
      SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, previous_filter.c_str());
    }
    file_util::override_user_config_dir({}, true);
    fs::remove_all(directory);
  }

  SDL_JoystickID attach() {
    SDL_VirtualJoystickDesc description = {};
    SDL_INIT_INTERFACE(&description);
    description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    description.naxes = SDL_GAMEPAD_AXIS_COUNT;
    description.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    description.vendor_id = 0x1234;
    description.product_id = 0x4321;
    description.name = "OpenGOAL test controller";
    const auto id = SDL_AttachVirtualJoystick(&description);
    devices.push_back(id);
    return id;
  }

  void refresh(InputManager& input) {
    SDL_Event event = {};
    event.type = SDL_EVENT_GAMEPAD_ADDED;
    input.process_sdl_event(event);
  }

  void button(InputManager& input, const SDL_JoystickID id, const bool down) {
    SDL_Event event = {};
    event.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    event.gbutton.which = id;
    event.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
    input.process_sdl_event(event);
  }

  bool pressed(InputManager& input, const int port = 0) {
    return input.get_current_data(port).value()->button_data.at(PadData::CROSS);
  }

  fs::path directory;
  std::string previous_driver;
  std::string previous_filter;
  SDL_Window* window = nullptr;
  SDL_JoystickID first = 0;
  SDL_JoystickID second = 0;
  std::vector<SDL_JoystickID> devices;
};

TEST_F(InputManagerControllers, FreshSettingsAssignIdenticalControllersToDifferentOwners) {
  ASSERT_FALSE(fs::exists(directory));
  InputManager one(window);
  InputManager two(window);
  EXPECT_NE(one.get_controller_index(0), two.get_controller_index(0));
  button(one, first, true);
  button(two, first, true);
  EXPECT_TRUE(pressed(one));
  EXPECT_FALSE(pressed(two));
  button(two, second, true);
  EXPECT_TRUE(pressed(two));
}

TEST_F(InputManagerControllers, NoticeWithoutSteamDoesNotCaptureSettingsShortcut) {
  InputManager input(window);
  int presses = 0;
  input.register_command(CommandBinding::Source::KEYBOARD,
                         CommandBinding(SDLK_F1, [&]() { ++presses; }));
  EXPECT_FALSE(input.controller_layout_notice(true));
  input.finish_polling();
  EXPECT_FALSE(input.controller_layout_notice(true));
  SDL_Event event = {};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.key = SDLK_F1;
  event.key.scancode = SDL_SCANCODE_F1;
  input.process_sdl_event(event);
  EXPECT_EQ(presses, 1);
  EXPECT_FALSE(input.controller_layout_notice(false));
}

TEST_F(InputManagerControllers, HotplugPreservesAssignmentsAndHeldButtons) {
  InputManager one(window);
  InputManager two(window);
  button(two, second, true);
  ASSERT_NE(attach(), 0u);
  refresh(two);
  refresh(one);
  EXPECT_TRUE(pressed(two));
  button(two, first, true);
  button(one, second, true);
  EXPECT_FALSE(pressed(one));
  button(two, second, false);
  EXPECT_FALSE(pressed(two));
}

TEST_F(InputManagerControllers, DisconnectClearsButtonsAndFallsBackToKeyboard) {
  ASSERT_TRUE(SDL_DetachVirtualJoystick(second));
  InputManager owner(window);
  InputManager waiting(window);
  EXPECT_EQ(waiting.get_controller_index(0), -1);
  EXPECT_TRUE(waiting.is_keyboard_enabled());
  button(owner, first, true);
  ASSERT_TRUE(pressed(owner));
  ASSERT_TRUE(SDL_DetachVirtualJoystick(first));
  refresh(owner);
  EXPECT_FALSE(pressed(owner));
  EXPECT_EQ(owner.get_controller_index(0), -1);
  EXPECT_TRUE(owner.is_keyboard_enabled());
}

TEST_F(InputManagerControllers, RemovingEarlierIdenticalPadDoesNotTakeTheSurvivor) {
  InputManager one(window);
  InputManager two(window);
  ASSERT_TRUE(SDL_DetachVirtualJoystick(first));
  refresh(one);
  refresh(two);
  EXPECT_EQ(one.get_controller_index(0), -1);
  button(one, second, true);
  button(two, second, true);
  EXPECT_FALSE(pressed(one));
  EXPECT_TRUE(pressed(two));
  const auto replacement = attach();
  ASSERT_NE(replacement, 0u);
  refresh(one);
  refresh(two);
  button(one, replacement, true);
  EXPECT_TRUE(pressed(one));
  button(two, second, false);
  EXPECT_FALSE(pressed(two));
}

TEST_F(InputManagerControllers, ManualSelectionCannotTakeAnotherOwnersController) {
  InputManager one(window);
  InputManager two(window);
  const int selected = two.get_controller_index(0);
  two.set_controller_for_port(one.get_controller_index(0), 0);
  two.process_ee_events();
  EXPECT_EQ(two.get_controller_index(0), selected);
  one.set_controller_for_port(-1, 0);
  one.process_ee_events();
  EXPECT_GE(one.get_controller_index(0), 0);
}

TEST_F(InputManagerControllers, QueuedSelectionUsesDeviceIdentityAfterHotplug) {
  InputManager input(window);
  const int other = input.get_controller_index(0) + 1;
  input.set_controller_for_port(other, 1);
  ASSERT_TRUE(SDL_DetachVirtualJoystick(first));
  refresh(input);
  input.process_ee_events();
  EXPECT_EQ(input.get_controller_index(0), -1);
  button(input, second, true);
  EXPECT_TRUE(pressed(input, 1));
  EXPECT_FALSE(pressed(input));
}
