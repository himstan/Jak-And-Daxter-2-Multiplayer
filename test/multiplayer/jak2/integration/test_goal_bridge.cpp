#include <chrono>
#include <cstddef>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_set>
#include <vector>

#include "common/common_types.h"
#include "common/goos/ParseHelpers.h"
#include "common/goos/Reader.h"
#include "common/type_system/TypeSystem.h"
#include "common/type_system/deftype.h"
#include "common/util/json_util.h"

#include "game/kernel/common/kscheme.h"
#include "game/multiplayer/jak2/api/preferences.h"
#include "game/multiplayer/jak2/application/presentation_runtime.h"
#include "game/multiplayer/jak2/application/replication_mailbox.h"
#include "game/multiplayer/jak2/bridge/goal_bridge.h"
#include "game/multiplayer/jak2/bridge/goal_replication_types.h"
#include "game/multiplayer/jak2/wire/event_types.h"
#include "gtest/gtest.h"

extern u8* g_ee_main_mem;

namespace {

const fs::path& use_test_profiles() {
  static const fs::path root =
      fs::temp_directory_path() /
      ("jadmp-jak2-test-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  static bool configured = false;
  if (!configured) {
    set_multiplayer_preferences_root(root);
    load_multiplayer_preferences();
    configured = true;
  }
  return root;
}

class ScopedPreferencesRoot {
 public:
  ScopedPreferencesRoot() : previous_root_(use_test_profiles()) {
    const auto root = fs::temp_directory_path() /
                      ("jadmp-preferences-test-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    set_multiplayer_preferences_root(root);
    load_multiplayer_preferences();
    path_ = root / "jak2" / "profile-1" / "preferences.json";
  }

  ~ScopedPreferencesRoot() {
    set_multiplayer_preferences_root(previous_root_);
    load_multiplayer_preferences();
  }

  const fs::path& path() const { return path_; }

 private:
  fs::path previous_root_;
  fs::path path_;
};

class GoalMemoryFixture {
 public:
  GoalMemoryFixture()
      : previous_memory_(g_ee_main_mem),
        memory_(0x12000 + sizeof(MPReplicationStateGOAL) + 0x1000, 0) {
    g_ee_main_mem = memory_.data();
  }
  ~GoalMemoryFixture() { g_ee_main_mem = previous_memory_; }
  template <typename T>
  T& at(const uint32_t offset) {
    return *reinterpret_cast<T*>(memory_.data() + offset);
  }

 private:
  u8* previous_memory_ = nullptr;
  std::vector<u8> memory_;
};

MPReplicationStateGOAL& replication_state(GoalMemoryFixture& memory) {
  auto& state = memory.at<MPReplicationStateGOAL>(0x12000);
  state.abi_size = kMPReplicationStateSize;
  state.local_player_id = 0;
  state.host_player_id = 0;
  state.local.authority.selected = kMPInvalidPlayerId;
  state.remote.authority.selected = kMPInvalidPlayerId;
  return state;
}

}  // namespace

TEST(Jak2GoalBridge, NativeEventDefinitionsMatchEveryGoalIdAndPayloadSize) {
  TypeSystem types;
  types.add_builtin_types(GameVersion::Jak2);
  goos::Reader reader;
  std::vector<std::string> envelopes;
  const auto read = [&](const std::string& path) {
    return reader.read_from_file({std::string(MP_SOURCE_ROOT) + "/goal_src/jak2/" + path});
  };
  const auto load_types = [&](const std::string& path, const std::string& selected = "") {
    const auto source = read(path);
    goos::for_each_in_list(source.as_pair()->cdr, [&](const goos::Object& form) {
      if (!form.is_pair() || !form.as_pair()->car.is_symbol("deftype"))
        return;
      const auto& definition = form.as_pair()->cdr;
      const auto name = definition.as_pair()->car.as_symbol().name_ptr;
      if (selected.empty() || selected == name) {
        parse_deftype(definition, &types);
        if (selected.empty() && std::string(name) != "mp-event" &&
            types.tc(types.make_typespec("mp-event"), types.make_typespec(name)))
          envelopes.emplace_back(name);
      }
    });
  };
  load_types("kernel/gcommon.gc", "vector");
  load_types("kernel/gkernel-h.gc", "time-frame");
  load_types("engine/math/quaternion-h.gc", "quaternion");
  load_types("multiplayer/event/mp-event-h.gc");

  struct Contract {
    int64_t id;
    int size;
  };
  std::map<std::string, Contract> contracts;
  std::unordered_set<int64_t> ids;
  const auto declarations = read("multiplayer/event/mp-event-types.gc");
  goos::for_each_in_list(declarations.as_pair()->cdr, [&](const goos::Object& form) {
    if (!form.is_pair() || !form.as_pair()->car.is_symbol("define-mp-events"))
      return;
    goos::for_each_in_list(form.as_pair()->cdr, [&](const goos::Object& entry) {
      std::vector<goos::Object> values;
      goos::for_each_in_list(entry, [&](const auto& value) { values.push_back(value); });
      ASSERT_EQ(values.size(), 4u);
      ASSERT_TRUE(values[3].is_pair());
      ASSERT_TRUE(values[3].as_pair()->car.is_symbol());
      const std::string name = values[0].as_symbol().name_ptr;
      ASSERT_TRUE(name.starts_with("MP_EVENT_"));
      const auto id = values[1].as_int();
      const std::string payload = values[2].as_symbol().name_ptr;
      const int size = payload == "none" ? 0 : types.lookup_type(payload)->get_size_in_memory();
      EXPECT_TRUE(ids.insert(id).second);
      EXPECT_TRUE(contracts.emplace(name.substr(9), Contract{id, size}).second);
      EXPECT_GE(size, 0);
      EXPECT_LE(size, sizeof(MPEventGOAL::data));
    });
  });
  const auto& events = multiplayer::jak2::wire::kEvents;
  ASSERT_EQ(contracts.size(), events.size());
  for (const auto& event : events) {
    SCOPED_TRACE(event.name);
    const auto& contract = contracts.at(event.name);
    EXPECT_EQ(contract.id, event.id);
    EXPECT_EQ(contract.size, event.payload_size);
  }
  EXPECT_EQ(multiplayer::jak2::wire::event_descriptor(0), nullptr);
  EXPECT_EQ(multiplayer::jak2::wire::event_descriptor(255), nullptr);
  EXPECT_EQ(types.lookup_type("mp-event")->get_size_in_memory(), sizeof(MPEventGOAL));
  EXPECT_EQ(types.lookup_field_info("mp-event", "data").field.offset(),
            offsetof(MPEventGOAL, data));
  for (const auto& name : envelopes) {
    SCOPED_TRACE(name);
    EXPECT_EQ(types.lookup_type(name)->get_size_in_memory(), sizeof(MPEventGOAL));
    EXPECT_EQ(types.lookup_field_info(name, "payload").field.offset(), offsetof(MPEventGOAL, data));
  }
}

TEST(Jak2GoalBridge, DirectionalAggregateHasCanonicalCompactAbi) {
  EXPECT_EQ(sizeof(MPReplicationPlayerIdentityGOAL), 279u);
  EXPECT_EQ(sizeof(MPReplicationPlayerVehicleGOAL), 94u);
  EXPECT_EQ(sizeof(MPReplicationPlayerGOAL), 544u);
  EXPECT_EQ(sizeof(MPReplicationEnemySetGOAL), 57360u);
  EXPECT_EQ(sizeof(MPReplicationPedestrianStateGOAL), 60u);
  EXPECT_EQ(sizeof(MPReplicationTrafficSetGOAL), 13328u);
  EXPECT_EQ(sizeof(MPReplicationBootstrapStateGOAL), 16454u);
  EXPECT_EQ(sizeof(MPReplicationFrameGOAL), 92464u);
  EXPECT_EQ(sizeof(MPReplicationStateGOAL), 195216u);
  EXPECT_EQ(offsetof(MPReplicationStateGOAL, local), 16u);
  EXPECT_EQ(offsetof(MPReplicationStateGOAL, remote), 92480u);
  EXPECT_EQ(offsetof(MPReplicationStateGOAL, outbound_events), 184960u);
  EXPECT_EQ(offsetof(MPReplicationStateGOAL, inbound_events), 190096u);
}

TEST(Jak2GoalBridge, PlayerAppearanceConversionRoundTripsValues) {
  MPPlayerAppearanceGOAL goal = {};
  for (size_t index = 0; index < kMPPlayerAppearanceSlotCount; ++index) {
    goal.colors[index] = static_cast<uint32_t>(index) * 0x010203u;
    goal.strengths[index] = static_cast<float>(index) / 32.0f;
  }
  const auto appearance = get_player_appearance_from_goal(goal);
  MPPlayerAppearanceGOAL round_trip = {};
  copy_player_appearance_to_goal(appearance, round_trip);
  EXPECT_EQ(std::memcmp(&goal, &round_trip, sizeof(goal)), 0);
}

TEST(Jak2GoalBridge, FreeBridgeFunctionsReadAndWriteGoalValues) {
  GoalMemoryFixture memory;
  auto& goal_string = memory.at<String>(0x10000);
  goal_string.len = 5;
  std::memcpy(goal_string.data(), "hello", 6);
  std::string text;
  ASSERT_TRUE(multiplayer::jak2::bridge::read_string(0x10000, text));
  EXPECT_EQ(text, "hello");

  auto& config = memory.at<MPPlayerCharacterConfigGOAL>(0x11000);
  for (size_t index = 0; index < kMPMaxPlayers; ++index) {
    config.characters[index] =
        static_cast<uint32_t>(index % 2 == 0 ? PlayerCharacter::JAK : PlayerCharacter::DAXTER);
  }
  std::array<PlayerCharacter, kMPMaxPlayers> characters = {};
  ASSERT_TRUE(multiplayer::jak2::bridge::read_character_config(0x11000, characters));
  EXPECT_EQ(characters, get_default_player_character_config());

  auto& source = memory.at<MPPlayerAppearanceGOAL>(0x11500);
  for (size_t index = 0; index < kMPPlayerAppearanceSlotCount; ++index) {
    source.colors[index] = static_cast<uint32_t>(index + 1);
    source.strengths[index] = static_cast<float>(index) / 10.0f;
  }
  MPPlayerAppearance appearance = {};
  ASSERT_TRUE(multiplayer::jak2::bridge::read_appearance(0x11500, appearance));
  ASSERT_TRUE(multiplayer::jak2::bridge::write_appearance(0x11800, appearance));
  const auto& destination = memory.at<MPPlayerAppearanceGOAL>(0x11800);
  EXPECT_EQ(std::memcmp(&source, &destination, sizeof(source)), 0);
}

TEST(Jak2GoalBridge, ExchangeRejectsInvalidAddressAndAbiSize) {
  GoalMemoryFixture memory;
  multiplayer::jak2::application::ReplicationMailbox mailbox;
  auto remote = std::make_unique<multiplayer::jak2::application::RemoteReplicationFrame>();
  remote->generation = 1;
  mailbox.publish_remote_frame(std::move(remote));
  EXPECT_FALSE(multiplayer::jak2::bridge::exchange_state(0, mailbox));
  auto queued_remote = mailbox.take_remote_frame();
  ASSERT_TRUE(queued_remote);
  EXPECT_EQ(queued_remote->generation, 1u);
  auto& state = replication_state(memory);
  state.abi_size = kMPReplicationStateSize - 1;
  remote = std::make_unique<multiplayer::jak2::application::RemoteReplicationFrame>();
  remote->generation = 2;
  mailbox.publish_remote_frame(std::move(remote));
  EXPECT_FALSE(multiplayer::jak2::bridge::exchange_state(0x12000, mailbox));
  EXPECT_FALSE(mailbox.take_local_frame());
  queued_remote = mailbox.take_remote_frame();
  ASSERT_TRUE(queued_remote);
  EXPECT_EQ(queued_remote->generation, 2u);
}

TEST(Jak2GoalBridge, ExchangeCopiesLocalStateAndAcknowledgesOnlyAcceptedEvents) {
  GoalMemoryFixture memory;
  auto& state = replication_state(memory);
  state.local.players[0].transform.position[0] = 42.0f;
  state.outbound_event_count = 1;
  state.outbound_events[0].etype = 30;
  state.outbound_events[0].payload_size = 1;
  state.outbound_events[0].data[0] = 7;
  multiplayer::jak2::application::ReplicationMailbox mailbox;
  ASSERT_TRUE(multiplayer::jak2::bridge::exchange_state(0x12000, mailbox));
  EXPECT_EQ(state.outbound_event_count, 0);
  const auto local = mailbox.take_local_frame();
  ASSERT_TRUE(local != nullptr);
  EXPECT_FLOAT_EQ(local->players[0].position[0], 42.0f);
  const auto events = mailbox.take_outbound_events(1);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].payload[0], 7u);
  std::vector<multiplayer::jak2::core::GameEvent> full(
      multiplayer::jak2::application::kReplicationEventCapacity);
  ASSERT_TRUE(mailbox.push_outbound_events(full));
  state.outbound_event_count = 1;
  ASSERT_TRUE(multiplayer::jak2::bridge::exchange_state(0x12000, mailbox));
  EXPECT_EQ(state.outbound_event_count, 1);
}

