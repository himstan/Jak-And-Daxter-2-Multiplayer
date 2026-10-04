#include <chrono>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

#include "common/common_types.h"
#include "common/goal_constants.h"
#include "common/goos/Interpreter.h"
#include "common/goos/ParseHelpers.h"
#include "common/goos/Reader.h"
#include "common/type_system/TypeSystem.h"
#include "common/type_system/defenum.h"
#include "common/type_system/deftype.h"
#include "common/util/FileUtil.h"
#include "common/util/json_util.h"

#include "game/kernel/common/kscheme.h"
#include "game/multiplayer/jak2/api/preferences.h"
#include "game/multiplayer/jak2/application/presentation_runtime.h"
#include "game/multiplayer/jak2/application/replication_mailbox.h"
#include "game/multiplayer/jak2/bridge/goal_bridge.h"
#include "game/multiplayer/jak2/bridge/goal_preferences_types.h"
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

MultiplayerPreferencesGOAL& preferences_state(GoalMemoryFixture& memory) {
  auto& state = memory.at<MultiplayerPreferencesGOAL>(0x11000);
  state.player_name = 0x10000;
  state.room_code = 0x10100;
  memory.at<String>(state.player_name).len = 15;
  memory.at<String>(state.room_code).len = 6;
  return state;
}

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

TEST(Jak2GoalBridge, PedestrianAppearanceLayoutMatchesGoal) {
  TypeSystem types;
  types.add_builtin_types(GameVersion::Jak2);
  goos::Reader reader;
  const auto load = [&](const std::string& path, const std::string& name) {
    const auto source =
        reader.read_from_file({std::string(MP_SOURCE_ROOT) + "/goal_src/jak2/" + path});
    goos::for_each_in_list(source.as_pair()->cdr, [&](const goos::Object& form) {
      if (!form.is_pair())
        return;
      const auto& definition = form.as_pair()->cdr;
      if (!definition.is_pair() || !definition.as_pair()->car.is_symbol(name))
        return;
      if (form.as_pair()->car.is_symbol("defenum"))
        parse_defenum(definition, &types, nullptr);
      else if (form.as_pair()->car.is_symbol("deftype"))
        parse_deftype(definition, &types);
    });
  };
  load("kernel/gcommon.gc", "vector");
  load("engine/math/quaternion-h.gc", "quaternion");
  load("multiplayer/system/traffic/mp-traffic-types.gc", "pedestrian-appearance");
  EXPECT_EQ(types.lookup_type("pedestrian-appearance")->get_size_in_memory(), sizeof(uint32_t));
  const auto parts = types.lookup_bitfield_info("pedestrian-appearance", "parts");
  const auto width = types.lookup_bitfield_info("pedestrian-appearance", "width-scale");
  const auto height = types.lookup_bitfield_info("pedestrian-appearance", "height-scale");
  EXPECT_EQ(parts.offset, 0);
  EXPECT_EQ(parts.size, 24);
  EXPECT_EQ(width.offset, 24);
  EXPECT_EQ(width.size, 4);
  EXPECT_EQ(height.offset, 28);
  EXPECT_EQ(height.size, 4);
  EXPECT_FALSE(parts.sign_extend);
  EXPECT_FALSE(width.sign_extend);
  EXPECT_FALSE(height.sign_extend);
  load("multiplayer/system/traffic/mp-traffic-types.gc", "mp-pedestrian-flags");
  load("multiplayer/system/traffic/mp-traffic-types.gc", "mp-pedestrian-state");
  EXPECT_EQ(types.lookup_type("mp-pedestrian-state")->get_size_in_memory(), 60u);
  load("multiplayer/data/mp-replication-h.gc", "mp-replication-pedestrian-state");
  EXPECT_EQ(types.lookup_type("mp-replication-pedestrian-state")->get_size_in_memory(),
            sizeof(MPReplicationPedestrianStateGOAL));
  EXPECT_EQ(
      types.lookup_field_info("mp-replication-pedestrian-state", "appearance-mask").field.offset(),
      offsetof(MPReplicationPedestrianStateGOAL, appearance_mask));
  EXPECT_EQ(types.lookup_field_info("mp-replication-pedestrian-state", "state-id").field.offset(),
            offsetof(MPReplicationPedestrianStateGOAL, state_id));
  EXPECT_EQ(
      types.lookup_field_info("mp-replication-pedestrian-state", "travel-speed").field.offset(),
      offsetof(MPReplicationPedestrianStateGOAL, travel_speed));
  EXPECT_EQ(types.lookup_field_info("mp-pedestrian-state", "travel-speed").field.offset(), 59);
  const auto source =
      reader.read_from_file({std::string(MP_SOURCE_ROOT) +
                             "/goal_src/jak2/multiplayer/system/traffic/mp-traffic-types.gc"});
  bool found_resolution = false;
  goos::for_each_in_list(source.as_pair()->cdr, [&](const goos::Object& form) {
    if (!form.is_pair() || !form.as_pair()->car.is_symbol("defconstant"))
      return;
    const auto& definition = form.as_pair()->cdr;
    if (definition.as_pair()->car.is_symbol("PEDESTRIAN_TRAVEL_SPEED_RESOLUTION")) {
      EXPECT_FLOAT_EQ(definition.as_pair()->cdr.as_pair()->car.as_float(),
                      kPedestrianTravelSpeedResolution);
      found_resolution = true;
    }
  });
  EXPECT_TRUE(found_resolution);
}

