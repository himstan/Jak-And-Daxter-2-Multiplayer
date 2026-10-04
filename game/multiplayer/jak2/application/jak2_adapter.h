#pragma once

#include <cstdint>
#include <optional>

#include "game/multiplayer/jak2/application/presentation_runtime.h"
#include "game/multiplayer/jak2/application/replication_mailbox.h"
#include "game/multiplayer/jak2/core/replication_state.h"
#include "game/multiplayer/platform/session/packet_registry.h"

namespace multiplayer::jak2::application {

class Jak2Adapter final : public platform::GameAdapter {
 public:
  Jak2Adapter();
  ~Jak2Adapter() noexcept override = default;

  Jak2Adapter(const Jak2Adapter&) = delete;
  Jak2Adapter& operator=(const Jak2Adapter&) = delete;

  ReplicationMailbox& mailbox() { return mailbox_; }
  const platform::GameDescriptor& descriptor() const override { return descriptor_; }
  bool configure_compatibility_identity(std::string identity) override;
  void installed(platform::GameSessionEndpoint& endpoint) override;
  void session_started(const platform::SessionState& session) override;
  void session_reset() override;
  platform::PacketRegistry& packets() override { return packets_; }
  bool validate_profile_extension(std::span<const uint8_t> extension,
                                  std::vector<uint8_t>& canonical) override;
  std::vector<uint8_t> create_bootstrap(platform::PlayerId player_id) override;
  bool apply_bootstrap(uint32_t generation, std::span<const uint8_t> payload) override;
  void player_profile_changed(const platform::PlayerProfile& profile) override;
  void player_departed(platform::PlayerId player_id) override;
  void tick(uint64_t now_ms) override;
  void stop() override;

 private:
  std::vector<std::unique_ptr<platform::PacketHandler>> make_packet_handlers();
  void add_player_handlers(std::vector<std::unique_ptr<platform::PacketHandler>>&);
  void add_world_handlers(std::vector<std::unique_ptr<platform::PacketHandler>>&);
  void add_entity_handlers(std::vector<std::unique_ptr<platform::PacketHandler>>&);
  void add_traffic_handlers(std::vector<std::unique_ptr<platform::PacketHandler>>&);
  std::vector<core::PlayerId> traffic_subscribers(core::PlayerId source,
                                                  const platform::SessionSnapshot& session) const;
  void add_event_handler(std::vector<std::unique_ptr<platform::PacketHandler>>&);
  void publish_remote_frame(const platform::SessionState& session, uint64_t now_ms);

  platform::GameSessionEndpoint* endpoint_ = nullptr;
  ReplicationMailbox mailbox_;
  core::ReplicationState state_;
  std::unique_ptr<LocalReplicationFrame> local_frame_;
  std::optional<core::TrafficAuthority> last_traffic_authority_;
  std::optional<core::PlayerRulesState> last_player_rules_;
  std::optional<uint64_t> last_remote_publish_ms_;
  uint32_t remote_generation_ = 0;
  std::array<uint32_t, core::kMaxPlayers> player_lifecycles_ = {};
  PresentationRuntime presentation_;
  platform::GameDescriptor descriptor_;
  platform::PacketRegistry packets_;
};

Jak2Adapter& jak2_adapter();

}  // namespace multiplayer::jak2::application