TEST(Jak2GoalBridge, ExchangePreservesFullBootstrapAidCapacity) {
  GoalMemoryFixture memory;
  auto& state = replication_state(memory);
  constexpr auto capacity = multiplayer::jak2::core::kMaxBootstrapAids;
  state.local.bootstrap.synchronized_aid_count = static_cast<uint16_t>(capacity);
  state.local.bootstrap.synchronized_aids[0] = 11;
  state.local.bootstrap.synchronized_aids[128] = 22;
  state.local.bootstrap.synchronized_aids[capacity - 1] = 33;
  multiplayer::jak2::application::ReplicationMailbox mailbox;
  ASSERT_TRUE(multiplayer::jak2::bridge::exchange_state(0x12000, mailbox));
  const auto local = mailbox.take_local_frame();
  ASSERT_TRUE(local != nullptr);
  EXPECT_EQ(local->bootstrap.synchronized_aid_count, capacity);
  EXPECT_EQ(local->bootstrap.synchronized_aids[0], 11u);
  EXPECT_EQ(local->bootstrap.synchronized_aids[128], 22u);
  EXPECT_EQ(local->bootstrap.synchronized_aids[capacity - 1], 33u);

  auto remote = std::make_unique<multiplayer::jak2::application::RemoteReplicationFrame>();
  remote->bootstrap = local->bootstrap;
  mailbox.publish_remote_frame(std::move(remote));
  ASSERT_TRUE(multiplayer::jak2::bridge::exchange_state(0x12000, mailbox));
  EXPECT_EQ(state.remote.bootstrap.synchronized_aid_count, capacity);
  EXPECT_EQ(state.remote.bootstrap.synchronized_aids[0], 11u);
  EXPECT_EQ(state.remote.bootstrap.synchronized_aids[128], 22u);
  EXPECT_EQ(state.remote.bootstrap.synchronized_aids[capacity - 1], 33u);
}

