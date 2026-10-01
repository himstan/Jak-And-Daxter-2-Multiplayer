#include <string>

#include "game/multiplayer/platform/runtime/runtime_command.h"
#include "gtest/gtest.h"

namespace {
using namespace multiplayer::platform;

class ConnectionPort final : public ConnectionExecutionPort {
 public:
  CommandError host(HostSessionRequest) override { return called(ConnectionAction::HOST); }
  CommandError connect(ControllerClientConfig) override {
    return called(ConnectionAction::CONNECT);
  }
  CommandError start_discovery(DiscoveryRequest) override {
    return called(ConnectionAction::START_DISCOVERY);
  }
  CommandError stop_discovery() override { return called(ConnectionAction::STOP_DISCOVERY); }
  CommandError connect_discovered() override {
    return called(ConnectionAction::CONNECT_DISCOVERED);
  }
  CommandError reconnect() override { return called(ConnectionAction::RECONNECT); }
  CommandError disconnect(int) override { return called(ConnectionAction::DISCONNECT); }

  CommandError called(ConnectionAction value) {
    action = value;
    ++calls;
    return CommandError::UNAVAILABLE;
  }
  ConnectionAction action = ConnectionAction::NONE;
  int calls = 0;
};

class SessionPort final : public SessionExecutionPort {
 public:
  CommandError set_profile(PlayerProfile) override { return called(SessionAction::SET_PROFILE); }
  CommandError set_character(PlayerCharacter) override {
    return called(SessionAction::SET_CHARACTER);
  }
  CommandError set_ready(bool) override { return called(SessionAction::SET_READY); }
  CommandError start_countdown(uint32_t) override { return called(SessionAction::START_COUNTDOWN); }
  CommandError cancel_countdown() override { return called(SessionAction::CANCEL_COUNTDOWN); }
  CommandError start_game() override { return called(SessionAction::START_GAME); }
  CommandError enter_lobby() override { return called(SessionAction::ENTER_LOBBY); }
  CommandError enter_game() override { return called(SessionAction::ENTER_GAME); }
  CommandError request_bootstrap() override { return called(SessionAction::REQUEST_BOOTSTRAP); }

  CommandError called(SessionAction value) {
    action = value;
    ++calls;
    return CommandError::INVALID_STATE;
  }
  SessionAction action = SessionAction::NONE;
  int calls = 0;
};

class ControlPort final : public RuntimeControlExecutionPort {
 public:
  CommandError configure_compatibility_identity(std::string) override {
    ++calls;
    return CommandError::NOT_ALLOWED;
  }
#ifdef ENABLE_NETWORK_SIMULATION
  CommandError set_network_simulation(NetworkSimulationSettings) override {
    ++calls;
    return CommandError::NOT_ALLOWED;
  }
#endif
  int calls = 0;
};

CommandError execute(RuntimeCommand& command, RuntimeExecutionPorts& ports) {
  return command.execute(ports);
}

TEST(RuntimeCommands, FamiliesDispatchOnlyToTheirNarrowPort) {
  ConnectionPort connection;
  SessionPort session;
  ControlPort control;
  RuntimeExecutionPorts ports{connection, session, control};

  ConnectSessionCommand connect({});
  EXPECT_EQ(connect.domain(), CommandDomain::CONNECTION);
  EXPECT_EQ(execute(connect, ports), CommandError::UNAVAILABLE);
  EXPECT_EQ(connection.action, ConnectionAction::CONNECT);
  EXPECT_EQ(connection.calls, 1);
  EXPECT_EQ(session.calls, 0);
  EXPECT_EQ(control.calls, 0);

  SetReadyCommand ready(true);
  EXPECT_EQ(ready.domain(), CommandDomain::SESSION);
  EXPECT_EQ(execute(ready, ports), CommandError::INVALID_STATE);
  EXPECT_EQ(session.action, SessionAction::SET_READY);
  EXPECT_EQ(connection.calls, 1);
  EXPECT_EQ(session.calls, 1);
  EXPECT_EQ(control.calls, 0);

  SetCompatibilityIdentityCommand identity("test-build");
  EXPECT_EQ(identity.domain(), CommandDomain::CONTROL);
  EXPECT_EQ(execute(identity, ports), CommandError::NOT_ALLOWED);
  EXPECT_EQ(connection.calls, 1);
  EXPECT_EQ(session.calls, 1);
  EXPECT_EQ(control.calls, 1);
}

TEST(RuntimeCommands, EveryConnectionActionHasAnExplicitCommand) {
  ConnectionPort connection;
  SessionPort session;
  ControlPort control;
  RuntimeExecutionPorts ports{connection, session, control};
  HostSessionCommand host({});
  ConnectSessionCommand connect({});
  StartDiscoveryCommand start_discovery({});
  StopDiscoveryCommand stop_discovery;
  ConnectDiscoveredCommand connect_discovered;
  ReconnectCommand reconnect;
  DisconnectSessionCommand disconnect;
  execute(host, ports);
  execute(connect, ports);
  execute(start_discovery, ports);
  execute(stop_discovery, ports);
  execute(connect_discovered, ports);
  execute(reconnect, ports);
  execute(disconnect, ports);
  EXPECT_EQ(connection.calls, 7);
  EXPECT_EQ(connection.action, ConnectionAction::DISCONNECT);
}

TEST(RuntimeCommands, EverySessionActionHasAnExplicitCommand) {
  ConnectionPort connection;
  SessionPort session;
  ControlPort control;
  RuntimeExecutionPorts ports{connection, session, control};
  SetProfileCommand set_profile({});
  SetCharacterCommand set_character(PlayerCharacter::JAK);
  SetReadyCommand set_ready(true);
  StartCountdownCommand start_countdown(3);
  CancelCountdownCommand cancel_countdown;
  StartGameCommand start_game;
  EnterLobbyCommand enter_lobby;
  EnterGameCommand enter_game;
  RequestBootstrapCommand request_bootstrap;
  execute(set_profile, ports);
  execute(set_character, ports);
  execute(set_ready, ports);
  execute(start_countdown, ports);
  execute(cancel_countdown, ports);
  execute(start_game, ports);
  execute(enter_lobby, ports);
  execute(enter_game, ports);
  execute(request_bootstrap, ports);
  EXPECT_EQ(session.calls, 9);
  EXPECT_EQ(session.action, SessionAction::REQUEST_BOOTSTRAP);
}
}  // namespace