TEST(Jak2GoalBridge, PedestrianAnimationChoicesIgnoreSpawnOrderAndSharedRandomness) {
  ASSERT_TRUE(file_util::setup_project_path(fs::path(MP_SOURCE_ROOT), true));
  goos::Interpreter interpreter;
  const auto environment = interpreter.global_environment.as_env_ptr();
  uint32_t net_id = 0;
  uint8_t object_type = 0;
  int random_calls = 0;
  interpreter.register_form("->", [&](const auto&, auto& args, const auto&) {
    return goos::Object::make_integer(args.unnamed.at(1).is_symbol("net-id") ? net_id
                                                                             : object_type);
  });
  interpreter.register_form(
      "nonnull?", [&](const auto&, auto&, const auto&) { return interpreter.intern("#t"); });
  interpreter.register_form("zero?", [&](const auto&, auto& args, const auto& env) {
    interpreter.eval_args(&args, env);
    return interpreter.intern(args.unnamed.at(0).as_int() == 0 ? "#t" : "#f");
  });
  for (const auto* cast : {"the", "the-as"}) {
    interpreter.register_form(cast, [&](const auto&, auto& args, const auto& env) {
      return interpreter.eval(args.unnamed.at(1), env);
    });
  }
  for (const std::string operation : {"logxor", "logand", "shr", "mod"}) {
    interpreter.register_form(operation, [&, operation](const auto&, auto& args, const auto& env) {
      interpreter.eval_args(&args, env);
      int64_t result = args.unnamed.at(0).as_int();
      for (size_t index = 1; index < args.unnamed.size(); ++index) {
        const auto value = args.unnamed[index].as_int();
        if (operation == "logxor")
          result ^= value;
        else if (operation == "logand")
          result &= value;
        else if (operation == "shr")
          result = static_cast<uint64_t>(result) >> value;
        else
          result %= value;
      }
      return goos::Object::make_integer(result);
    });
  }
  interpreter.register_form("rnd-int-count", [&](const auto&, auto&, const auto&) {
    ++random_calls;
    return goos::Object::make_integer(2);
  });
  const auto evaluate = [&](const std::string& source) {
    return interpreter.eval(interpreter.reader.read_from_string(source), environment);
  };
  const auto source = interpreter.reader.read_from_file(
      {std::string(MP_SOURCE_ROOT) +
       "/goal_src/jak2/multiplayer/system/traffic/pedestrian/mp-pedestrian-animation-sync.gc"});
  bool found = false;
  goos::for_each_in_list(source.as_pair()->cdr, [&](const goos::Object& form) {
    if (!form.is_pair() || !form.as_pair()->car.is_symbol("defun"))
      return;
    const auto& definition = form.as_pair()->cdr;
    if (!definition.as_pair()->car.is_symbol("get-pedestrian-animation-choice"))
      return;
    std::string lambda = "(define get-choice (lambda (ped salt count)";
    goos::for_each_in_list(definition.as_pair()->cdr.as_pair()->cdr,
                           [&](const auto& body) { lambda += " " + body.print(); });
    evaluate(lambda + "))");
    found = true;
  });
  ASSERT_TRUE(found);
  const auto declarations = interpreter.reader.read_from_file(
      {std::string(MP_SOURCE_ROOT) +
       "/goal_src/jak2/multiplayer/system/traffic/mp-traffic-types.gc"});
  std::vector<std::string> salts;
  goos::for_each_in_list(declarations.as_pair()->cdr, [&](const goos::Object& form) {
    if (!form.is_pair() || !form.as_pair()->car.is_symbol("defconstant"))
      return;
    const auto& definition = form.as_pair()->cdr;
    const auto name = definition.as_pair()->car.print();
    if (name.starts_with("PEDESTRIAN_") && name.ends_with("_ANIMATION_SEED")) {
      evaluate("(define " + name + " " + definition.as_pair()->cdr.as_pair()->car.print() + ")");
      salts.push_back(name);
    }
  });
  ASSERT_EQ(salts.size(), 4u);
  const auto choice = [&](const std::string& salt) {
    return evaluate("(get-choice 1 " + salt + " 3)").as_int();
  };
  for (uint8_t type : {1, 2, 3, 4, 5}) {
    object_type = type;
    for (const auto& salt : salts) {
      std::array<bool, 3> seen = {};
      for (uint32_t id = 1; id <= 128; ++id) {
        net_id = 0x11000000u + id;
        const auto expected = choice(salt);
        ASSERT_GE(expected, 0);
        ASSERT_LT(expected, 3);
        seen[expected] = true;
        net_id = 0x12000000u + (129 - id);
        choice(salt);
        net_id = 0x11000000u + id;
        EXPECT_EQ(choice(salt), expected);
      }
      EXPECT_TRUE(seen[0] && seen[1] && seen[2]);
    }
  }
  EXPECT_EQ(random_calls, 0);
  net_id = 0;
  EXPECT_EQ(choice(salts[0]), 2);
  EXPECT_EQ(random_calls, 1);
}