TEST(Jak2GoalBridge, InvalidLocalCaptureStillPublishesRemoteStateAndEvents) {
  GoalMemoryFixture memory;
  auto& state = replication_state(memory);
  state.local_player_id = kMPInvalidPlayerId;
  auto remote = std::make_unique<multiplayer::jak2::application::RemoteReplicationFrame>();
  remote->generation = 9;
  std::vector<multiplayer::jak2::core::GameEvent> events = {
      {.event_id = 7, .source_player_id = 1, .payload_size = 1, .payload = {42}}};
  multiplayer::jak2::application::ReplicationMailbox mailbox;
  mailbox.publish_remote_frame(std::move(remote));
  ASSERT_TRUE(mailbox.push_inbound_events(events));

  EXPECT_FALSE(multiplayer::jak2::bridge::exchange_state(0x12000, mailbox));
  EXPECT_FALSE(mailbox.take_local_frame());
  EXPECT_EQ(state.remote.generation, 9u);
  ASSERT_EQ(state.inbound_event_count, 1u);
  EXPECT_EQ(state.inbound_events[0].etype, 7u);
  EXPECT_EQ(state.inbound_events[0].data[0], 42u);
}

TEST(Jak2GoalBridge, InvalidRemoteStateStillPublishesLocalFrame) {
  GoalMemoryFixture memory;
  auto& state = replication_state(memory);
  state.local.players[0].transform.position[0] = 42.0f;
  state.inbound_event_count = 65;
  multiplayer::jak2::application::ReplicationMailbox mailbox;

  EXPECT_FALSE(multiplayer::jak2::bridge::exchange_state(0x12000, mailbox));
  const auto local = mailbox.take_local_frame();
  ASSERT_TRUE(local);
  EXPECT_FLOAT_EQ(local->players[0].position[0], 42.0f);
}

