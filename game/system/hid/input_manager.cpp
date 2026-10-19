#include "input_manager.h"

#include <algorithm>

#include "sdl_util.h"

#include "common/global_profiler/GlobalProfiler.h"
#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "game/external/steam/steam_client.h"
#include "game/runtime.h"

#include "third-party/SDL/include/SDL3/SDL_clipboard.h"
#include "third-party/SDL/include/SDL3/SDL_hints.h"

InputManager::InputManager(SDL_Window* window)
    : m_window(window),
      // Load user settings
      m_settings(std::make_shared<game_settings::InputSettings>(game_settings::InputSettings())),
      m_text_editor({
          .read = []() {
            char* text = SDL_GetClipboardText();
            const std::string result = text ? text : "";
            SDL_free(text);
            return result;
          },
          .write = [](const std::string_view text) {
            const std::string terminated(text);
            SDL_SetClipboardText(terminated.c_str());
          },
      }) {
  prof().instant_event("ROOT");
  {
    auto p = scoped_prof("input_manager::init");
    m_settings->load_settings();
#ifdef WIN32
    if (!SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3_SIXAXIS_DRIVER, "1")) {
      sdl_util::log_error("Unable to set SDL_HINT_JOYSTICK_HIDAPI_PS3_SIXAXIS_DRIVER to true!");
    }
#else
    if (!SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3, "1")) {
      sdl_util::log_error("Unable to set SDL_HINT_JOYSTICK_HIDAPI_PS3 to true!");
    }
#endif
    SDL_setenv_unsafe("SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD", "1", 0);
    steam::update_controller_input();
    ignore_background_controller_events(false);
    {
      auto p = scoped_prof("input_manager::init::sdl_init_subsystem");
      // initializing the controllers on startup can sometimes take a very long time
      // so we isolate that to here instead
      if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        sdl_util::log_error(
            "Could not initialize SDL Controller support, controllers will not work!");
      }
    }

    // Update to latest controller DB file
    std::string mapping_path =
        (file_util::get_jak_project_dir() / "game" / "assets" / "sdl_controller_db.txt").string();
    if (file_util::file_exists(mapping_path)) {
      SDL_AddGamepadMappingsFromFile(mapping_path.c_str());
    } else {
      lg::error("Could not find SDL Controller DB at path `{}`", mapping_path);
    }
    // Initialize atleast 2 ports, because that's normal for Jak
    // more will be allocated if more controllers are found
    m_data[0] = std::make_shared<PadData>();
    m_data[1] = std::make_shared<PadData>();
    m_keyboard = KeyboardDevice(m_settings);
    m_mouse = MouseDevice(m_window, m_settings);

    if (m_data.find(m_keyboard_and_mouse_port) == m_data.end()) {
      m_data[m_keyboard_and_mouse_port] = std::make_shared<PadData>();
    }
    m_command_binds = CommandBindingGroups();
    refresh_device_list();
    hide_cursor(m_auto_hide_mouse);
  }
}

InputManager::~InputManager() {
  prof().instant_event("ROOT");
  {
    auto p = scoped_prof("input_manager::destroy");
    for (const auto& [port, index] : m_controller_port_mapping) {
      m_available_controllers.at(index)->clear_trigger_effect(
          dualsense_effects::TriggerEffectOption::BOTH);
    }
    for (auto& device : m_available_controllers) {
      device->close_device();
    }
    m_settings->save_settings();
    if (m_text_editor.active()) {
      SDL_StopTextInput(m_window);
    }
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  }
}

