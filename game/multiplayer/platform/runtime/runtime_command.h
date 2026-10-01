#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "game/multiplayer/platform/session/session_controller.h"
#ifdef ENABLE_NETWORK_SIMULATION
#include "game/multiplayer/platform/transport/network_simulation.h"
#endif

namespace multiplayer::platform {

enum class CommandDomain : uint8_t { CONNECTION, SESSION, CONTROL };
enum class CommandOutcome : uint8_t { IDLE, QUEUED, APPLIED, REJECTED };
enum class CommandError : uint8_t {
  NONE,
  RUNTIME_INACTIVE,
  QUEUE_FULL,
  INVALID_REQUEST,
  INVALID_STATE,
  NOT_ALLOWED,
  UNAVAILABLE,
  START_FAILED,
  INTERNAL_ERROR,
};

enum class ConnectionAction : uint8_t {
  NONE,
  HOST,
  CONNECT,
  START_DISCOVERY,
  STOP_DISCOVERY,
  CONNECT_DISCOVERED,
  RECONNECT,
  DISCONNECT,
};
enum class SessionAction : uint8_t {
  NONE,
  SET_PROFILE,
  SET_CHARACTER,
  SET_READY,
  START_COUNTDOWN,
  CANCEL_COUNTDOWN,
  START_GAME,
  ENTER_LOBBY,
  ENTER_GAME,
  REQUEST_BOOTSTRAP,
};
enum class ControlAction : uint8_t { NONE, CONFIGURE_IDENTITY, NETWORK_SIMULATION };

struct CommandResult {
  uint32_t revision = 0;
  uint8_t action = 0;
  CommandOutcome outcome = CommandOutcome::IDLE;
  CommandError error = CommandError::NONE;
};

struct HostSessionRequest {
  ControllerHostConfig config;
  uint16_t discovery_port = 0;
  bool internet = false;
  bool automatic_port_mapping = false;
};

enum class DiscoveryKind : uint8_t { NONE, LAN, DIRECTED };

struct DiscoveryRequest {
  uint16_t discovery_port = 0;
  uint16_t expected_game_port = 0;
  std::string directed_address;
  ParticipantProfile profile;
};

class ConnectionExecutionPort {
 public:
  virtual ~ConnectionExecutionPort() = default;
  virtual CommandError host(HostSessionRequest) = 0;
  virtual CommandError connect(ControllerClientConfig) = 0;
  virtual CommandError start_discovery(DiscoveryRequest) = 0;
  virtual CommandError stop_discovery() = 0;
  virtual CommandError connect_discovered() = 0;
  virtual CommandError reconnect() = 0;
  virtual CommandError disconnect(int reason) = 0;
};

class SessionExecutionPort {
 public:
  virtual ~SessionExecutionPort() = default;
  virtual CommandError set_profile(ParticipantProfile) = 0;
  virtual CommandError set_character(PlayerCharacter) = 0;
  virtual CommandError set_ready(bool) = 0;
  virtual CommandError start_countdown(uint32_t) = 0;
  virtual CommandError cancel_countdown() = 0;
  virtual CommandError start_game() = 0;
  virtual CommandError enter_lobby() = 0;
  virtual CommandError enter_game() = 0;
  virtual CommandError request_bootstrap() = 0;
};

class RuntimeControlExecutionPort {
 public:
  virtual ~RuntimeControlExecutionPort() = default;
  virtual CommandError configure_compatibility_identity(std::string) = 0;
#ifdef ENABLE_NETWORK_SIMULATION
  virtual CommandError set_network_simulation(NetworkSimulationSettings) = 0;
#endif
};

struct RuntimeExecutionPorts {
  ConnectionExecutionPort& connection;
  SessionExecutionPort& session;
  RuntimeControlExecutionPort& control;
};

class RuntimeCommand {
 public:
  virtual ~RuntimeCommand() = default;
  virtual CommandDomain domain() const = 0;
  virtual uint8_t action() const = 0;
  virtual CommandError execute(RuntimeExecutionPorts&) = 0;
};

class ConnectionCommand : public RuntimeCommand {
 public:
  CommandDomain domain() const final { return CommandDomain::CONNECTION; }
  CommandError execute(RuntimeExecutionPorts& ports) final { return execute(ports.connection); }

 private:
  virtual CommandError execute(ConnectionExecutionPort&) = 0;
};

class SessionCommand : public RuntimeCommand {
 public:
  CommandDomain domain() const final { return CommandDomain::SESSION; }
  CommandError execute(RuntimeExecutionPorts& ports) final { return execute(ports.session); }

 private:
  virtual CommandError execute(SessionExecutionPort&) = 0;
};

class RuntimeControlCommand : public RuntimeCommand {
 public:
  CommandDomain domain() const final { return CommandDomain::CONTROL; }
  CommandError execute(RuntimeExecutionPorts& ports) final { return execute(ports.control); }