TEST(Jak2GoalBridge, ExchangePublishesTargetsAndPreservesLocalHalf) {
  GoalMemoryFixture memory;
  auto& state = replication_state(memory);
  state.local.generation = 77;
  auto remote = std::make_unique<multiplayer::jak2::application::RemoteReplicationFrame>();
  remote->generation = 9;
  remote->players[1].player_id = 1;
  remote->players[1].state_ready = true;
  remote->player_targets[1].valid = true;
  remote->player_targets[1].generation = 4;
  remote->player_targets[1].position = {1.0f, 2.0f, 3.0f};
  remote->player_targets[1].quaternion = {0.0f, 0.5f, 0.0f, 0.5f};
  remote->player_targets[1].velocity = {4.0f, 5.0f, 6.0f};
  remote->selected_traffic.sequence = 3;
  remote->selected_traffic.pedestrians.push_back({.net_id = 0x11000001});
  multiplayer::jak2::application::ReplicationMailbox mailbox;
  mailbox.publish_remote_frame(std::move(remote));
  ASSERT_TRUE(multiplayer::jak2::bridge::exchange_state(0x12000, mailbox));
  EXPECT_EQ(state.local.generation, 77u);
  EXPECT_EQ(state.remote.generation, 9u);
  EXPECT_EQ(state.remote.players[1].generation, 4u);
  EXPECT_EQ(state.remote.players[1].valid, 1u);
  EXPECT_FLOAT_EQ(state.remote.players[1].presentation_position[1], 2.0f);
  EXPECT_FLOAT_EQ(state.remote.players[1].presentation_velocity[2], 6.0f);
  ASSERT_EQ(state.remote.traffic.pedestrian_count, 1u);
  EXPECT_EQ(state.remote.traffic.pedestrians[0].value.net_id, 0x11000001u);
}