void InputManager::refresh_device_list(const SDL_JoystickID preferred_remote) {
  prof().instant_event("ROOT");
  auto p = scoped_prof("input_manager::refresh_device_list");
  auto previous = std::move(m_available_controllers);
  const auto previous_mapping = std::move(m_controller_port_mapping);
  m_available_controllers.clear();
  m_controller_port_mapping.clear();
  int count = 0;
  auto gamepads = SDL_GetGamepads(&count);
  for (int i = 0; i < count; ++i) {
    const auto found = std::find_if(previous.begin(), previous.end(), [&](const auto& controller) {
      return controller->get_id() == gamepads[i];
    });
    auto controller = found != previous.end()
                          ? *found
                          : std::make_shared<GameController>(gamepads[i], m_settings);
    if (controller->is_loaded()) {
      controller->refresh_metadata();
      m_available_controllers.push_back(std::move(controller));
    }
  }
  SDL_free(gamepads);

  for (const auto& [port, old_index] : previous_mapping) {
    for (int i = 0; i < static_cast<int>(m_available_controllers.size()); ++i) {
      if (m_available_controllers[i]->get_id() == previous.at(old_index)->get_id() &&
          try_claim_controller(i)) {
        m_controller_port_mapping[port] = i;
        break;
      }
    }
    if (!m_controller_port_mapping.contains(port)) {
      m_data.at(port)->clear();
    }
  }

  if (SDL_GetWindowFlags(m_window) & SDL_WINDOW_INPUT_FOCUS) {
    for (int i = 0; i < static_cast<int>(m_available_controllers.size()); ++i) {
      const auto& controller = m_available_controllers[i];
      const bool newly_connected =
          std::none_of(previous.begin(), previous.end(),
                       [&](const auto& old) { return old->get_id() == controller->get_id(); });
      if ((newly_connected || controller->get_id() == preferred_remote) &&
          controller->is_remote() && try_claim_controller(i)) {
        m_controller_port_mapping[0] = i;
        m_data.at(0)->clear();
        break;
      }
    }
  }

  if (!m_controller_port_mapping.contains(0)) {
    std::vector<int> candidates;
    for (int i = 0; i < static_cast<int>(m_available_controllers.size()); ++i) {
      candidates.push_back(i);
    }
    const auto preference = [&](const int index) {
      const auto guid = m_available_controllers[index]->get_guid();
      if (guid == m_settings->last_selected_controller_guid)
        return 0;
      const auto saved = m_settings->controller_port_mapping.find(guid);
      return saved != m_settings->controller_port_mapping.end() && saved->second == 0 ? 1 : 2;
    };
    std::stable_sort(candidates.begin(), candidates.end(), [&](const int left, const int right) {
      return preference(left) < preference(right);
    });
    for (const int index : candidates) {
      const bool mapped =
          std::any_of(m_controller_port_mapping.begin(), m_controller_port_mapping.end(),
                      [&](const auto& mapping) { return mapping.second == index; });
      if (!mapped && try_claim_controller(index)) {
        m_controller_port_mapping[0] = index;
        break;
      }
    }
  }
  release_unused_controller_claims();
  m_settings->_keyboard_temp_enabled = !m_controller_port_mapping.contains(0);
}

bool InputManager::try_claim_controller(const int controller_idx) {
  if (controller_idx < 0 || controller_idx >= (int)m_available_controllers.size()) {
    return false;
  }
  const auto key = m_available_controllers.at(controller_idx)->get_claim_key();
  if (m_controller_claims.contains(key)) {
    return true;
  }
  if (auto claim = DeviceClaim::acquire(key)) {
    m_controller_claims.emplace(key, std::move(claim));
    m_available_controllers.at(controller_idx)->clear_trigger_effect(
        dualsense_effects::TriggerEffectOption::BOTH);
    return true;
  }
  return false;
}

void InputManager::release_unused_controller_claims() {
  std::erase_if(m_controller_claims, [&](const auto& claim) {
    for (const auto& [port, index] : m_controller_port_mapping) {
      if (m_available_controllers.at(index)->get_claim_key() == claim.first) {
        return false;
      }
    }
    for (auto& controller : m_available_controllers) {
      if (controller->get_claim_key() == claim.first) {
        controller->clear_trigger_effect(dualsense_effects::TriggerEffectOption::BOTH);
      }
    }
    return true;
  });
}

void InputManager::enqueue_ignore_background_controller_events(const bool ignore) {
  const std::lock_guard<std::mutex> lock(m_event_queue_mtx);
  ee_event_queue.push({EEInputEventType::IGNORE_BACKGROUND_CONTROLLER_EVENTS, ignore, {}, {}, {}});
}

void InputManager::ignore_background_controller_events(const bool ignore) {
  m_ignore_background_controller_events = ignore;
  // TODO - ignoring return value (atleast log it)
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, ignore ? "0" : "1");
}

void InputManager::hide_cursor(const bool hide_cursor) {
  if (hide_cursor == m_mouse_currently_hidden) {
    return;
  }
  // NOTE - seems like an SDL bug, but the cursor will be visible / locked to the center of the
  // screen if you use the 'start menu' to exit the window / return to it (atleast in windowed mode)
  if (hide_cursor && !SDL_HideCursor()) {
    sdl_util::log_error("Unable to hide mouse cursor");
    return;
  }
  if (!hide_cursor && !SDL_ShowCursor()) {
    sdl_util::log_error("Unable to show mouse cursor");
    return;
  }
  m_mouse_currently_hidden = hide_cursor;
}