 private:
  virtual CommandError execute(RuntimeControlExecutionPort&) = 0;
};

#define MP_VALUE_COMMAND(name, family, action_value, value_type, port_type, port_method) \
  class name final : public family {                                                     \
   public:                                                                               \
    explicit name(value_type value) : value_(std::move(value)) {}                        \
    uint8_t action() const final {                                                       \
      return static_cast<uint8_t>(action_value);                                         \
    }                                                                                    \
                                                                                         \
   private:                                                                              \
    CommandError execute(port_type& port) final {                                        \
      return port.port_method(std::move(value_));                                        \
    }                                                                                    \
    value_type value_;                                                                   \
  }

#define MP_SIMPLE_COMMAND(name, family, action_value, port_type, expression) \
  class name final : public family {                                         \
   public:                                                                   \
    uint8_t action() const final {                                           \
      return static_cast<uint8_t>(action_value);                             \
    }                                                                        \
                                                                             \
   private:                                                                  \
    CommandError execute(port_type& port) final {                            \
      return expression;                                                     \
    }                                                                        \
  }

MP_VALUE_COMMAND(HostSessionCommand,
                 ConnectionCommand,
                 ConnectionAction::HOST,
                 HostSessionRequest,
                 ConnectionExecutionPort,
                 host);
MP_VALUE_COMMAND(ConnectSessionCommand,
                 ConnectionCommand,
                 ConnectionAction::CONNECT,
                 ControllerClientConfig,
                 ConnectionExecutionPort,
                 connect);
MP_VALUE_COMMAND(StartDiscoveryCommand,
                 ConnectionCommand,
                 ConnectionAction::START_DISCOVERY,
                 DiscoveryRequest,
                 ConnectionExecutionPort,
                 start_discovery);
MP_SIMPLE_COMMAND(StopDiscoveryCommand,
                  ConnectionCommand,
                  ConnectionAction::STOP_DISCOVERY,
                  ConnectionExecutionPort,
                  port.stop_discovery());
MP_SIMPLE_COMMAND(ConnectDiscoveredCommand,
                  ConnectionCommand,
                  ConnectionAction::CONNECT_DISCOVERED,
                  ConnectionExecutionPort,
                  port.connect_discovered());
MP_SIMPLE_COMMAND(ReconnectCommand,
                  ConnectionCommand,
                  ConnectionAction::RECONNECT,
                  ConnectionExecutionPort,
                  port.reconnect());
class DisconnectSessionCommand final : public ConnectionCommand {
 public:
  explicit DisconnectSessionCommand(int reason = 0) : reason_(reason) {}
  uint8_t action() const final { return static_cast<uint8_t>(ConnectionAction::DISCONNECT); }

 private:
  CommandError execute(ConnectionExecutionPort& port) final { return port.disconnect(reason_); }
  int reason_;
};

MP_VALUE_COMMAND(SetProfileCommand,
                 SessionCommand,
                 SessionAction::SET_PROFILE,
                 ParticipantProfile,
                 SessionExecutionPort,
                 set_profile);
MP_VALUE_COMMAND(SetCharacterCommand,
                 SessionCommand,
                 SessionAction::SET_CHARACTER,
                 PlayerCharacter,
                 SessionExecutionPort,
                 set_character);
MP_VALUE_COMMAND(SetReadyCommand,
                 SessionCommand,
                 SessionAction::SET_READY,
                 bool,
                 SessionExecutionPort,
                 set_ready);
MP_VALUE_COMMAND(StartCountdownCommand,
                 SessionCommand,
                 SessionAction::START_COUNTDOWN,
                 uint32_t,
                 SessionExecutionPort,
                 start_countdown);
MP_SIMPLE_COMMAND(CancelCountdownCommand,
                  SessionCommand,
                  SessionAction::CANCEL_COUNTDOWN,
                  SessionExecutionPort,
                  port.cancel_countdown());
MP_SIMPLE_COMMAND(StartGameCommand,
                  SessionCommand,
                  SessionAction::START_GAME,
                  SessionExecutionPort,
                  port.start_game());
MP_SIMPLE_COMMAND(EnterLobbyCommand,
                  SessionCommand,
                  SessionAction::ENTER_LOBBY,
                  SessionExecutionPort,
                  port.enter_lobby());
MP_SIMPLE_COMMAND(EnterGameCommand,
                  SessionCommand,
                  SessionAction::ENTER_GAME,
                  SessionExecutionPort,
                  port.enter_game());
MP_SIMPLE_COMMAND(RequestBootstrapCommand,
                  SessionCommand,
                  SessionAction::REQUEST_BOOTSTRAP,
                  SessionExecutionPort,
                  port.request_bootstrap());

MP_VALUE_COMMAND(SetCompatibilityIdentityCommand,
                 RuntimeControlCommand,
                 ControlAction::CONFIGURE_IDENTITY,
                 std::string,
                 RuntimeControlExecutionPort,
                 configure_compatibility_identity);

#ifdef ENABLE_NETWORK_SIMULATION
MP_VALUE_COMMAND(SetNetworkSimulationCommand,
                 RuntimeControlCommand,
                 ControlAction::NETWORK_SIMULATION,
                 NetworkSimulationSettings,
                 RuntimeControlExecutionPort,
                 set_network_simulation);
#endif

#undef MP_SIMPLE_COMMAND
#undef MP_VALUE_COMMAND

}  // namespace multiplayer::platform