TEST(Jak2GoalBridge, EmptyRemoteAirlockSnapshotClearsPreviousGoalRecords) {
  GoalMemoryFixture memory;
  auto& state = replication_state(memory);
  multiplayer::jak2::application::ReplicationMailbox mailbox;

  auto populated = std::make_unique<multiplayer::jak2::application::RemoteReplicationFrame>();
  populated->airlocks[1].source_player_id = 1;
  populated->airlocks[1].sequence = 7;
  populated->airlocks[1].states = {{.airlock_aid = 17, .state_id = 1},
                                   {.airlock_aid = 18, .state_id = 3}};
  populated->airlocks[2].source_player_id = 2;
  populated->airlocks[2].sequence = 9;
  populated->airlocks[2].states = {{.airlock_aid = 19, .state_id = 2}};
  mailbox.publish_remote_frame(std::move(populated));
  ASSERT_TRUE(multiplayer::jak2::bridge::exchange_state(0x12000, mailbox));
  ASSERT_EQ(state.remote.airlocks.count, 3u);
  EXPECT_EQ(state.remote.airlocks.states[0].value.airlock_aid, 17u);
  EXPECT_EQ(state.remote.airlocks.states[1].value.airlock_aid, 18u);
  EXPECT_EQ(state.remote.airlocks.states[0].value.source_player_id, 1u);
  EXPECT_EQ(state.remote.airlocks.states[2].value.airlock_aid, 19u);
  EXPECT_EQ(state.remote.airlocks.states[2].value.source_player_id, 2u);

  auto empty = std::make_unique<multiplayer::jak2::application::RemoteReplicationFrame>();
  empty->airlocks[1].source_player_id = 1;
  empty->airlocks[1].sequence = 8;
  mailbox.publish_remote_frame(std::move(empty));
  ASSERT_TRUE(multiplayer::jak2::bridge::exchange_state(0x12000, mailbox));
  EXPECT_EQ(state.remote.airlocks.count, 0u);
  for (const auto& slot : state.remote.airlocks.states) {
    EXPECT_EQ(slot.value.airlock_aid, 0u);
    EXPECT_EQ(slot.value.state_id, 0u);
    EXPECT_EQ(slot.value.level_id, 0u);
    EXPECT_EQ(slot.value.sequence, 0u);
  }
}

TEST(Jak2GoalBridge, RemoteReplicationIgnoresLocallyOwnedEnemies) {
  GoalMemoryFixture memory;
  auto& state = replication_state(memory);
  state.local_player_id = 0;
  multiplayer::jak2::application::ReplicationMailbox mailbox;

  auto frame = std::make_unique<multiplayer::jak2::application::RemoteReplicationFrame>();
  frame->enemies.sequence = 12;
  frame->enemies.enemies = {
      {.actor_id = 101, .owner_player_id = 0, .hit_points = 4},
      {.actor_id = 102, .owner_player_id = 1, .hit_points = 3},
  };
  mailbox.publish_remote_frame(std::move(frame));
  ASSERT_TRUE(multiplayer::jak2::bridge::exchange_state(0x12000, mailbox));
  ASSERT_EQ(state.remote.enemies.count, 1u);
  EXPECT_EQ(state.remote.enemies.enemies[0].actor_id, 102u);
  EXPECT_EQ(state.remote.enemies.enemies[0].owner_player_id, 1u);
  EXPECT_EQ(state.remote.enemies.enemies[0].hp, 3);
}