void InputManager::process_sdl_event(const SDL_Event& event) {
  if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_F1 &&
      !event.key.repeat && (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0 &&
      m_controller_notice_enabled && !m_text_editor.active() && !m_waiting_for_bind) {
    if (const auto controller = m_controller_layout_issue.load()) {
      steam::show_controller_settings(controller);
      return;
    }
  }
  if (m_text_editor.handle_event(event)) {
    return;
  }

  if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_T &&
      !event.key.repeat && m_text_input_shortcut_enabled && !m_waiting_for_bind &&
      (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0) {
    m_text_input_requested = true;
    return;
  }

  // Detect controller connections and disconnects
  if (sdl_util::is_any_event_type(
          event.type, {SDL_EVENT_GAMEPAD_ADDED, SDL_EVENT_GAMEPAD_REMOVED,
                       SDL_EVENT_GAMEPAD_REMAPPED, SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED})) {
    lg::info("Controller added or removed. refreshing controller device list");
    refresh_device_list();
  }

  if (m_data.find(m_keyboard_and_mouse_port) != m_data.end()) {
    m_keyboard.process_event(event, m_command_binds, m_data.at(m_keyboard_and_mouse_port),
                             m_waiting_for_bind);
    m_mouse.process_event(event, m_command_binds, m_data.at(m_keyboard_and_mouse_port),
                          m_waiting_for_bind);
  }

  // Send event to active controller device
  // This goes last so it takes precedence
  for (const auto& [port, controller_idx] : m_controller_port_mapping) {
    if (m_data.find(port) != m_data.end() && (int)m_available_controllers.size() > controller_idx) {
      m_available_controllers.at(controller_idx)
          ->process_event(event, m_command_binds, m_data.at(port), m_waiting_for_bind);
    }
  }

  // Clear the binding assignment if we got one
  if (m_waiting_for_bind && m_waiting_for_bind->assigned) {
    stop_waiting_for_bind();
    // NOTE - this is a total hack, but it's to prevent immediately re-assigning the "confirmation"
    // bind if you use a source that is polled
    // TODO: There's a correct way to do this....figure it out eventually
    m_skip_polling_for_n_frames = 60;
  }

  // Adjust mouse cursor visibility
  if (m_auto_hide_mouse) {
    if (event.type == SDL_EVENT_MOUSE_MOTION && !m_mouse.is_camera_being_controlled()) {
      hide_cursor(false);
    } else if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
      hide_cursor(true);
    }
  }
}

void InputManager::poll_keyboard_data() {
  const auto keyboard_state = SDL_GetKeyboardState(nullptr);
  m_text_input_keyboard_captured =
      m_text_editor.active() ||
      (m_text_input_keyboard_captured &&
       (keyboard_state[SDL_SCANCODE_RETURN] || keyboard_state[SDL_SCANCODE_KP_ENTER] ||
        keyboard_state[SDL_SCANCODE_ESCAPE] || keyboard_state[SDL_SCANCODE_T]));
  if (m_text_input_keyboard_captured) {
    clear_keyboard_actions();
    return;
  }
  if (is_keyboard_enabled() && m_skip_polling_for_n_frames <= 0 && !m_waiting_for_bind) {
    if (m_data.find(m_keyboard_and_mouse_port) != m_data.end()) {
      m_keyboard.poll_state(m_data.at(m_keyboard_and_mouse_port));
    }
  }
}

void InputManager::clear_keyboard_actions() {
  if (is_keyboard_enabled()) {
    if (m_data.find(m_keyboard_and_mouse_port) != m_data.end()) {
      m_keyboard.clear_actions(m_data.at(m_keyboard_and_mouse_port));
    }
  }
}

void InputManager::poll_mouse_data() {
  if (m_mouse_enabled && m_skip_polling_for_n_frames <= 0 && !m_waiting_for_bind) {
    if (m_data.find(m_keyboard_and_mouse_port) != m_data.end()) {
      m_mouse.poll_state(m_data.at(m_keyboard_and_mouse_port));
    }
  }
}

void InputManager::clear_mouse_actions() {
  if (m_mouse_enabled && !m_waiting_for_bind) {
    if (m_data.find(m_keyboard_and_mouse_port) != m_data.end()) {
      m_mouse.clear_actions(m_data.at(m_keyboard_and_mouse_port));
    }
  }
}

void InputManager::finish_polling() {
  m_skip_polling_for_n_frames--;
  if (m_skip_polling_for_n_frames < 0) {
    m_skip_polling_for_n_frames = 0;
  }
  steam::update_controller_input();
  bool changed = false;
  SDL_JoystickID preferred_remote = 0;
  for (auto& controller : m_available_controllers) {
    if (controller->refresh_metadata()) {
      changed = true;
      if (controller->is_remote()) {
        preferred_remote = controller->get_id();
      }
    }
  }
  if (changed) {
    refresh_device_list(preferred_remote);
  }
  update_controller_layout_notice();
}

bool InputManager::controller_layout_notice(const bool enabled) {
  m_controller_notice_enabled = enabled;
  if (!enabled)
    m_controller_layout_issue = 0;
  return enabled && m_controller_layout_issue.load() != 0;
}

void InputManager::update_controller_layout_notice() {
  uint64_t candidate = 0;
  if (m_controller_notice_enabled && !m_controller_port_mapping.contains(0)) {
    for (const auto handle : steam::controllers_without_gamepad_layout()) {
      const auto key = fmt::format("steam|{:016x}", handle);
      const bool available_in_sdl =
          std::any_of(m_available_controllers.begin(), m_available_controllers.end(),
                      [&](const auto& controller) { return controller->get_claim_key() == key; });
      if (!available_in_sdl && DeviceClaim::acquire(key)) {
        candidate = handle;
        break;
      }
    }
  }
  const auto now = SDL_GetTicks();
  if (candidate != m_controller_layout_candidate) {
    m_controller_layout_candidate = candidate;
    m_controller_layout_since = now;
  }
  m_controller_layout_issue = candidate && now - m_controller_layout_since >= 3000 ? candidate : 0;
}

