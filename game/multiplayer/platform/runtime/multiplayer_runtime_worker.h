#pragma once

#include <optional>

#include "game/multiplayer/platform/discovery/discovery_service.h"
#include "game/multiplayer/platform/nat/port_mapping_coordinator.h"
#include "game/multiplayer/platform/runtime/multiplayer_runtime.h"
#include "game/multiplayer/platform/session/reconnect_policy.h"

namespace multiplayer::platform {

class RuntimeWorker final : public ConnectionExecutionPort,
                            public SessionExecutionPort,
                            public RuntimeControlExecutionPort {
 public:
  RuntimeWorker(MultiplayerRuntime& runtime, GameAdapter& adapter);
  void run(std::stop_token stop_token);

 private:
  struct CanonicalEndpoint {
    std::string address;
    uint16_t port = 0;
    std::string room_code;
  };
  struct ClientConnectionState {
    PlayerProfile profile;
    std::optional<CanonicalEndpoint> endpoint;
  };
  CommandError host(HostSessionRequest request) override;
  CommandError connect(ControllerClientConfig config) override;
  CommandError start_discovery(DiscoveryRequest request) override;
  CommandError stop_discovery() override;
  CommandError connect_discovered() override;
  CommandError reconnect() override;
  CommandError disconnect(int reason) override;
  CommandError set_profile(PlayerProfile profile) override;
  CommandError set_character(PlayerCharacter character) override;
  CommandError set_ready(bool ready) override;
  CommandError start_countdown(uint32_t seconds) override;
  CommandError cancel_countdown() override;
  CommandError start_game() override;
  CommandError enter_lobby() override;
  CommandError enter_game() override;
  CommandError request_bootstrap() override;
  CommandError configure_compatibility_identity(std::string identity) override;
#ifdef ENABLE_NETWORK_SIMULATION
  CommandError set_network_simulation(NetworkSimulationSettings settings) override;
#endif

  static uint64_t steady_time_ms();
  CommandError connect_canonical(CanonicalEndpoint endpoint, bool reset_retry = true);
  CommandError session_action(void (SessionController::*action)());
  void stop_connection_services();
  void reset_host_state();
  void reset_for_new_connection();
  void process_commands();
  void pump_reconnect(uint64_t now_ms);
  void persist_profile(const SessionSnapshot& session);
  void update_discovery();
  void update_host_advertisement(const SessionSnapshot& session);
  void publish(const SessionSnapshot& session);

  MultiplayerRuntime& runtime_;
  GameAdapter& adapter_;
  SessionController controller_;
  DiscoveryScanner discovery_scanner_;
  DiscoveryResponder discovery_responder_;
  PortMappingCoordinator port_mapping_;
  std::optional<DiscoveryResult> discovery_result_;
  ReconnectState reconnect_;
  ClientConnectionState client_;
  std::optional<StoredPlayerProfile> last_saved_profile_;
  DiscoveryKind discovery_kind_ = DiscoveryKind::NONE;
  HostSnapshot host_;
  std::string compatibility_identity_;
  bool internet_host_ = false;
  bool automatic_port_mapping_ = false;
};

}  // namespace multiplayer::platform