TEST(Jak2GoalBridge, MailboxTransfersLatestFrameOwnershipAndResets) {
  using namespace multiplayer::jak2::application;
  ReplicationMailbox mailbox;
  EXPECT_FALSE(mailbox.take_local_frame());
  EXPECT_FALSE(mailbox.take_remote_frame());

  mailbox.publish_local_frame(std::make_unique<LocalReplicationFrame>());
  mailbox.publish_remote_frame(std::make_unique<RemoteReplicationFrame>());
  auto local = std::make_unique<LocalReplicationFrame>();
  auto remote = std::make_unique<RemoteReplicationFrame>();
  local->bootstrap.synchronized_aids.back() = 123;
  remote->bootstrap.synchronized_aids.back() = 456;
  const auto* local_address = local.get();
  const auto* remote_address = remote.get();
  mailbox.publish_local_frame(std::move(local));
  mailbox.publish_remote_frame(std::move(remote));
  EXPECT_FALSE(local);
  EXPECT_FALSE(remote);

  local = mailbox.take_local_frame();
  remote = mailbox.take_remote_frame();
  ASSERT_EQ(local.get(), local_address);
  ASSERT_EQ(remote.get(), remote_address);
  EXPECT_EQ(local->bootstrap.synchronized_aids.back(), 123u);
  EXPECT_EQ(remote->bootstrap.synchronized_aids.back(), 456u);
  EXPECT_FALSE(mailbox.take_local_frame());
  EXPECT_FALSE(mailbox.take_remote_frame());

  mailbox.publish_local_frame(std::move(local));
  mailbox.publish_remote_frame(std::move(remote));
  mailbox.reset();
  EXPECT_FALSE(mailbox.take_local_frame());
  EXPECT_FALSE(mailbox.take_remote_frame());
}

TEST(Jak2GoalBridge, RepeatedRemoteExchangeClearsStaleDataAndPreservesLocalFrame) {
  using namespace multiplayer::jak2::application;
  GoalMemoryFixture memory;
  auto& state = replication_state(memory);
  state.local.generation = 77;
  state.local.bootstrap.synchronized_aid_count = multiplayer::jak2::core::kMaxBootstrapAids;
  std::ranges::fill(state.local.bootstrap.synchronized_aids, 99u);
  auto original_local = std::make_unique<MPReplicationFrameGOAL>();
  std::memcpy(original_local.get(), &state.local, sizeof(state.local));
  ReplicationMailbox mailbox;

  for (uint32_t generation = 1; generation <= 4; ++generation) {
    auto remote = std::make_unique<RemoteReplicationFrame>();
    remote->generation = generation;
    const bool populated = generation % 2 != 0;
    if (populated) {
      remote->bootstrap.sequence = generation;
      remote->bootstrap.synchronized_aid_count = multiplayer::jak2::core::kMaxBootstrapAids;
      remote->bootstrap.synchronized_aids.fill(123);
      remote->enemies.sequence = generation;
      remote->enemies.enemies.push_back({.actor_id = 101, .owner_player_id = 1});
      remote->selected_traffic.sequence = generation;
      remote->selected_traffic.pedestrians.push_back({.net_id = 0x11000001});
      remote->selected_traffic.vehicles.push_back({.net_id = 0x12000001});
    }
    mailbox.publish_remote_frame(std::move(remote));
    ASSERT_TRUE(multiplayer::jak2::bridge::exchange_state(0x12000, mailbox));
    EXPECT_EQ(std::memcmp(original_local.get(), &state.local, sizeof(state.local)), 0);
    EXPECT_EQ(state.remote.generation, generation);
    EXPECT_EQ(state.remote.bootstrap_valid, populated);
    EXPECT_EQ(state.remote.enemies.count, populated ? 1u : 0u);
    EXPECT_EQ(state.remote.traffic.pedestrian_count, populated ? 1u : 0u);
    EXPECT_EQ(state.remote.traffic.vehicle_count, populated ? 1u : 0u);
    for (const auto aid : state.remote.bootstrap.synchronized_aids)
      EXPECT_EQ(aid, populated ? 123u : 0u);
    if (!populated) {
      EXPECT_EQ(state.remote.enemies.enemies[0].actor_id, 0u);
      EXPECT_EQ(state.remote.traffic.pedestrians[0].value.net_id, 0u);
      EXPECT_EQ(state.remote.traffic.vehicles[0].net_id, 0u);
    }
  }
}

TEST(Jak2GoalBridge, RoomCodeGeneratedWhenPreferenceEmpty) {
  use_test_profiles();
  const auto original_prefs = multiplayer_preferences();

  set_room_code_preference("");
  EXPECT_TRUE(multiplayer_preferences().room_code.empty());

  const std::string resolved = get_resolved_host_room_code();
  EXPECT_EQ(resolved.size(), multiplayer::platform::kMultiplayerRoomCodeLength);
  std::string normalized;
  EXPECT_TRUE(multiplayer::platform::normalize_room_code(resolved, normalized, false));
  EXPECT_EQ(resolved, normalized);

  set_room_code_preference(original_prefs.room_code);
}