void InputManager::process_ee_events() {
  const std::lock_guard<std::mutex> lock(m_event_queue_mtx);
  // Fully process any events from the EE
  while (!ee_event_queue.empty()) {
    const auto& evt = ee_event_queue.front();
    switch (evt.type) {
      case EEInputEventType::IGNORE_BACKGROUND_CONTROLLER_EVENTS:
        ignore_background_controller_events(std::get<bool>(evt.param1));
        break;
      case EEInputEventType::UPDATE_RUMBLE:
        controller_send_rumble(std::get<int>(evt.param1), std::get<u8>(evt.param2),
                               std::get<u8>(evt.param3));
        break;
      case EEInputEventType::SET_CONTROLLER_LED:
        set_controller_led(std::get<int>(evt.param1), std::get<u8>(evt.param2),
                           std::get<u8>(evt.param3), std::get<u8>(evt.param4));
        break;
      case EEInputEventType::UPDATE_MOUSE_OPTIONS:
        update_mouse_options(std::get<bool>(evt.param1), std::get<bool>(evt.param2),
                             std::get<bool>(evt.param3));
        break;
      case EEInputEventType::SET_AUTO_HIDE_MOUSE:
        set_auto_hide_mouse(std::get<bool>(evt.param1));
        break;
      case EEInputEventType::CONTROLLER_CLEAR_TRIGGER_EFFECT:
        controller_clear_trigger_effect(
            std::get<int>(evt.param1),
            std::get<dualsense_effects::TriggerEffectOption>(evt.param2));
        break;
      case EEInputEventType::CONTROLLER_SEND_TRIGGER_EFFECT_FEEDBACK:
        controller_send_trigger_effect_feedback(
            std::get<int>(evt.param1), std::get<dualsense_effects::TriggerEffectOption>(evt.param2),
            std::get<u8>(evt.param3), std::get<u8>(evt.param4));
        break;
      case EEInputEventType::CONTROLLER_SEND_TRIGGER_EFFECT_VIBRATE:
        controller_send_trigger_effect_vibrate(
            std::get<int>(evt.param1), std::get<dualsense_effects::TriggerEffectOption>(evt.param2),
            std::get<u8>(evt.param3), std::get<u8>(evt.param4), std::get<u8>(evt.param5));
        break;
      case EEInputEventType::CONTROLLER_SEND_TRIGGER_EFFECT_WEAPON:
        controller_send_trigger_effect_weapon(
            std::get<int>(evt.param1), std::get<dualsense_effects::TriggerEffectOption>(evt.param2),
            std::get<u8>(evt.param3), std::get<u8>(evt.param4), std::get<u8>(evt.param5));
        break;
      case EEInputEventType::CONTROLLER_SEND_TRIGGER_RUMBLE:
        controller_send_trigger_rumble(std::get<int>(evt.param1), std::get<u16>(evt.param2),
                                       std::get<u16>(evt.param3), std::get<u32>(evt.param4));
        break;
      case EEInputEventType::SET_TRIGGER_EFFECTS_ENABLED:
        set_trigger_effects_enabled(std::get<bool>(evt.param1));
        break;
      case EEInputEventType::START_TEXT_INPUT:
        if (!SDL_StartTextInput(m_window)) {
          sdl_util::log_error("Unable to start text input");
          m_text_editor.fail_active_session();
        }
        break;
      case EEInputEventType::STOP_TEXT_INPUT:
        if (!SDL_StopTextInput(m_window)) {
          sdl_util::log_error("Unable to stop text input");
        }
        break;
      case EEInputEventType::SET_CONTROLLER_FOR_PORT:
        for (int i = 0; i < static_cast<int>(m_available_controllers.size()); ++i) {
          if (m_available_controllers[i]->get_id() == std::get<u32>(evt.param4)) {
            assign_controller_to_port(i, std::get<int>(evt.param2));
            break;
          }
        }
        break;
    }
    ee_event_queue.pop();
  }
}

bool InputManager::text_input_shortcut(const bool enabled) {
  m_text_input_shortcut_enabled = enabled;
  const bool requested = m_text_input_requested.exchange(false);
  return enabled && requested && !m_text_editor.active();
}

uint32_t InputManager::begin_text_input(const std::string_view initial_text,
                                        TextInputEditor::Policy policy) {
  const uint32_t token = m_text_editor.begin(initial_text, std::move(policy));
  if (token != 0) {
    const std::lock_guard lock(m_event_queue_mtx);
    ee_event_queue.push({.type = EEInputEventType::START_TEXT_INPUT});
  }
  return token;
}

