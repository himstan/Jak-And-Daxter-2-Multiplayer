#include "custom_audio_stream.h"

#include <cmath>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "game/kernel/common/custom_audio.h"
#include "game/overlord/jak2/ssound.h"
#include "game/overlord/jak2/vag.h"

namespace jak2 {
namespace {

struct RegisteredStream {
  std::string full_path;
};

struct StreamInstance {
  std::string key;
  std::unique_ptr<custom_audio::Source> source;
  SoundParams params{};
  CustomAudioStreamStatus status = CustomAudioStreamStatus::READY;
  CustomAudioStreamStatus last_reported_status = CustomAudioStreamStatus::NOT_CUSTOM;
  u32 start_request_count = 0;
  s32 last_logged_position_second = -1;
  bool has_spatial_params = false;
  bool queued = false;
  bool paused = false;
};

std::mutex g_stream_mutex;
std::unordered_map<std::string, RegisteredStream> g_registered_streams;
std::unordered_map<s32, StreamInstance> g_stream_instances;

bool is_safe_relative_path(const fs::path& path) {
  if (path.empty() || path.is_absolute()) {
    return false;
  }
  for (const auto& component : path) {
    if (component == "..") {
      return false;
    }
  }
  return true;
}

StreamInstance& prepare_instance(const s32 id, const std::string& key) {
  auto& instance = g_stream_instances[id];
  if (instance.key != key) {
    instance = {};
    instance.key = key;
    instance.params.volume = 0x400;
    instance.params.fo_min = 5;
    instance.params.fo_max = 30;
    instance.params.fo_curve = 2;
  }
  return instance;
}

void refresh_status(StreamInstance& instance) {
  if (instance.status == CustomAudioStreamStatus::ACTIVE && !instance.paused && instance.source &&
      instance.source->is_at_end()) {
    instance.status = CustomAudioStreamStatus::FINISHED;
    instance.source.reset();
  }
}

void apply_spatial_volume(StreamInstance& instance) {
  if (!instance.source || !instance.has_spatial_params ||
      instance.status != CustomAudioStreamStatus::ACTIVE) {
    return;
  }

  const s32 base_volume = (instance.params.volume * MasterVolume[2]) >> 10;
  const auto [left, right] =
      CalculateSpatializedVolume(&instance.params.trans, base_volume, instance.params.fo_curve,
                                 instance.params.fo_min, instance.params.fo_max);
  instance.source->set_stereo_volume(left, right);
}

void apply_params(StreamInstance& instance, const SoundParams& params) {
  const u32 mask = params.mask;
  if (mask & 0x1) {
    instance.params.volume = params.volume;
  }
  if (mask & 0x20) {
    instance.params.trans = params.trans;
    instance.has_spatial_params = true;
  }
  if (mask & 0x40) {
    instance.params.fo_min = params.fo_min;
  }
  if (mask & 0x80) {
    instance.params.fo_max = params.fo_max;
  }
  if (mask & 0x100) {
    instance.params.fo_curve = params.fo_curve;
  }
  instance.params.mask |= mask;
  apply_spatial_volume(instance);
}

}  // namespace

bool RegisterCustomAudioStream(const char* key, const char* relative_path) {
  if (!key || !relative_path || !key[0] || !relative_path[0] || std::strlen(key) >= 48) {
    return false;
  }

  const fs::path relative(relative_path);
  if (!is_safe_relative_path(relative)) {
    lg::warn("Rejected unsafe custom audio path '{}'", relative_path);
    return false;
  }

  const fs::path full_path =
      file_util::get_jak_project_dir() / "custom_assets" / "jak2" / "audio" / relative;
  if (!file_util::file_exists(full_path.string())) {
    lg::warn("Custom audio '{}' is unavailable at '{}'", key, full_path.string());
    return false;
  }

  std::lock_guard lock(g_stream_mutex);
  g_registered_streams[std::string(key)] = {full_path.string()};
  lg::info("[CUSTOM_SPATIAL_AUDIO] registered key='{}' path='{}'", key, full_path.string());
  return true;
}

CustomAudioStreamStatus GetCustomAudioStreamStatus(const char* key, const s32 id) {
  if (!key || !key[0] || !id) {
    return CustomAudioStreamStatus::NOT_CUSTOM;
  }

  std::lock_guard lock(g_stream_mutex);
  const auto registration = g_registered_streams.find(key);
  if (registration == g_registered_streams.end()) {
    return CustomAudioStreamStatus::NOT_CUSTOM;
  }

  const auto entry = g_stream_instances.find(id);
  if (entry == g_stream_instances.end() || entry->second.key != registration->first) {
    return CustomAudioStreamStatus::PENDING;
  }

  auto& instance = entry->second;
  refresh_status(instance);
  if (instance.last_reported_status != instance.status) {
    instance.last_reported_status = instance.status;
  }
  return instance.status;
}

s32 GetCustomAudioStreamPosition(const s32 id) {
  std::lock_guard lock(g_stream_mutex);
  const auto entry = g_stream_instances.find(id);
  if (entry == g_stream_instances.end()) {
    return -1;
  }

  auto& instance = entry->second;
  refresh_status(instance);
  if (!instance.source || instance.status == CustomAudioStreamStatus::FINISHED) {
    return -1;
  }
  const float position = instance.source->position_seconds();
  if (const s32 position_second = static_cast<s32>(std::floor(position)); position >= 0.0f && position_second != instance.last_logged_position_second) {
    instance.last_logged_position_second = position_second;
  }
  return position < 0.0f ? -1 : static_cast<s32>(std::floor(position * 30.0f));
}

u32 UpdateCustomAudioStreamQueue(const char* const* keys, const u32* ids, const u32 count) {
  std::lock_guard lock(g_stream_mutex);
  std::unordered_set<s32> selected_ids;
  u32 custom_slot_mask = 0;

  if (keys && ids) {
    for (u32 slot = 0; slot < count; ++slot) {
      const char* key = keys[slot];
      if (!key || !key[0] || !ids[slot]) {
        continue;
      }

      const auto registration = g_registered_streams.find(key);
      if (registration == g_registered_streams.end()) {
        continue;
      }

      const s32 id = static_cast<s32>(ids[slot]);
      if (slot < 32) {
        custom_slot_mask |= 1u << slot;
      }
      selected_ids.insert(id);
      if (auto& instance = prepare_instance(id, registration->first); !instance.queued) {
        instance.queued = true;
      }
    }
  }

  for (auto entry = g_stream_instances.begin(); entry != g_stream_instances.end();) {
    const s32 id = entry->first;
    auto& instance = entry->second;
    if (!instance.queued || selected_ids.contains(id)) {
      ++entry;
      continue;
    }

    instance.queued = false;
    if (instance.status == CustomAudioStreamStatus::READY) {
      entry = g_stream_instances.erase(entry);
      continue;
    }
    if (instance.status == CustomAudioStreamStatus::ACTIVE ||
        instance.status == CustomAudioStreamStatus::PENDING) {
      if (instance.source) {
        instance.source->stop();
        instance.source.reset();
      }
      instance.paused = false;
      instance.status = CustomAudioStreamStatus::FINISHED;
    }
    ++entry;
  }

  return custom_slot_mask;
}

bool StartCustomAudioStream(const char* key, s32 id) {
  if (!key || !id) {
    return false;
  }

  std::lock_guard lock(g_stream_mutex);
  const auto registration = g_registered_streams.find(key);
  if (registration == g_registered_streams.end()) {
    return false;
  }

  const auto entry = g_stream_instances.find(id);
  if (entry == g_stream_instances.end() || entry->second.key != registration->first ||
      !entry->second.queued) {
    lg::warn("[CUSTOM_SPATIAL_AUDIO] rejected unqueued start key='{}' id={}", key, id);
    return true;
  }

  auto& instance = entry->second;
  instance.start_request_count++;
  if (instance.status == CustomAudioStreamStatus::ACTIVE || instance.status == CustomAudioStreamStatus::PENDING) {
    return true;
  }

  instance.status = CustomAudioStreamStatus::PENDING;
  instance.source = std::make_unique<custom_audio::Source>();
  if (!instance.source->start(registration->second.full_path)) {
    lg::warn("Failed to decode custom audio '{}' at '{}'", key, registration->second.full_path);
    instance.source.reset();
    instance.status = CustomAudioStreamStatus::FINISHED;
    return true;
  }

  instance.paused = false;
  instance.status = CustomAudioStreamStatus::ACTIVE;
  apply_spatial_volume(instance);
  return true;
}

bool StopCustomAudioStream(const char* key, s32 id) {
  std::lock_guard lock(g_stream_mutex);
  if (key && key[0] && !g_registered_streams.contains(key)) {
    return false;
  }

  const auto entry = g_stream_instances.find(id);
  if (entry == g_stream_instances.end()) {
    const bool registered_key = key && key[0] && g_registered_streams.contains(key);
    if (registered_key) {
      lg::info("stop-request-without-instance key='{}' id={}", key, id);
    }
    return registered_key;
  }
  if ((!key || !key[0]) && entry->second.status == CustomAudioStreamStatus::FINISHED) {
    return false;
  }
  lg::info("stop-request requested-key='{}' id={}", key && key[0] ? key : "<by-id>", id);
  if (entry->second.source) {
    entry->second.source->stop();
    entry->second.source.reset();
  }
  entry->second.paused = false;
  entry->second.status = CustomAudioStreamStatus::FINISHED;
  return true;
}

bool PauseCustomAudioStream(const s32 id) {
  std::lock_guard lock(g_stream_mutex);
  const auto entry = g_stream_instances.find(id);
  if (entry == g_stream_instances.end() ||
      entry->second.status == CustomAudioStreamStatus::FINISHED) {
    return false;
  }
  if (entry->second.source && entry->second.status == CustomAudioStreamStatus::ACTIVE) {
    entry->second.source->pause();
    entry->second.paused = true;
  }
  return true;
}

bool ContinueCustomAudioStream(const s32 id) {
  std::lock_guard lock(g_stream_mutex);
  const auto entry = g_stream_instances.find(id);
  if (entry == g_stream_instances.end() ||
      entry->second.status == CustomAudioStreamStatus::FINISHED) {
    return false;
  }
  if (entry->second.source && entry->second.status == CustomAudioStreamStatus::ACTIVE &&
      entry->second.paused) {
    entry->second.source->resume();
    entry->second.paused = false;
    apply_spatial_volume(entry->second);
  }
  return true;
}

bool SetCustomAudioStreamParams(const s32 id, const SoundParams& params) {
  std::lock_guard lock(g_stream_mutex);
  const auto entry = g_stream_instances.find(id);
  if (entry == g_stream_instances.end() ||
      entry->second.status == CustomAudioStreamStatus::FINISHED) {
    return false;
  }
  apply_params(entry->second, params);
  return true;
}

void UpdateCustomAudioStreams() {
  std::lock_guard lock(g_stream_mutex);
  for (auto& instance : g_stream_instances | std::views::values) {
    refresh_status(instance);
    apply_spatial_volume(instance);
  }
}

void PauseCustomAudioStreams() {
  std::lock_guard lock(g_stream_mutex);
  for (auto& instance : g_stream_instances | std::views::values) {
    if (instance.source && instance.status == CustomAudioStreamStatus::ACTIVE && !instance.paused) {
      instance.source->pause();
      instance.paused = true;
    }
  }
}

void ContinueCustomAudioStreams() {
  std::lock_guard lock(g_stream_mutex);
  for (auto& instance : g_stream_instances | std::views::values) {
    if (instance.source && instance.status == CustomAudioStreamStatus::ACTIVE && instance.paused) {
      instance.source->resume();
      instance.paused = false;
      apply_spatial_volume(instance);
    }
  }
}

void StopCustomAudioStreams() {
  std::lock_guard lock(g_stream_mutex);
  for (auto& instance : g_stream_instances | std::views::values) {
    if (instance.source) {
      instance.source->stop();
      instance.source.reset();
    }
    instance.paused = false;
    instance.status = CustomAudioStreamStatus::FINISHED;
  }
}

}  // namespace jak2