TEST(Jak2GoalBridge, NativeEventDefinitionsMatchEveryGoalIdAndPayloadSize) {
  TypeSystem types;
  types.add_builtin_types(GameVersion::Jak2);
  goos::Reader reader;
  std::vector<std::string> envelopes;
  goos::EnvironmentMap constants;
  const auto read = [&](const std::string& path) {
    return reader.read_from_file({std::string(MP_SOURCE_ROOT) + "/goal_src/jak2/" + path});
  };
  const auto load_types = [&](const std::string& path, const std::string& selected = "") {
    const auto source = read(path);
    goos::for_each_in_list(source.as_pair()->cdr, [&](const goos::Object& form) {
      if (!form.is_pair())
        return;
      const auto& definition = form.as_pair()->cdr;
      if (form.as_pair()->car.is_symbol("defconstant")) {
        const auto& value = definition.as_pair()->cdr.as_pair()->car;
        if (value.is_int())
          constants.set(definition.as_pair()->car.as_symbol(), value);
        return;
      }
      if (!form.as_pair()->car.is_symbol("deftype"))
        return;
      const auto name = definition.as_pair()->car.as_symbol().name_ptr;
      if (selected.empty() || selected == name) {
        parse_deftype(definition, &types, &constants);
        if (selected.empty() && std::string(name) != "mp-event" &&
            types.tc(types.make_typespec("mp-event"), types.make_typespec(name)))
          envelopes.emplace_back(name);
      }
    });
  };
  load_types("kernel/gcommon.gc", "vector");
  load_types("kernel/gcommon.gc", "inline-array-class");
  load_types("kernel/gkernel-h.gc", "time-frame");
  load_types("engine/math/quaternion-h.gc", "quaternion");
  load_types("multiplayer/player/mp-player-types.gc", "mp-player-appearance");
  load_types("multiplayer/core/preferences.gc", "multiplayer-preferences");
  EXPECT_EQ(types.lookup_type("multiplayer-preferences")->get_size_in_memory(),
            sizeof(MultiplayerPreferencesGOAL));
  for (const auto& [field, offset] : std::initializer_list<std::pair<const char*, size_t>>{
           {"appearance", offsetof(MultiplayerPreferencesGOAL, appearance)},
           {"player-name", offsetof(MultiplayerPreferencesGOAL, player_name)},
           {"network-port", offsetof(MultiplayerPreferencesGOAL, network_port)},
           {"respawn-delay-seconds", offsetof(MultiplayerPreferencesGOAL, respawn_delay_seconds)},
           {"session-player-limit", offsetof(MultiplayerPreferencesGOAL, session_player_limit)},
           {"friendly-fire", offsetof(MultiplayerPreferencesGOAL, friendly_fire)}}) {
    EXPECT_EQ(types.lookup_field_info("multiplayer-preferences", field).field.offset(), offset);
  }
  load_types("multiplayer/event/mp-event-h.gc");
  load_types("multiplayer/data/mp-world-h.gc", "mp-world-sync-state");
  EXPECT_EQ(types.lookup_type("mp-world-sync-state")->get_size_in_memory(),
            sizeof(MPWorldSyncStateGOAL));
  EXPECT_EQ(types.lookup_field_info("mp-world-sync-state", "respawn-delay-seconds").field.offset(),
            offsetof(MPWorldSyncStateGOAL, respawn_delay_seconds));
  EXPECT_EQ(types.lookup_field_info("mp-world-sync-state", "player-collision").field.offset(),
            offsetof(MPWorldSyncStateGOAL, player_collision));
  EXPECT_EQ(types.lookup_field_info("mp-world-sync-state", "friendly-fire").field.offset(),
            offsetof(MPWorldSyncStateGOAL, friendly_fire));
  EXPECT_EQ(types.lookup_field_info("mp-world-sync-state", "task-mask").field.offset(),
            offsetof(MPWorldSyncStateGOAL, task_mask));
  load_types("multiplayer/data/mp-replication-h.gc", "gungame-target-record");
  EXPECT_EQ(types.get_deref_info(types.make_inline_array_typespec("gungame-target-record")).stride,
            sizeof(GungameTargetRecordGOAL));
  EXPECT_EQ(types.lookup_field_info("gungame-target-record", "state").field.offset(),
            offsetof(GungameTargetRecordGOAL, state));
  load_types("multiplayer/data/mp-replication-h.gc", "gungame-target-record-array");
  const auto* target_array = types.lookup_type("gungame-target-record-array");
  const auto target_data = types.lookup_field_info("gungame-target-record-array", "data").field;
  EXPECT_EQ(target_data.alignment(), alignof(GungameTargetRecordGOAL));
  EXPECT_EQ(target_data.offset(), target_array->get_size_in_memory());
  EXPECT_TRUE(target_data.is_inline());
  EXPECT_TRUE(target_data.is_dynamic());
  EXPECT_EQ(target_data.type().print(), "gungame-target-record");
  EXPECT_EQ(types.lookup_method("gungame-target-record-array", "new").defined_in_type,
            "inline-array-class");
  load_types("multiplayer/data/mp-replication-h.gc", "gungame-state");
  EXPECT_EQ(types.lookup_type("gungame-state")->get_size_in_memory(), sizeof(GungameStateGOAL));
  EXPECT_EQ(types.lookup_field_info("gungame-state", "targets").field.offset(),
            offsetof(GungameStateGOAL, targets));
  EXPECT_EQ(types.lookup_field_info("gungame-state", "count").field.offset(),
            offsetof(GungameStateGOAL, count));

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

TEST(Jak2GoalBridge, GungameStateHandlersPreserveEventReplies) {
  ASSERT_TRUE(file_util::setup_project_path(fs::path(MP_SOURCE_ROOT), true));
  goos::Interpreter interpreter;
  auto reply = goos::Object::make_integer(123456);
  std::optional<goos::Object> forwarded;
  interpreter.register_form("gungame-event-handler",
                            [&](const auto&, auto&, const auto&) { return reply; });
  interpreter.register_form("return", [&](const auto&, auto& args, const auto& environment) {
    interpreter.eval_args(&args, environment);
    forwarded = args.unnamed.at(0);
    return *forwarded;
  });
  const auto source = interpreter.reader.read_from_file(
      {std::string(MP_SOURCE_ROOT) + "/goal_src/jak2/levels/gungame/gungame-obs.gc"});
  size_t direct_handlers = 0;
  size_t forwarding_handlers = 0;
  goos::for_each_in_list(source.as_pair()->cdr, [&](const goos::Object& form) {
    if (!form.is_pair() || !form.as_pair()->car.is_symbol("defstate"))
      return;
    std::vector<goos::Object> state;
    goos::for_each_in_list(form.as_pair()->cdr, [&](const auto& value) { state.push_back(value); });
    if (!state.at(1).as_pair()->car.is_symbol("training-manager"))
      return;
    SCOPED_TRACE(state.at(0).print());
    for (size_t index = 2; index + 1 < state.size(); ++index) {
      if (!state[index].is_symbol(":event"))
        continue;
      const auto& handler = state[index + 1];
      if (handler.is_symbol("gungame-event-handler")) {
        ++direct_handlers;
        break;
      }
      ASSERT_TRUE(handler.is_pair());
      ASSERT_TRUE(handler.as_pair()->car.is_symbol("behavior"));
      auto body = handler.as_pair()->cdr.as_pair()->cdr;
      if (body.as_pair()->car.as_pair()->car.is_symbol("local-vars"))
        body = body.as_pair()->cdr;
      const auto& forwarding = body.as_pair()->car;
      for (const auto& value : {goos::Object::make_integer(123456), goos::Object::make_integer(0),
                                interpreter.intern("#t"), interpreter.intern("#f")}) {
        reply = value;
        forwarded.reset();
        interpreter.eval(forwarding, interpreter.global_environment.as_env_ptr());
        if (value.is_symbol("#f")) {
          EXPECT_FALSE(forwarded.has_value());
        } else {
          ASSERT_TRUE(forwarded.has_value());
          EXPECT_EQ(*forwarded, value);
        }
      }
      ++forwarding_handlers;
      break;
    }
  });
  EXPECT_EQ(direct_handlers, 3u);
  EXPECT_EQ(forwarding_handlers, 4u);
}

TEST(Jak2GoalBridge, DirectionalAggregateHasCanonicalCompactAbi) {
  EXPECT_EQ(sizeof(MPReplicationPlayerIdentityGOAL), 279u);
  EXPECT_EQ(sizeof(MPReplicationPlayerVehicleGOAL), 94u);
  EXPECT_EQ(sizeof(MPReplicationPlayerGOAL), 544u);
  EXPECT_EQ(sizeof(MPReplicationEnemySetGOAL), 57360u);
  EXPECT_EQ(sizeof(MPReplicationPedestrianStateGOAL), 60u);
  EXPECT_EQ(sizeof(MPReplicationTrafficSetGOAL), 13328u);
  EXPECT_EQ(sizeof(MPReplicationBootstrapStateGOAL), 16454u);
  EXPECT_EQ(sizeof(MPReplicationFrameGOAL), 92512u);
  EXPECT_EQ(sizeof(MPReplicationStateGOAL), 195312u);
  EXPECT_EQ(offsetof(MPReplicationStateGOAL, local), 16u);
  EXPECT_EQ(offsetof(MPReplicationStateGOAL, remote), 92528u);
  EXPECT_EQ(offsetof(MPReplicationStateGOAL, outbound_events), 185056u);
  EXPECT_EQ(offsetof(MPReplicationStateGOAL, inbound_events), 190192u);
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

  auto& source = memory.at<MPPlayerAppearanceGOAL>(0x11500);
  for (size_t index = 0; index < kMPPlayerAppearanceSlotCount; ++index) {
    source.colors[index] = static_cast<uint32_t>(index + 1);
    source.strengths[index] = static_cast<float>(index) / 10.0f;
  }
  MPPlayerAppearance appearance = {};
  ASSERT_TRUE(multiplayer::jak2::bridge::read_appearance(0x11500, appearance));
  auto& preferences = preferences_state(memory);
  MultiplayerPreferences values;
  values.player_appearance = appearance;
  ASSERT_TRUE(multiplayer::jak2::bridge::write_preferences(0x11000, values));
  const auto& destination = preferences.appearance;
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
  state.local.traffic.pedestrian_count = 1;
  state.local.traffic.pedestrians[0].value.appearance_mask = 0xff7ffeb1;
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
  remote->selected_traffic.pedestrians.push_back(
      {.net_id = 0x11000001, .appearance_mask = 0xffbfeddf});
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
  EXPECT_EQ(state.remote.traffic.pedestrians[0].value.appearance_mask, 0xffbfeddfu);
  const auto local = mailbox.take_local_frame();
  ASSERT_TRUE(local);
  ASSERT_EQ(local->pedestrians.pedestrians.size(), 1u);
  EXPECT_EQ(local->pedestrians.pedestrians[0].appearance_mask, 0xff7ffeb1u);
}

TEST(Jak2GoalBridge, PedestrianAnimationSpeedUsesHorizontalPresentationVelocity) {
  using namespace multiplayer::jak2::application;
  GoalMemoryFixture memory;
  auto& state = replication_state(memory);
  ReplicationMailbox mailbox;
  const float unit = kGoalUnitsPerMeter;
  const float invalid = std::numeric_limits<float>::quiet_NaN();
  struct Sample {
    std::array<float, 3> velocity;
    bool valid;
    uint8_t expected;
  };
  const Sample samples[] = {
      {{0.0f, 0.0f, 0.0f}, true, 0},
      {{2.0f * unit, 0.0f, 0.0f}, true, 16},
      {{-6.0f * unit, 100.0f * unit, 8.0f * unit}, true, 80},
      {{100.0f * unit, 0.0f, 0.0f}, true, 255},
      {{10.0f * unit, 0.0f, 0.0f}, false, 0},
      {{invalid, 0.0f, 0.0f}, true, 0},
  };
  for (const auto& sample : samples) {
    auto frame = std::make_unique<RemoteReplicationFrame>();
    frame->selected_traffic.sequence = 1;
    frame->selected_traffic.pedestrians.push_back({.net_id = 1});
    frame->pedestrian_targets.push_back({.velocity = sample.velocity, .valid = sample.valid});
    mailbox.publish_remote_frame(std::move(frame));
    ASSERT_TRUE(multiplayer::jak2::bridge::exchange_state(0x12000, mailbox));
    ASSERT_EQ(state.remote.traffic.pedestrian_count, 1u);
    EXPECT_EQ(state.remote.traffic.pedestrians[0].value.travel_speed, sample.expected);
  }
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
  ScopedPreferencesRoot preferences_root;
  auto preferences = get_multiplayer_preferences();
  preferences.room_code.clear();
  ASSERT_TRUE(set_multiplayer_preferences(preferences));
  EXPECT_TRUE(multiplayer_preferences().room_code.empty());
  const std::string resolved = get_resolved_host_room_code();
  EXPECT_EQ(resolved.size(), multiplayer::platform::kMultiplayerRoomCodeLength);
  std::string normalized;
  EXPECT_TRUE(multiplayer::platform::normalize_room_code(resolved, normalized, false));
  EXPECT_EQ(resolved, normalized);
}

TEST(Jak2GoalBridge, RoomCodeResolutionPreservesConfiguredPreferences) {
  ScopedPreferencesRoot preferences_root;
  auto preferences = get_multiplayer_preferences();
  preferences.room_code = "ABC123";
  ASSERT_TRUE(set_multiplayer_preferences(preferences));
  EXPECT_EQ(get_resolved_host_room_code(), "ABC123");
  preferences.room_code = "xyz789";
  ASSERT_TRUE(set_multiplayer_preferences(preferences));
  EXPECT_EQ(get_resolved_host_room_code(), "XYZ789");
}

TEST(Jak2GoalBridge, PreferencesValidateBeforeMutation) {
  ScopedPreferencesRoot preferences_root;
  const auto original = get_multiplayer_preferences();
  const auto saved = file_util::read_text_file(preferences_root.path());
  const auto reject = [&](const MultiplayerPreferences& invalid) {
    EXPECT_FALSE(set_multiplayer_preferences(invalid));
    EXPECT_EQ(file_util::read_text_file(preferences_root.path()), saved);
    EXPECT_EQ(multiplayer_preferences().player_name, original.player_name);
    EXPECT_EQ(multiplayer_preferences().network_port, original.network_port);
  };
  auto invalid = original;
  invalid.player_name = "bad name";
  reject(invalid);
  invalid = original;
  invalid.room_code = "bad-code";
  reject(invalid);
  invalid = original;
  invalid.network_port = 1023;
  reject(invalid);
  invalid.network_port = multiplayer::platform::kMultiplayerDiscoveryPort;
  reject(invalid);
  invalid = original;
  invalid.session_player_limit = 1;
  reject(invalid);
  invalid.session_player_limit = kMPMaxPlayers + 1;
  reject(invalid);
  invalid = original;
  invalid.preferred_character = PlayerCharacter::UNKNOWN;
  reject(invalid);
  invalid = original;
  invalid.player_appearance.strengths[0] = 2.0f;
  reject(invalid);
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

TEST(Jak2GoalBridge, CharacterPreferenceBelongsToIdentityAndSurvivesOtherSettings) {
  ScopedPreferencesRoot preferences_root;
  const auto identity_path = preferences_root.path().parent_path() / "identity.json";
  auto preferences = get_multiplayer_preferences();
  preferences.preferred_character = PlayerCharacter::DAXTER;
  ASSERT_TRUE(set_multiplayer_preferences(preferences));
  auto identity = parse_commented_json(file_util::read_text_file(identity_path), "identity.json");
  EXPECT_EQ(identity["preferred_character"], static_cast<uint8_t>(PlayerCharacter::DAXTER));
  preferences.preferred_character = PlayerCharacter::UNKNOWN;
  EXPECT_FALSE(set_multiplayer_preferences(preferences));

  auto settings =
      parse_commented_json(file_util::read_text_file(preferences_root.path()), "preferences.json");
  settings["session_characters"] = {1, 1, 1, 1, 1, 1, 1, 1};
  file_util::write_text_file(preferences_root.path(), settings.dump(2));
  load_multiplayer_preferences();
  EXPECT_EQ(get_multiplayer_preferences().preferred_character, PlayerCharacter::DAXTER);
  settings =
      parse_commented_json(file_util::read_text_file(preferences_root.path()), "preferences.json");
  EXPECT_FALSE(settings.contains("session_characters"));

  preferences = get_multiplayer_preferences();
  preferences.player_name = "Player2";
  ASSERT_TRUE(set_multiplayer_preferences(preferences));
  reset_multiplayer_preferences();
  load_multiplayer_preferences();
  EXPECT_EQ(get_multiplayer_preferences().preferred_character, PlayerCharacter::DAXTER);
  identity = parse_commented_json(file_util::read_text_file(identity_path), "identity.json");
  EXPECT_EQ(identity["display_name"], "Player2");
  identity["preferred_character"] = static_cast<uint8_t>(PlayerCharacter::JAK);
  file_util::write_text_file(identity_path, identity.dump(2));
  preferences = get_multiplayer_preferences();
  preferences.automatic_port_mapping = false;
  ASSERT_TRUE(set_multiplayer_preferences(preferences));
  EXPECT_EQ(get_multiplayer_preferences().preferred_character, PlayerCharacter::JAK);
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
  frame->player_lifecycles[1] = 4;
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
  ASSERT_TRUE(replication.players().apply(
      player, {.sequence = 1, .source = {.authenticated_player_id = 1}, .received_at_ms = 100}));
  replication.expire(2101);
  auto frame = std::make_unique<application::RemoteReplicationFrame>();
  frame->players = replication.players().players();
  frame->identities = replication.players().identities();
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

TEST(Jak2GoalBridge, RemoteEnemiesPreserveFullCapacityAcrossPlayers) {
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

TEST(Jak2GoalBridge, EveryGungameCourseFitsLengthAndTargetNumberContracts) {
  goos::Reader reader;
  const auto source = reader.read_from_file(
      {std::string(MP_SOURCE_ROOT) + "/goal_src/jak2/levels/gungame/gungame-data.gc"});
  const std::map<std::string, size_t> expected = {{"*red-training-path-global-info*", 137},
                                                  {"*yellow-training-path-global-info*", 191},
                                                  {"*blue-training-path-global-info*", 209},
                                                  {"*peace-training-path-global-info*", 109}};
  size_t courses = 0;
  int64_t maximum_number = 0;
  const auto elements = [](const goos::Object& list) {
    std::vector<goos::Object> values;
    goos::for_each_in_list(list, [&](const auto& value) { values.push_back(value); });
    return values;
  };
  goos::for_each_in_list(source.as_pair()->cdr, [&](const goos::Object& form) {
    if (!form.is_pair() || !form.as_pair()->car.is_symbol("define"))
      return;
    const auto definition = elements(form);
    const auto found = expected.find(definition[1].as_symbol().name_ptr);
    if (found == expected.end())
      return;
    SCOPED_TRACE(found->first);
    const auto entries = elements(definition[2]);
    ASSERT_GE(entries.size(), 5u);
    EXPECT_EQ(entries.size() - 5, found->second);
    EXPECT_LE(entries.size() - 5, UINT16_MAX);
    std::unordered_set<int64_t> numbers;
    for (size_t index = 5; index < entries.size(); ++index) {
      const auto fields = elements(entries[index]);
      int64_t number = 0;
      for (size_t field = 3; field + 1 < fields.size(); ++field) {
        if (fields[field].is_symbol(":num"))
          number = fields[field + 1].as_int();
      }
      EXPECT_GE(number, 0);
      EXPECT_LE(number, UINT16_MAX);
      EXPECT_TRUE(numbers.insert(number).second);
      maximum_number = std::max(maximum_number, number);
    }
    ++courses;
  });
  EXPECT_EQ(courses, 4u);
  EXPECT_EQ(maximum_number, 3820);
}

TEST(Jak2GoalBridge, GungameExchangeOwnsCopiedTargetsAndPreservesDestinationDescriptor) {
  using namespace multiplayer::jak2;
  GoalMemoryFixture memory;
  auto& state = replication_state(memory);
  state.local.gungame = {.run_id = 5,
                         .score = 1300,
                         .elapsed_time = -120,
                         .targets = 0x10000,
                         .count = 209,
                         .capacity = 209,
                         .course_id = 3,
                         .phase = 3,
                         .end_door = 1,
                         .open_end = 1};
  state.remote.gungame.targets = 0x11000;
  state.remote.gungame.capacity = 209;
  auto* targets = &memory.at<GungameTargetRecordGOAL>(0x10000);
  for (size_t index = 0; index < 209; ++index)
    targets[index] = {.spawn_time = index % 3 ? static_cast<int32_t>(index * 300) : 0,
                      .state = static_cast<uint8_t>(index % 3)};
  application::ReplicationMailbox mailbox;
  ASSERT_TRUE(bridge::exchange_state(0x12000, mailbox));
  const auto local = mailbox.take_local_frame();
  ASSERT_TRUE(local);
  ASSERT_TRUE(local->gungame);
  EXPECT_EQ(local->gungame->targets.size(), 209u);
  EXPECT_EQ(local->gungame->targets[128].state, core::GungameTargetState::BROKEN);
  EXPECT_EQ(local->gungame->targets[128].spawn_time, 38400);
  targets[128] = {};
  EXPECT_EQ(local->gungame->targets[128].spawn_time, 38400);
  EXPECT_EQ(local->gungame->targets[128].state, core::GungameTargetState::BROKEN);
  const auto publish = [&] {
    auto frame = std::make_unique<application::RemoteReplicationFrame>();
    frame->gungame = *local->gungame;
    frame->gungame.sequence = 7;
    mailbox.publish_remote_frame(std::move(frame));
    EXPECT_TRUE(bridge::exchange_state(0x12000, mailbox));
  };
  publish();
  EXPECT_EQ(state.remote.gungame.targets, 0x11000u);
  EXPECT_EQ(state.remote.gungame.capacity, 209u);
  EXPECT_EQ(state.remote.gungame.sequence, 7u);
  EXPECT_EQ(state.remote.gungame.count, 209u);
  EXPECT_EQ(state.remote.gungame.elapsed_time, -120);
  EXPECT_EQ(
      memory.at<GungameTargetRecordGOAL>(0x11000 + 128 * sizeof(GungameTargetRecordGOAL)).state,
      2u);
  state.remote.gungame.capacity = 208;
  memory.at<uint8_t>(0x11000) = 0xee;
  publish();
  EXPECT_EQ(state.remote.gungame.sequence, 0u);
  EXPECT_EQ(memory.at<uint8_t>(0x11000), 0xee);
  state.remote.gungame.capacity = 209;
  publish();
  EXPECT_EQ(state.remote.gungame.sequence, 7u);
  state.local.gungame.targets = 0x10800;
  state.remote.gungame.targets = 0x11800;
  std::memset(targets, 3, 209 * sizeof(GungameTargetRecordGOAL));
  auto* moved_targets = &memory.at<GungameTargetRecordGOAL>(0x10800);
  for (size_t index = 0; index < 209; ++index)
    moved_targets[index] = {.spawn_time = 300, .state = 1};
  std::memset(&memory.at<GungameTargetRecordGOAL>(0x11000), 0xee,
              209 * sizeof(GungameTargetRecordGOAL));
  publish();
  const auto moved_local = mailbox.take_local_frame();
  ASSERT_TRUE(moved_local);
  ASSERT_TRUE(moved_local->gungame);
  EXPECT_EQ(moved_local->gungame->targets.size(), 209u);
  EXPECT_EQ(moved_local->gungame->targets[128].state, core::GungameTargetState::SPAWNED);
  EXPECT_EQ(state.remote.gungame.targets, 0x11800u);
  EXPECT_EQ(
      memory.at<GungameTargetRecordGOAL>(0x11800 + 128 * sizeof(GungameTargetRecordGOAL)).state,
      2u);
  EXPECT_EQ(
      memory.at<GungameTargetRecordGOAL>(0x11000 + 128 * sizeof(GungameTargetRecordGOAL)).state,
      0xee);
  state.remote.gungame.targets = 0;
  state.remote.gungame.capacity = 0;
  publish();
  EXPECT_EQ(state.remote.gungame.sequence, 0u);
}

TEST(Jak2GoalBridge, InvalidGungameCaptureDoesNotBlockOtherDomainsOrEvents) {
  using namespace multiplayer::jak2;
  GoalMemoryFixture memory;
  auto& state = replication_state(memory);
  state.local.gungame = {
      .run_id = 1, .targets = 0x10000, .count = 209, .capacity = 208, .course_id = 3, .phase = 3};
  state.local.traffic.pedestrian_count = 1;
  state.local.traffic.pedestrians[0].value.net_id = 0x10000001;
  state.outbound_event_count = 1;
  state.outbound_events[0] = {.etype = 44, .source_player_id = 0, .payload_size = 8};
  application::ReplicationMailbox mailbox;
  float position = 10.0f;
  const auto capture = [&](bool valid_gungame) {
    state.local.players[0].transform.position[0] = ++position;
    state.local.traffic.pedestrians[0].value.position[0] = position;
    EXPECT_TRUE(bridge::exchange_state(0x12000, mailbox));
    const auto frame = mailbox.take_local_frame();
    ASSERT_TRUE(frame);
    EXPECT_FLOAT_EQ(frame->players[0].position[0], position);
    ASSERT_EQ(frame->pedestrians.pedestrians.size(), 1u);
    EXPECT_FLOAT_EQ(frame->pedestrians.pedestrians[0].position[0], position);
    EXPECT_EQ(frame->gungame.has_value(), valid_gungame);
  };
  capture(false);
  EXPECT_EQ(state.outbound_event_count, 0u);
  const auto events = mailbox.take_outbound_events(64);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].event_id, 44u);
  state.local.gungame.capacity = 209;
  for (const uint32_t address :
       {0u, 0xfffu, 0x10001u, static_cast<uint32_t>(EE_MAIN_MEM_SIZE - 8)}) {
    state.local.gungame.targets = address;
    capture(false);
  }
  state.local.gungame.targets = 0x10000;
  auto& target =
      memory.at<GungameTargetRecordGOAL>(0x10000 + 208 * sizeof(GungameTargetRecordGOAL));
  target.state = 3;
  capture(false);
  target = {.spawn_time = 9000, .state = 0};
  capture(false);
  target.spawn_time = 0;
  capture(true);
}

TEST(Jak2GoalBridge, PlayerRulesPersistAndResetToOff) {
  ScopedPreferencesRoot preferences_root;
  EXPECT_FALSE(multiplayer_preferences().player_collision);
  EXPECT_FALSE(multiplayer_preferences().friendly_fire);
  auto preferences = get_multiplayer_preferences();
  preferences.player_collision = true;
  preferences.friendly_fire = true;
  ASSERT_TRUE(set_multiplayer_preferences(preferences));
  load_multiplayer_preferences();
  EXPECT_TRUE(multiplayer_preferences().player_collision);
  EXPECT_TRUE(multiplayer_preferences().friendly_fire);
  const auto json =
      parse_commented_json(file_util::read_text_file(preferences_root.path()), "preferences.json");
  EXPECT_EQ(json.at("player_collision"), true);
  EXPECT_EQ(json.at("friendly_fire"), true);
  const auto malformed =
      parse_multiplayer_preferences(R"({"player_collision": "true", "friendly_fire": 1})");
  EXPECT_FALSE(malformed.player_collision);
  EXPECT_FALSE(malformed.friendly_fire);
  reset_multiplayer_preferences();
  load_multiplayer_preferences();
  EXPECT_FALSE(multiplayer_preferences().player_collision);
  EXPECT_FALSE(multiplayer_preferences().friendly_fire);
}

TEST(Jak2GoalBridge, RespawnDelayValidatesPersistsAndResets) {
  ScopedPreferencesRoot preferences_root;
  EXPECT_EQ(get_multiplayer_preferences().respawn_delay_seconds, 20);
  for (const uint16_t delay : {0, 35, 65535}) {
    auto preferences = get_multiplayer_preferences();
    preferences.respawn_delay_seconds = delay;
    ASSERT_TRUE(set_multiplayer_preferences(preferences));
    load_multiplayer_preferences();
    EXPECT_EQ(get_multiplayer_preferences().respawn_delay_seconds, delay);
    const auto root = parse_commented_json(file_util::read_text_file(preferences_root.path()),
                                           "preferences.json");
    EXPECT_EQ(root.at("respawn_delay_seconds"), delay);
    EXPECT_EQ(parse_multiplayer_preferences(root.dump()).respawn_delay_seconds, delay);
  }
  for (const std::string_view invalid : {"-1", "1.5", "\"35\"", "true", "65536", "4294967296"}) {
    EXPECT_EQ(
        parse_multiplayer_preferences("{\"respawn_delay_seconds\":" + std::string(invalid) + "}")
            .respawn_delay_seconds,
        multiplayer::jak2::core::kDefaultRespawnDelaySeconds);
  }
  EXPECT_EQ(parse_multiplayer_preferences("{}").respawn_delay_seconds,
            multiplayer::jak2::core::kDefaultRespawnDelaySeconds);
  reset_multiplayer_preferences();
  load_multiplayer_preferences();
  EXPECT_EQ(get_multiplayer_preferences().respawn_delay_seconds, 20);
}

TEST(Jak2GoalBridge, PreferencesSnapshotRoundTripsAndPersistsTogether) {
  ScopedPreferencesRoot preferences_root;
  GoalMemoryFixture memory;
  auto& state = preferences_state(memory);
  auto preferences = get_multiplayer_preferences();
  preferences.player_name = "Player2";
  preferences.room_code = "xyz789";
  preferences.network_port = 26212;
  preferences.respawn_delay_seconds = 65535;
  preferences.session_player_limit = kMPMaxPlayers;
  preferences.preferred_character = PlayerCharacter::DAXTER;
  preferences.automatic_port_mapping = false;
  preferences.player_collision = true;
  preferences.friendly_fire = true;
  preferences.player_appearance = get_default_player_appearance(0x123456);
  std::memset(memory.at<String>(state.player_name).data(), 'x', 16);
  ASSERT_TRUE(multiplayer::jak2::bridge::write_preferences(0x11000, preferences));
  EXPECT_EQ(state.respawn_delay_seconds, 65535);
  EXPECT_EQ(state.session_player_limit, kMPMaxPlayers);
  EXPECT_EQ(state.preferred_character, static_cast<uint8_t>(PlayerCharacter::DAXTER));
  EXPECT_EQ(state.automatic_port_mapping, 0);
  EXPECT_EQ(state.player_collision, 1);
  EXPECT_EQ(state.friendly_fire, 1);

  MultiplayerPreferences restored;
  ASSERT_TRUE(multiplayer::jak2::bridge::read_preferences(0x11000, restored));
  EXPECT_EQ(restored.player_name, preferences.player_name);
  EXPECT_EQ(restored.network_port, preferences.network_port);
  EXPECT_EQ(restored.player_appearance.colors, preferences.player_appearance.colors);
  EXPECT_EQ(restored.player_appearance.strengths, preferences.player_appearance.strengths);
  ASSERT_TRUE(set_multiplayer_preferences(restored));
  load_multiplayer_preferences();
  const auto saved = get_multiplayer_preferences();
  ASSERT_TRUE(multiplayer::jak2::bridge::write_preferences(0x11000, saved));
  ASSERT_TRUE(multiplayer::jak2::bridge::read_preferences(0x11000, restored));
  EXPECT_EQ(restored.player_name, "Player2");
  EXPECT_EQ(restored.room_code, "XYZ789");
  EXPECT_EQ(restored.network_port, 26212);
  EXPECT_EQ(restored.respawn_delay_seconds, 65535);
  EXPECT_EQ(restored.session_player_limit, kMPMaxPlayers);
  EXPECT_EQ(restored.preferred_character, PlayerCharacter::DAXTER);
  EXPECT_FALSE(restored.automatic_port_mapping);
  EXPECT_TRUE(restored.player_collision);
  EXPECT_TRUE(restored.friendly_fire);
  EXPECT_EQ(restored.player_appearance.colors, preferences.player_appearance.colors);
  EXPECT_EQ(restored.player_appearance.strengths, preferences.player_appearance.strengths);
  const auto root =
      parse_commented_json(file_util::read_text_file(preferences_root.path()), "preferences.json");
  EXPECT_FALSE(root.contains("preferred_character"));
}

TEST(Jak2GoalBridge, PreferencesSnapshotRejectsInvalidBuffersAndFlags) {
  GoalMemoryFixture memory;
  auto& state = preferences_state(memory);
  MultiplayerPreferences values;
  values.player_name = "Player2";
  values.room_code = "ABC123";
  ASSERT_TRUE(multiplayer::jak2::bridge::write_preferences(0x11000, values));
  for (uint8_t* flag :
       {&state.automatic_port_mapping, &state.player_collision, &state.friendly_fire}) {
    const auto saved = *flag;
    *flag = 2;
    EXPECT_FALSE(multiplayer::jak2::bridge::read_preferences(0x11000, values));
    *flag = saved;
  }
  EXPECT_FALSE(multiplayer::jak2::bridge::read_preferences(0, values));
  EXPECT_FALSE(multiplayer::jak2::bridge::write_preferences(EE_MAIN_MEM_SIZE - 1, values));
  memory.at<String>(state.room_code).len = 2;
  values.network_port = 30000;
  EXPECT_FALSE(multiplayer::jak2::bridge::write_preferences(0x11000, values));
  EXPECT_EQ(state.network_port, multiplayer::platform::kDefaultMultiplayerPort);
  EXPECT_STREQ(memory.at<String>(state.player_name).data(), "Player2");
  memory.at<String>(state.room_code).len = 6;
  state.player_name = 0;
  EXPECT_FALSE(multiplayer::jak2::bridge::read_preferences(0x11000, values));
  EXPECT_FALSE(multiplayer::jak2::bridge::write_preferences(0x11000, values));
}

TEST(Jak2GoalBridge, PlayerRulesCrossTheWorldBridge) {
  GoalMemoryFixture memory;
  auto& state = replication_state(memory);
  state.local.world.respawn_delay_seconds = 0;
  state.local.world.player_collision = 1;
  state.local.world.friendly_fire = 1;
  multiplayer::jak2::application::ReplicationMailbox mailbox;
  ASSERT_TRUE(multiplayer::jak2::bridge::exchange_state(0x12000, mailbox));
  const auto local = mailbox.take_local_frame();
  ASSERT_TRUE(local);
  EXPECT_EQ(local->world.respawn_delay_seconds, 0);
  EXPECT_TRUE(local->world.player_collision);
  EXPECT_TRUE(local->world.friendly_fire);
  auto remote = std::make_unique<multiplayer::jak2::application::RemoteReplicationFrame>();
  remote->world = local->world;
  remote->world.respawn_delay_seconds = 65535;
  remote->world.friendly_fire = false;
  mailbox.publish_remote_frame(std::move(remote));
  ASSERT_TRUE(multiplayer::jak2::bridge::exchange_state(0x12000, mailbox));
  EXPECT_EQ(state.remote.world.respawn_delay_seconds, 65535);
  EXPECT_EQ(state.remote.world.player_collision, 1);
  EXPECT_EQ(state.remote.world.friendly_fire, 0);
}