TextInputEditor::Snapshot InputManager::text_input_snapshot(const uint32_t token) const {
  return m_text_editor.snapshot(token);
}

bool InputManager::submit_text_input(const uint32_t token) {
  return m_text_editor.request_submit(token);
}

bool InputManager::cancel_text_input(const uint32_t token) {
  return m_text_editor.request_cancel(token);
}

bool InputManager::resolve_text_input(const uint32_t token, const bool accepted) {
  if (!m_text_editor.resolve_submission(token, accepted)) {
    return false;
  }
  if (accepted) {
    const std::lock_guard lock(m_event_queue_mtx);
    ee_event_queue.push({.type = EEInputEventType::STOP_TEXT_INPUT});
  }
  return true;
}

bool InputManager::close_text_input(const uint32_t token) {
  if (!m_text_editor.close(token)) {
    return false;
  }
  const std::lock_guard lock(m_event_queue_mtx);
  ee_event_queue.push({.type = EEInputEventType::STOP_TEXT_INPUT});
  return true;
}

void InputManager::register_command(const CommandBinding::Source source,
                                    const CommandBinding bind) {
  switch (source) {
    case CommandBinding::Source::CONTROLLER:
      if (m_command_binds.controller_binds.find(bind.host_key) ==
          m_command_binds.controller_binds.end()) {
        m_command_binds.controller_binds[bind.host_key] = {};
      }
      m_command_binds.controller_binds[bind.host_key].push_back(bind);
      break;
    case CommandBinding::Source::KEYBOARD:
      if (m_command_binds.keyboard_binds.find(bind.host_key) ==
          m_command_binds.keyboard_binds.end()) {
        m_command_binds.keyboard_binds[bind.host_key] = {};
      }
      m_command_binds.keyboard_binds[bind.host_key].push_back(bind);
      break;
    case CommandBinding::Source::MOUSE:
      if (m_command_binds.mouse_binds.find(bind.host_key) == m_command_binds.mouse_binds.end()) {
        m_command_binds.mouse_binds[bind.host_key] = {};
      }
      m_command_binds.mouse_binds[bind.host_key].push_back(bind);
      break;
  }
}

std::optional<std::shared_ptr<PadData>> InputManager::get_current_data(const int port) const {
  if (m_data.find(port) == m_data.end()) {
    return {};
  }
  return m_data.at(port);
}

std::string InputManager::get_controller_name(const int controller_id) {
  if ((size_t)controller_id >= m_available_controllers.size()) {
    return "";
  }
  return m_available_controllers.at(controller_id)->get_name();
}

std::string InputManager::get_current_bind(const int port,
                                           const InputDeviceType device_type,
                                           const bool buttons,
                                           const int input_idx,
                                           const bool analog_for_minimum) {
  std::vector<InputBindingInfo> binding_info;
  switch (device_type) {
    case InputDeviceType::CONTROLLER:
      if (m_controller_port_mapping.find(port) != m_controller_port_mapping.end() &&
          m_controller_port_mapping.at(port) < (int)m_available_controllers.size() &&
          m_settings->controller_binds.find(
              m_available_controllers.at(m_controller_port_mapping.at(port))->get_guid()) !=
              m_settings->controller_binds.end()) {
        binding_info =
            m_settings->controller_binds
                .at(m_available_controllers.at(m_controller_port_mapping.at(port))->get_guid())
                .lookup_button_binds((PadData::ButtonIndex)input_idx);
      }
      break;
    case InputDeviceType::KEYBOARD:
      if (!buttons) {
        binding_info = m_settings->keyboard_binds.lookup_analog_binds(
            (PadData::AnalogIndex)input_idx, analog_for_minimum);
      } else {
        binding_info =
            m_settings->keyboard_binds.lookup_button_binds((PadData::ButtonIndex)input_idx);
      }
      break;
    case InputDeviceType::MOUSE:
      binding_info = m_settings->mouse_binds.lookup_button_binds((PadData::ButtonIndex)input_idx);
      break;
  }
  if (binding_info.empty()) {
    return "";
  }
  return binding_info.front().host_name;
}

int InputManager::get_controller_index(const int port) {
  if (m_controller_port_mapping.find(port) == m_controller_port_mapping.end()) {
    return -1;
  }
  return m_controller_port_mapping.at(port);
}

void InputManager::set_controller_for_port(const int controller_id, const int port) {
  if (controller_id < 0 || controller_id >= static_cast<int>(m_available_controllers.size())) {
    return;
  }
  const std::lock_guard lock(m_event_queue_mtx);
  ee_event_queue.push({EEInputEventType::SET_CONTROLLER_FOR_PORT,
                       {},
                       port,
                       {},
                       m_available_controllers[controller_id]->get_id()});
}

