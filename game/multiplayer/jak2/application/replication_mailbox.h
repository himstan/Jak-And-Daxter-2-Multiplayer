#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "game/multiplayer/jak2/core/multiplayer_types.h"
#include "game/multiplayer/platform/core/mailbox.h"

namespace multiplayer::jak2::application {

inline constexpr size_t kReplicationEventCapacity = 256;

struct LocalReplicationFrame {
  uint32_t sample_time_ms = 0;
  core::PlayerId local_player_id = core::kInvalidPlayerId;
  core::PlayerId host_player_id = core::kInvalidPlayerId;
  std::array<core::PlayerIdentity, core::kMaxPlayers> identities = {};
  std::array<core::PlayerState, core::kMaxPlayers> players = {};
  std::optional<core::PlayerVehicleState> player_vehicle;
  std::optional<core::TurretState> turret;
  core::WorldState world = {};
  core::BootstrapState bootstrap = {};
  core::EnemySnapshot enemies = {};
  core::TrafficSnapshot pedestrians = {};
  core::TrafficSnapshot vehicles = {};
  std::array<core::BossState, 2> bosses = {};
  core::AirlockSnapshot airlocks = {};
  core::TrafficAuthority traffic_authority = {};
  core::PlayerId selected_traffic_authority = core::kInvalidPlayerId;
};

struct PresentationTarget {
  uint32_t entity_id = 0;
  std::array<float, 3> position = {};
  std::array<float, 4> quaternion = {0.0f, 0.0f, 0.0f, 1.0f};
  std::array<float, 3> velocity = {};
  uint32_t generation = 0;
  bool valid = false;
};

struct RemoteReplicationFrame {
  std::array<uint32_t, core::kMaxPlayers> participant_lifecycles = {};
  std::array<core::PlayerIdentity, core::kMaxPlayers> identities = {};
  std::array<core::PlayerState, core::kMaxPlayers> players = {};
  std::array<core::PlayerVehicleState, core::kMaxPlayers> player_vehicles = {};
  std::array<core::TurretState, core::kMaxPlayers> turrets = {};
  std::array<PresentationTarget, core::kMaxPlayers> player_targets = {};
  std::array<PresentationTarget, core::kMaxPlayers> player_vehicle_targets = {};
  core::WorldState world = {};
  core::BootstrapState bootstrap = {};
  core::EnemySnapshot enemies = {};
  core::TrafficSnapshot selected_traffic = {};
  std::vector<PresentationTarget> enemy_targets;
  std::vector<PresentationTarget> pedestrian_targets;
  std::vector<PresentationTarget> ambient_vehicle_targets;
  std::array<core::BossState, 2> bosses = {};
  std::array<PresentationTarget, 2> boss_targets = {};
  std::array<core::AirlockSnapshot, core::kMaxPlayers> airlocks = {};
  core::TrafficAuthority traffic_authority = {};
  core::PlayerId selected_traffic_authority = core::kInvalidPlayerId;
  uint32_t generation = 0;
  uint32_t enemy_generation = 0;
  uint32_t traffic_generation = 0;
};

class ReplicationMailbox {
 public:
  void publish_local_frame(std::unique_ptr<LocalReplicationFrame> frame) {
    local_frame_.publish(std::move(frame));
  }

  std::unique_ptr<LocalReplicationFrame> take_local_frame() {
    auto frame = local_frame_.take();
    return frame ? std::move(*frame) : nullptr;
  }

  void publish_remote_frame(std::unique_ptr<RemoteReplicationFrame> frame) {
    remote_frame_.publish(std::move(frame));
  }

  std::unique_ptr<RemoteReplicationFrame> take_remote_frame() {
    auto frame = remote_frame_.take();
    return frame ? std::move(*frame) : nullptr;
  }

  bool push_outbound_events(const std::vector<core::GameEvent>& events) {
    return outbound_events_.push_back(events);
  }

  bool restore_outbound_events(const std::vector<core::GameEvent>& events) {
    return outbound_events_.push_front(events);
  }

  std::vector<core::GameEvent> take_outbound_events(const size_t max_count) {
    return outbound_events_.take(max_count);
  }

  bool push_inbound_events(const std::vector<core::GameEvent>& events) {
    return inbound_events_.push_back(events);
  }

  void discard_participant_events(core::PlayerId player_id) {
    inbound_events_.erase_if(
        [player_id](const core::GameEvent& event) { return event.source_player_id == player_id; });
  }

  std::vector<core::GameEvent> take_inbound_events(const size_t max_count) {
    return inbound_events_.take(max_count);
  }

  void reset() {
    local_frame_.clear();
    remote_frame_.clear();
    outbound_events_.clear();
    inbound_events_.clear();
  }

  size_t outbound_event_count() const { return outbound_events_.size(); }
  size_t inbound_event_count() const { return inbound_events_.size(); }
  size_t inbound_event_capacity() const { return inbound_events_.remaining(); }

 private:
  platform::LatestValueMailbox<std::unique_ptr<LocalReplicationFrame>> local_frame_;
  platform::LatestValueMailbox<std::unique_ptr<RemoteReplicationFrame>> remote_frame_;
  platform::BoundedMailbox<core::GameEvent, kReplicationEventCapacity> outbound_events_;
  platform::BoundedMailbox<core::GameEvent, kReplicationEventCapacity> inbound_events_;
};

}  // namespace multiplayer::jak2::application