TEST(Jak2GoalBridge, RoomCodeResolutionPreservesConfiguredPreferences) {
  use_test_profiles();
  const auto original_prefs = multiplayer_preferences();

  EXPECT_TRUE(set_room_code_preference("ABC123"));
  EXPECT_EQ(get_resolved_host_room_code(), "ABC123");

  EXPECT_TRUE(set_room_code_preference("xyz789"));
  EXPECT_EQ(get_resolved_host_room_code(), "XYZ789");

  set_room_code_preference(original_prefs.room_code);
}

TEST(Jak2GoalBridge, PreferenceFieldsValidateBeforeMutation) {
  use_test_profiles();
  const auto original_prefs = multiplayer_preferences();

  EXPECT_FALSE(set_multiplayer_preference(0, "1023"));
  EXPECT_EQ(multiplayer_preferences().network_port, original_prefs.network_port);
  EXPECT_TRUE(set_multiplayer_preference(0, "26212"));
  EXPECT_EQ(multiplayer_preferences().network_port, 26212);

  EXPECT_FALSE(set_multiplayer_preference(1, "bad-code"));
  EXPECT_TRUE(set_multiplayer_preference(1, "abc123"));
  EXPECT_EQ(multiplayer_preferences().room_code, "ABC123");

  EXPECT_FALSE(set_multiplayer_preference(2, "bad name"));
  EXPECT_TRUE(set_multiplayer_preference(2, "Player2"));
  EXPECT_EQ(multiplayer_preferences().player_name, "Player2");

  EXPECT_TRUE(set_multiplayer_preference(0, std::to_string(original_prefs.network_port)));
  EXPECT_TRUE(set_multiplayer_preference(1, original_prefs.room_code));
  EXPECT_TRUE(set_multiplayer_preference(2, original_prefs.player_name));
}

TEST(Jak2GoalBridge, TexturePreferencesKeepValidGroupsAndRepairInvalidGroups) {
  ScopedPreferencesRoot preferences_root;
  auto root =
      parse_commented_json(file_util::read_text_file(preferences_root.path()), "preferences.json");
  auto& groups = root["player_texture_groups"];
  groups["jak_jacket"] = {{"color", "#ABCDEF"}, {"tint_strength", 0.6f}};
  groups["jak_pants"] = {{"color", "#123456"}, {"tint_strength", 0.4f}};
  file_util::write_text_file(preferences_root.path(), root.dump(2));
  const std::string valid_contents = file_util::read_text_file(preferences_root.path());
  load_multiplayer_preferences();
  EXPECT_EQ(file_util::read_text_file(preferences_root.path()), valid_contents);

  groups.erase("jak_straps");
  groups["jak_leggings"]["tint_strength"] = 2.0f;
  file_util::write_text_file(preferences_root.path(), root.dump(2));
  load_multiplayer_preferences();
  constexpr auto primary = player_appearance_group_index(MPPlayerAppearanceGroup::PRIMARY);
  constexpr auto leggings = player_appearance_group_index(MPPlayerAppearanceGroup::JAK_LEGGINGS);
  constexpr auto straps = player_appearance_group_index(MPPlayerAppearanceGroup::JAK_STRAPS);
  const auto& [colors, strengths] = multiplayer_preferences().player_appearance;
  EXPECT_EQ(colors[leggings], colors[primary]);
  EXPECT_FLOAT_EQ(strengths[leggings], 0.0f);
  EXPECT_EQ(colors[straps], colors[primary]);
  EXPECT_FLOAT_EQ(strengths[straps], 0.0f);
  const auto saved =
      parse_commented_json(file_util::read_text_file(preferences_root.path()), "preferences.json");
  EXPECT_EQ(saved["player_texture_groups"]["jak_leggings"]["color"],
            format_player_color(colors[primary]));
  EXPECT_EQ(saved["player_texture_groups"]["jak_straps"]["color"],
            format_player_color(colors[primary]));
}

TEST(Jak2GoalBridge, LifecycleChangeDiscardsAlreadyCopiedEventsFromPreviousOccupant) {
  GoalMemoryFixture memory;
  auto& state = replication_state(memory);
  state.remote.players[1].lifecycle_generation = 3;
  state.inbound_event_count = 3;
  state.inbound_events[0].source_player_id = 1;
  state.inbound_events[1].source_player_id = 2;
  state.inbound_events[2].source_player_id = 1;
  multiplayer::jak2::application::ReplicationMailbox mailbox;
  auto frame = std::make_unique<multiplayer::jak2::application::RemoteReplicationFrame>();
  frame->participant_lifecycles[1] = 4;
  frame->identities[1].joined = true;
  mailbox.publish_remote_frame(std::move(frame));
  ASSERT_TRUE(multiplayer::jak2::bridge::exchange_state(0x12000, mailbox));
  EXPECT_EQ(state.remote.players[1].lifecycle_generation, 4u);
  ASSERT_EQ(state.inbound_event_count, 1u);
  EXPECT_EQ(state.inbound_events[0].source_player_id, 2u);
}