void InputManager::assign_controller_to_port(const int controller_id, const int port) {
  if (port >= 0 && try_claim_controller(controller_id)) {
    // Reset inputs as this device won't be able to be read from again!
    clear_inputs();
    std::erase_if(m_controller_port_mapping,
                  [&](const auto& mapping) { return mapping.second == controller_id; });
    auto& controller = m_available_controllers.at(controller_id);
    m_controller_port_mapping[port] = controller_id;
    if (!m_data.contains(port)) {
      m_data[port] = std::make_shared<PadData>();
    }
    release_unused_controller_claims();
    m_settings->_keyboard_temp_enabled = !m_controller_port_mapping.contains(0);
    m_settings->controller_port_mapping[controller->get_guid()] = port;
    // NOTE - only tracking port 0 for now
    if (port == 0) {
      m_settings->last_selected_controller_guid = controller->get_guid();
    }
    m_settings->save_settings();
  }
}

bool InputManager::controller_has_led(const int port) {
  if (m_controller_port_mapping.find(port) == m_controller_port_mapping.end()) {
    return false;
  }
  const auto id = m_controller_port_mapping.at(port);
  if (id >= (int)m_available_controllers.size()) {
    return false;
  }
  return m_available_controllers.at(id)->has_led();
}

bool InputManager::controller_has_rumble(const int port) {
  if (m_controller_port_mapping.find(port) == m_controller_port_mapping.end()) {
    return false;
  }
  const auto id = m_controller_port_mapping.at(port);
  if (id >= (int)m_available_controllers.size()) {
    return false;
  }
  return m_available_controllers.at(id)->has_rumble();
}

bool InputManager::controller_has_pressure_sensitivity_support(const int port) {
  if (m_controller_port_mapping.find(port) == m_controller_port_mapping.end()) {
    return false;
  }
  const auto id = m_controller_port_mapping.at(port);
  if (id >= (int)m_available_controllers.size()) {
    return false;
  }
  return m_available_controllers.at(id)->has_pressure_sensitivity_support();
}

bool InputManager::controller_has_trigger_effect_support(const int port) {
  if (m_controller_port_mapping.find(port) == m_controller_port_mapping.end()) {
    return false;
  }
  const auto id = m_controller_port_mapping.at(port);
  if (id >= (int)m_available_controllers.size()) {
    return false;
  }
  return m_available_controllers.at(id)->has_trigger_effect_support();
}

int InputManager::controller_send_rumble(int port, u8 low_intensity, u8 high_intensity) {
  if (m_controller_port_mapping.find(port) == m_controller_port_mapping.end()) {
    return 0;
  }
  return m_available_controllers.at(m_controller_port_mapping.at(port))
      ->send_rumble(low_intensity, high_intensity);
}

void InputManager::controller_send_trigger_rumble(const int port,
                                                  const u16 left_rumble,
                                                  const u16 right_rumble,
                                                  const u32 duration_ms) {
  if (m_controller_port_mapping.find(port) == m_controller_port_mapping.end()) {
    return;
  }
  m_available_controllers.at(m_controller_port_mapping.at(port))
      ->send_trigger_rumble(left_rumble, right_rumble, duration_ms);
}

void InputManager::controller_clear_trigger_effect(const int port,
                                                   dualsense_effects::TriggerEffectOption option) {
  if (m_controller_port_mapping.find(port) == m_controller_port_mapping.end()) {
    return;
  }
  const auto id = m_controller_port_mapping.at(port);
  if (id >= (int)m_available_controllers.size()) {
    return;
  }
  m_available_controllers.at(id)->clear_trigger_effect(option);
}

void InputManager::controller_send_trigger_effect_feedback(
    const int port,
    dualsense_effects::TriggerEffectOption option,
    u8 position,
    u8 strength) {
  if (m_controller_port_mapping.find(port) == m_controller_port_mapping.end()) {
    return;
  }
  const auto id = m_controller_port_mapping.at(port);
  if (id >= (int)m_available_controllers.size()) {
    return;
  }
  m_available_controllers.at(id)->send_trigger_effect_feedback(option, position, strength);
}

void InputManager::controller_send_trigger_effect_vibrate(
    const int port,
    dualsense_effects::TriggerEffectOption option,
    u8 position,
    u8 amplitude,
    u8 frequency) {
  if (m_controller_port_mapping.find(port) == m_controller_port_mapping.end()) {
    return;
  }
  const auto id = m_controller_port_mapping.at(port);
  if (id >= (int)m_available_controllers.size()) {
    return;
  }
  m_available_controllers.at(id)->send_trigger_effect_vibrate(option, position, amplitude,
                                                              frequency);
}

void InputManager::controller_send_trigger_effect_weapon(
    const int port,
    dualsense_effects::TriggerEffectOption option,
    u8 start_position,
    u8 end_position,
    u8 strength) {
  if (m_controller_port_mapping.find(port) == m_controller_port_mapping.end()) {
    return;
  }
  const auto id = m_controller_port_mapping.at(port);
  if (id >= (int)m_available_controllers.size()) {
    return;
  }
  m_available_controllers.at(id)->send_trigger_effect_weapon(option, start_position, end_position,
                                                             strength);
}

