#include "game/multiplayer/jak2/application/jak2_adapter.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "common/global_profiler/GlobalProfiler.h"
#include "common/log/log.h"

#include "game/multiplayer/jak2/core/validation.h"
#include "game/multiplayer/jak2/player_appearance.h"
#include "game/multiplayer/jak2/wire/packets/bootstrap_state_packet.h"
#include "game/multiplayer/platform/core/compatibility_identity.h"
#include "game/multiplayer/platform/runtime/multiplayer_runtime.h"

namespace multiplayer::jak2::application {
namespace {
constexpr uint32_t kRemoteFrameIntervalMs = 8;
}  // namespace

Jak2Adapter::Jak2Adapter()
    : descriptor_{.game_id = "jak2",
                  .maximum_payload_bytes = 32768,
                  .maximum_players = core::kMaxPlayers,
                  .maximum_profile_extension_bytes = sizeof(core::PlayerAppearance),
                  .supported_characters = {PlayerCharacter::JAK, PlayerCharacter::DAXTER}},
      packets_(make_packet_handlers(), descriptor_.maximum_payload_bytes) {}

bool Jak2Adapter::configure_compatibility_identity(const std::string identity) {
  if (!platform::valid_compatibility_identity(identity))
    return false;
  descriptor_.compatibility_identity = identity;
  return true;
}

void Jak2Adapter::installed(platform::GameSessionEndpoint& endpoint) {
  endpoint_ = &endpoint;
}

void Jak2Adapter::session_started(const platform::SessionState&) {
  state_.reset();
  local_frame_.reset();
  last_traffic_authority_.reset();
  last_remote_publish_ms_.reset();
  remote_generation_ = 0;
  presentation_.reset();
}

void Jak2Adapter::session_reset() {
  state_.reset();
  mailbox_.reset();
  local_frame_.reset();
  last_traffic_authority_.reset();
  last_remote_publish_ms_.reset();
  remote_generation_ = 0;
  presentation_.reset();
}

bool Jak2Adapter::validate_profile_extension(const std::span<const uint8_t> extension,
                                             std::vector<uint8_t>& canonical) {
  if (extension.size() != sizeof(core::PlayerAppearance))
    return false;
  core::PlayerAppearance appearance;
  std::memcpy(&appearance, extension.data(), sizeof(appearance));
  if (!is_player_appearance_valid(appearance))
    return false;
  canonical.assign(extension.begin(), extension.end());
  return true;
}

std::vector<uint8_t> Jak2Adapter::create_bootstrap(platform::PlayerId) {
  const auto& bootstrap = state_.world().bootstrap();
  if (bootstrap.host_continue[0] == 0)
    return {};
  return platform::wire::encode_packet(wire::to_packet(bootstrap)).value_or(std::vector<uint8_t>{});
}

bool Jak2Adapter::apply_bootstrap(const uint32_t generation,
                                  const std::span<const uint8_t> payload) {
  const auto packet = platform::wire::decode_packet<wire::BootstrapStatePacket>(payload);
  if (!packet)
    return false;
  core::BootstrapState bootstrap;
  wire::from_packet(*packet, bootstrap);
  return state_.apply_bootstrap(bootstrap, generation);
}

void Jak2Adapter::participant_profile_changed(const platform::ParticipantProfile& profile) {
  core::PlayerIdentity identity = {};
  identity.player_id = profile.participant;
  identity.character = profile.character;
  identity.lobby_ready = profile.ready;
  const auto name_size = std::min(profile.display_name.size(), identity.name.size() - 1);
  std::memcpy(identity.name.data(), profile.display_name.data(), name_size);
  if (profile.game_extension.size() == sizeof(identity.appearance))
    std::memcpy(&identity.appearance, profile.game_extension.data(), sizeof(identity.appearance));
  if (!state_.update_participant_identity(identity)) {
    lg::error("[MP-Jak2] Rejected canonical platform profile for participant {}.",
              profile.participant);
  }
}

void Jak2Adapter::participant_departed(const platform::PlayerId participant) {
  if (participant < core::kMaxPlayers) {
    ++participant_lifecycles_[participant];
    presentation_.reset_player(participant);
    mailbox_.discard_participant_events(participant);
  }
  state_.depart_participant(participant);
}

void Jak2Adapter::tick(const uint64_t now_ms) {
  if (!endpoint_)
    return;
  const auto session = endpoint_->snapshot().state;
  state_.expire(now_ms);
  if (auto frame = mailbox_.take_local_frame()) {
    if (frame->local_player_id == session.local_player_id &&
        frame->host_player_id == session.host_player_id) {
      local_frame_ = std::move(frame);
    } else {
      local_frame_.reset();
    }
  }
  packets_.publish(*endpoint_, now_ms);
  if (local_frame_ && session.role != platform::SessionRole::NONE)
    state_.traffic().select_authority(local_frame_->selected_traffic_authority);
  if (const auto available = mailbox_.inbound_event_capacity(); available != 0)
    mailbox_.push_inbound_events(state_.events().take(available));
  if (!last_remote_publish_ms_ || now_ms - *last_remote_publish_ms_ >= kRemoteFrameIntervalMs) {
    publish_remote_frame(session, now_ms);
    last_remote_publish_ms_ = now_ms;
  }
}

void Jak2Adapter::stop() {
  session_reset();
  endpoint_ = nullptr;
}

void Jak2Adapter::publish_remote_frame(const platform::SessionState& session,
                                       const uint64_t now_ms) {
  auto profile = scoped_prof("multiplayer::jak2::publish_remote_frame");
  auto frame = std::make_unique<RemoteReplicationFrame>();
  frame->participant_lifecycles = participant_lifecycles_;
  frame->identities = state_.participants().identities();
  frame->players = state_.participants().players();
  frame->player_vehicles = state_.participants().player_vehicles();
  frame->turrets = state_.participants().turrets();
  frame->world = state_.world().world();
  frame->gungame = state_.world().gungame();
  frame->bootstrap = state_.world().bootstrap();
  const auto& enemy_snapshot = state_.entities().enemies();
  frame->enemies.source_player_id = enemy_snapshot.source_player_id;
  frame->enemies.sample_time_ms = enemy_snapshot.sample_time_ms;
  frame->enemies.sequence = enemy_snapshot.sequence;
  for (const auto& enemy : enemy_snapshot.enemies) {
    if (enemy.owner_player_id != session.local_player_id)
      frame->enemies.enemies.push_back(enemy);
  }
  frame->selected_traffic = state_.traffic().selected_snapshot();
  frame->bosses[0] = state_.entities().boss(core::BossState::Kind::PALACE_SQUID);
  frame->bosses[1] = state_.entities().boss(core::BossState::Kind::WIDOW);
  frame->airlocks = state_.entities().airlocks();
  frame->traffic_authority = state_.traffic().authority();
  frame->selected_traffic_authority = state_.traffic().selected_authority();
  presentation_.prepare(*frame, state_, now_ms);
  frame->generation = ++remote_generation_;
  mailbox_.publish_remote_frame(std::move(frame));
}

Jak2Adapter& jak2_adapter() {
  auto* adapter = dynamic_cast<Jak2Adapter*>(platform::multiplayer_runtime().adapter("jak2"));
  if (!adapter)
    throw std::logic_error("Jak 2 multiplayer adapter is not installed");
  return *adapter;
}

}  // namespace multiplayer::jak2::application