TEST(Jak2GoalBridge, ExpiredHostRetainsRawDestinationButCannotBePresentedAsReady) {
  using namespace multiplayer::jak2;
  GoalMemoryFixture memory;
  auto& state = replication_state(memory);
  core::ReplicationState replication;
  core::PlayerState player;
  player.player_id = 1;
  player.position = {123.0f, 456.0f, 789.0f};
  player.activity = core::PlayerActivity::IN_GAME;
  player.state_ready = true;
  player.levels[0].level_id = 1;
  ASSERT_TRUE(replication.participants().apply(
      player, {.sequence = 1, .source = {.authenticated_player_id = 1}, .received_at_ms = 100}));
  replication.expire(2101);
  auto frame = std::make_unique<application::RemoteReplicationFrame>();
  frame->players = replication.participants().players();
  frame->identities = replication.participants().identities();
  application::PresentationRuntime presentation;
  presentation.prepare(*frame, replication, 2101);
  application::ReplicationMailbox mailbox;
  mailbox.publish_remote_frame(std::move(frame));
  ASSERT_TRUE(bridge::exchange_state(0x12000, mailbox));
  EXPECT_EQ(state.remote.players[1].valid, 0u);
  EXPECT_EQ(state.remote.players[1].identity.state_ready, 0u);
  EXPECT_FLOAT_EQ(state.remote.players[1].transform.position[0], 123.0f);
  EXPECT_EQ(state.remote.players[1].transform.levels[0].level_id, 1u);
}

TEST(Jak2GoalBridge, AuthoritativeCoordinatesStayDistinctFromInterpolatedPresentation) {
  GoalMemoryFixture memory;
  auto& state = replication_state(memory);
  multiplayer::jak2::application::ReplicationMailbox mailbox;
  auto frame = std::make_unique<multiplayer::jak2::application::RemoteReplicationFrame>();
  frame->players[1].position = {100.0f, 200.0f, 300.0f};
  frame->players[1].levels[0].level_id = 2;
  frame->player_targets[1].position = {90.0f, 190.0f, 290.0f};
  frame->player_targets[1].valid = true;
  mailbox.publish_remote_frame(std::move(frame));
  ASSERT_TRUE(multiplayer::jak2::bridge::exchange_state(0x12000, mailbox));
  EXPECT_FLOAT_EQ(state.remote.players[1].transform.position[0], 100.0f);
  EXPECT_FLOAT_EQ(state.remote.players[1].presentation_position[0], 90.0f);
  EXPECT_EQ(state.remote.players[1].transform.levels[0].level_id, 2u);
}

TEST(Jak2GoalBridge, RemoteEnemiesPreserveFullCapacityAcrossParticipants) {
  GoalMemoryFixture memory;
  auto& state = replication_state(memory);
  multiplayer::jak2::application::ReplicationMailbox mailbox;
  auto frame = std::make_unique<multiplayer::jak2::application::RemoteReplicationFrame>();
  for (uint8_t source = 1; source < multiplayer::jak2::core::kMaxPlayers; ++source) {
    for (uint32_t index = 0; index < multiplayer::jak2::core::kMaxEnemies; ++index)
      frame->enemies.enemies.push_back(
          {.actor_id = source * static_cast<uint32_t>(multiplayer::jak2::core::kMaxEnemies) + index,
           .owner_player_id = source});
  }
  mailbox.publish_remote_frame(std::move(frame));
  ASSERT_TRUE(multiplayer::jak2::bridge::exchange_state(0x12000, mailbox));
  ASSERT_EQ(state.remote.enemies.count, kMPReplicationEnemyCapacity);
  EXPECT_EQ(state.remote.enemies.enemies[0].owner_player_id, 1u);
  EXPECT_EQ(state.remote.enemies.enemies[kMPReplicationEnemyCapacity - 1].owner_player_id,
            multiplayer::jak2::core::kMaxPlayers - 1);
  mailbox.publish_remote_frame(
      std::make_unique<multiplayer::jak2::application::RemoteReplicationFrame>());
  ASSERT_TRUE(multiplayer::jak2::bridge::exchange_state(0x12000, mailbox));
  EXPECT_EQ(state.remote.enemies.count, 0u);
  EXPECT_EQ(state.remote.enemies.enemies[kMPReplicationEnemyCapacity - 1].actor_id, 0u);
}