bool InputManager::set_trigger_effects_enabled(bool enabled) {
  controller_clear_trigger_effect(0, dualsense_effects::TriggerEffectOption::BOTH);
  return m_settings->enable_trigger_effects = enabled;
};

void InputManager::enqueue_set_controller_led(const int port,
                                              const u8 red,
                                              const u8 green,
                                              const u8 blue) {
  const std::lock_guard<std::mutex> lock(m_event_queue_mtx);
  ee_event_queue.push({EEInputEventType::SET_CONTROLLER_LED, port, red, green, blue});
}

void InputManager::set_controller_led(const int port, const u8 red, const u8 green, const u8 blue) {
  if (m_controller_port_mapping.find(port) == m_controller_port_mapping.end()) {
    return;
  }
  const auto id = m_controller_port_mapping.at(port);
  if (id >= (int)m_available_controllers.size()) {
    return;
  }
  m_available_controllers.at(id)->set_led(red, green, blue);
}

void InputManager::enqueue_update_rumble(const int port,
                                         const u8 low_intensity,
                                         const u8 high_intensity) {
  const std::lock_guard<std::mutex> lock(m_event_queue_mtx);
  ee_event_queue.push({EEInputEventType::UPDATE_RUMBLE, port, low_intensity, high_intensity, {}});
}

void InputManager::enable_keyboard(const bool enabled) {
  m_settings->keyboard_enabled = enabled;
  if (!m_settings->keyboard_enabled) {
    // Reset inputs as this device won't be able to be read from again!
    clear_inputs();
  }
}

void InputManager::enqueue_update_mouse_options(const bool enabled,
                                                const bool control_camera,
                                                const bool control_movement) {
  const std::lock_guard<std::mutex> lock(m_event_queue_mtx);
  ee_event_queue.push(
      {EEInputEventType::UPDATE_MOUSE_OPTIONS, enabled, control_camera, control_movement, {}});
}

void InputManager::update_mouse_options(const bool enabled,
                                        const bool control_camera,
                                        const bool control_movement) {
  m_mouse_enabled = enabled;
  if (!m_mouse_enabled) {
    // Reset inputs as this device won't be able to be read from again!
    clear_inputs();
  }
  // Switch relevant flags over related to the mouse
  m_mouse.enable_camera_control(enabled && control_camera);
  m_mouse.enable_movement_control(enabled && control_movement);
}

void InputManager::set_wait_for_bind(const InputDeviceType device_type,
                                     const bool for_analog,
                                     const bool for_minimum_analog,
                                     const int input_idx) {
  m_waiting_for_bind = InputBindAssignmentMeta();
  m_waiting_for_bind->device_type = device_type;
  m_waiting_for_bind->pad_idx = input_idx;
  m_waiting_for_bind->for_analog = for_analog;
  m_waiting_for_bind->for_analog_minimum = for_minimum_analog;
  m_waiting_for_bind->seen_keyboard_confirm_up = false;
  m_waiting_for_bind->keyboard_confirmation_binds =
      m_settings->keyboard_binds.lookup_button_binds(PadData::CROSS);
  m_waiting_for_bind->seen_controller_confirm_neutral = false;
  if (m_controller_port_mapping.find(0) != m_controller_port_mapping.end() &&
      m_controller_port_mapping.at(0) < (int)m_available_controllers.size() &&
      m_settings->controller_binds.find(
          m_available_controllers.at(m_controller_port_mapping.at(0))->get_guid()) !=
          m_settings->controller_binds.end()) {
    m_waiting_for_bind->controller_confirmation_binds =
        m_settings->controller_binds
            .at(m_available_controllers.at(m_controller_port_mapping.at(0))->get_guid())
            .lookup_button_binds(PadData::CROSS);
  }
  if (g_game_version == GameVersion::Jak1) {
    auto keyboard_circle_binds = m_settings->keyboard_binds.lookup_button_binds(PadData::CIRCLE);
    m_waiting_for_bind->keyboard_confirmation_binds.insert(
        m_waiting_for_bind->keyboard_confirmation_binds.end(), keyboard_circle_binds.begin(),
        keyboard_circle_binds.end());
    if (m_controller_port_mapping.find(0) != m_controller_port_mapping.end() &&
        m_controller_port_mapping.at(0) < (int)m_available_controllers.size() &&
        m_settings->controller_binds.find(
            m_available_controllers.at(m_controller_port_mapping.at(0))->get_guid()) !=
            m_settings->controller_binds.end()) {
      auto controller_circle_binds =
          m_settings->controller_binds
              .at(m_available_controllers.at(m_controller_port_mapping.at(0))->get_guid())
              .lookup_button_binds(PadData::CIRCLE);
      m_waiting_for_bind->controller_confirmation_binds.insert(
          m_waiting_for_bind->controller_confirmation_binds.end(), controller_circle_binds.begin(),
          controller_circle_binds.end());
    }
  }
}

