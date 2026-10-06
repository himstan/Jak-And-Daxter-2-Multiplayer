#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "game/multiplayer/platform/protocol/message_frame.h"
#include "game/multiplayer/platform/session/cadence_scheduler.h"
#include "game/multiplayer/platform/session/frame_submission.h"
#include "game/multiplayer/platform/session/game_adapter.h"
#include "game/multiplayer/platform/session/player_registry.h"
#include "game/multiplayer/platform/session/session_snapshot.h"
#include "game/multiplayer/platform/transport/session_platform.h"

namespace multiplayer::platform {

struct ControllerHostConfig {
  uint16_t port = 0;
  uint8_t player_limit = 0;
  std::string room_code;
  PlayerProfile local_profile;
};

struct ControllerClientConfig {
  std::string endpoint;
  uint16_t port = 0;
  std::string room_code;
  PlayerProfile local_profile;
};

class SessionController final : public GameSessionEndpoint {
 public:
  explicit SessionController(GameAdapter& adapter);
  ~SessionController() override;

  bool host(const ControllerHostConfig& config);
  bool connect(const ControllerClientConfig& config);
  void disconnect(int reason = 0);
  void pump(uint64_t now_ms);

  bool set_local_profile(PlayerProfile profile);
  const PlayerProfile& local_profile() const { return local_profile_; }
  bool set_character(PlayerCharacter character);
  bool set_ready(bool ready);
  bool start_countdown(uint32_t seconds);
  bool cancel_countdown();
  bool start_game();
  void enter_game();
  void enter_lobby();

  std::optional<uint32_t> send_gameplay(uint8_t message_id,
                                        const Audience& audience,
                                        std::span<const uint8_t> payload) override;
  bool cadence_due(uint8_t message_id, uint64_t now_ms, bool dirty = true) override;
  void request_bootstrap() override;
  const SessionSnapshot& snapshot() const override { return snapshot_; }
  uint32_t estimated_rtt_ms(PlayerId player_id) const override;
  NetworkPressure network_pressure() const override { return pressure_.pressure(); }
  bool severe_pressure_sustained(uint64_t now_ms) const override;

  uint16_t local_port() const { return transport_.local_port(); }
  std::string room_code() const { return room_code_; }
  std::string invite_for_address(std::string_view address) const;
  SessionPlatform& transport() { return transport_; }

 private:
  bool valid_character(PlayerCharacter character) const;
  bool valid_message_policies(std::string_view action) const;
  bool valid_game_identity(std::string_view action) const;
  bool character_change_allowed(const PlayerProfile& profile) const;
  bool validate_profile(PlayerProfile& profile, PlayerId player_id) const;
  void reset_adapter_session();
  void clear_countdown();
  uint32_t next_bootstrap_generation();
  void handle_event(TransportEvent event, uint64_t now_ms);
  void handle_connected(const TransportEvent& event, uint64_t now_ms);
  void handle_pending_message(const TransportEvent& event);
  void handle_admitted_message(const TransportEvent& event);
  void handle_control(ConnectionId connection,
                      PlayerId player_id,
                      PlayerId origin_id,
                      Delivery delivery,
                      std::span<const uint8_t> payload,
                      uint64_t now_ms);
  void handle_host_control(ConnectionId connection, PlayerId player_id, ControlMessage message);
  void handle_client_control(ConnectionId connection, ControlMessage message, uint64_t now_ms);
  void handle_gameplay(ConnectionId connection,
                       PlayerId player_id,
                       PlayerId origin,
                       Delivery delivery,
                       TransportLane lane,
                       std::span<const uint8_t> payload);
  void handle_bootstrap(ConnectionId connection,
                        PlayerId origin,
                        Delivery delivery,
                        std::span<const uint8_t> payload);
  void host_departure(ConnectionId connection, int reason);
  void reject(ConnectionId connection, RejectionReason reason, std::string_view required = {});
  FrameSendResult send_frame(FrameKind kind,
                             const Audience& audience,
                             PlayerId origin,
                             std::span<const uint8_t> payload,
                             TransportLane lane);
  void send_control(const ControlMessage& message, const Audience& audience);
  void broadcast_profile(const PlayerProfile& profile);
  void publish_roster(ConnectionId connection);
  void update_snapshot(std::vector<ConnectionSnapshot> connections);
  void send_pending_bootstraps(uint64_t now_ms);

  struct PendingGate {
    uint64_t deadline_ms = 0;
    std::string remote_address;
  };

  struct RejectionThrottle {
    uint64_t window_started_ms = 0;
    uint64_t blocked_until_ms = 0;
    uint8_t count = 0;
  };

  GameAdapter& adapter_;
  SessionPlatform transport_;
  PlayerRegistry registry_;
  CadenceScheduler cadence_;
  NetworkPressureTracker pressure_;
  SessionSnapshot snapshot_;
  PlayerProfile local_profile_;
  std::vector<PlayerProfile> profiles_;
  std::array<uint32_t, 256> outbound_sequences_ = {};
  std::unordered_map<ConnectionId, PendingGate> pending_gates_;
  std::unordered_map<ConnectionId, uint64_t> pending_rejection_closes_;
  std::unordered_map<std::string, RejectionThrottle> rejection_throttles_;
  std::string room_code_;
  uint32_t last_applied_bootstrap_ = 0;
  uint32_t host_bootstrap_generation_ = 0;
  uint64_t last_pump_ms_ = 0;
  uint64_t last_ping_publish_ms_ = 0;
  bool adapter_session_active_ = false;
  bool bootstrap_send_deferred_ = false;
  bool gameplay_send_observed_ = false;
  bool gameplay_receive_observed_ = false;
  bool gameplay_rejection_observed_ = false;
};

}  // namespace multiplayer::platform