void InputManager::set_camera_sens(const float xsens, const float ysens) {
  m_mouse.set_camera_sens(xsens, ysens);
}

void InputManager::reset_input_bindings_to_defaults(const int port,
                                                    const InputDeviceType device_type) {
  switch (device_type) {
    case InputDeviceType::CONTROLLER:
      if (m_controller_port_mapping.find(port) != m_controller_port_mapping.end() &&
          m_controller_port_mapping.at(port) < (int)m_available_controllers.size() &&
          m_settings->controller_binds.find(
              m_available_controllers.at(m_controller_port_mapping.at(port))->get_guid()) !=
              m_settings->controller_binds.end()) {
        m_settings->controller_binds
            .at(m_available_controllers.at(m_controller_port_mapping.at(port))->get_guid())
            .set_bindings(DEFAULT_CONTROLLER_BINDS);
      }
      break;
    case InputDeviceType::KEYBOARD:
      m_settings->keyboard_binds.set_bindings(DEFAULT_KEYBOARD_BINDS);
      break;
    case InputDeviceType::MOUSE:
      m_settings->mouse_binds.set_bindings(DEFAULT_MOUSE_BINDS);
      break;
  }
}

void InputManager::enqueue_set_auto_hide_mouse(const bool auto_hide_mouse) {
  const std::lock_guard<std::mutex> lock(m_event_queue_mtx);
  ee_event_queue.push({EEInputEventType::SET_AUTO_HIDE_MOUSE, auto_hide_mouse, {}, {}, {}});
}

void InputManager::enqueue_controller_clear_trigger_effect(
    const int port,
    const dualsense_effects::TriggerEffectOption option) {
  const std::lock_guard<std::mutex> lock(m_event_queue_mtx);
  ee_event_queue.push({.type = EEInputEventType::CONTROLLER_CLEAR_TRIGGER_EFFECT,
                       .param1 = port,
                       .param2 = option});
}

void InputManager::enqueue_controller_send_trigger_effect_feedback(
    const int port,
    const dualsense_effects::TriggerEffectOption option,
    const u8 position,
    const u8 strength) {
  const std::lock_guard<std::mutex> lock(m_event_queue_mtx);
  ee_event_queue.push({.type = EEInputEventType::CONTROLLER_SEND_TRIGGER_EFFECT_FEEDBACK,
                       .param1 = port,
                       .param2 = option,
                       .param3 = position,
                       .param4 = strength});
}

void InputManager::enqueue_controller_send_trigger_effect_vibrate(
    const int port,
    const dualsense_effects::TriggerEffectOption option,
    const u8 position,
    const u8 amplitude,
    const u8 frequency) {
  const std::lock_guard<std::mutex> lock(m_event_queue_mtx);
  ee_event_queue.push({.type = EEInputEventType::CONTROLLER_SEND_TRIGGER_EFFECT_VIBRATE,
                       .param1 = port,
                       .param2 = option,
                       .param3 = position,
                       .param4 = amplitude,
                       .param5 = frequency});
}

void InputManager::enqueue_controller_send_trigger_effect_weapon(
    const int port,
    const dualsense_effects::TriggerEffectOption option,
    const u8 start_position,
    const u8 end_position,
    const u8 strength) {
  const std::lock_guard<std::mutex> lock(m_event_queue_mtx);
  ee_event_queue.push({.type = EEInputEventType::CONTROLLER_SEND_TRIGGER_EFFECT_WEAPON,
                       .param1 = port,
                       .param2 = option,
                       .param3 = start_position,
                       .param4 = end_position,
                       .param5 = strength});
}

void InputManager::enqueue_controller_send_trigger_rumble(const int port,
                                                          const u16 left_rumble,
                                                          const u16 right_rumble,
                                                          const u32 duration_ms) {
  const std::lock_guard<std::mutex> lock(m_event_queue_mtx);
  ee_event_queue.push({.type = EEInputEventType::CONTROLLER_SEND_TRIGGER_RUMBLE,
                       .param1 = port,
                       .param2 = left_rumble,
                       .param3 = right_rumble,
                       .param4 = duration_ms});
}

void InputManager::enqueue_set_trigger_effects_enabled(const bool enabled) {
  const std::lock_guard<std::mutex> lock(m_event_queue_mtx);
  ee_event_queue.push({.type = EEInputEventType::SET_TRIGGER_EFFECTS_ENABLED, .param1 = enabled});
}

void InputManager::set_auto_hide_mouse(const bool auto_hide_mouse) {
  m_auto_hide_mouse = auto_hide_mouse;
  if (!auto_hide_mouse) {
    hide_cursor(false);
  }
}

void InputManager::clear_inputs() {
  // Reset inputs as this device won't be able to be read from again!
  for (auto& [port, data] : m_data) {
    data->clear();
  }
}
