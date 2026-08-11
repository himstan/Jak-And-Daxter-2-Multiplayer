#include "custom_audio_stream.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
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

const char* status_name(CustomAudioStreamStatus status) {
  switch (status) {
    case CustomAudioStreamStatus::NOT_CUSTOM:
      return "not-custom";
    case CustomAudioStreamStatus::READY:
      return "ready";
    case CustomAudioStreamStatus::PENDING:
      return "pending";
    case CustomAudioStreamStatus::ACTIVE:
      return "active";
    case CustomAudioStreamStatus::FINISHED:
      return "finished";
  }
  return "unknown";
}

void log_stream_state(const char* event, s32 id, const StreamInstance& instance) {
  const bool has_source = instance.source != nullptr;
  const bool playing = has_source && instance.source->is_playing();
  const bool at_end = has_source && instance.source->is_at_end();
  const float position = has_source ? instance.source->position_seconds() : -1.0f;
  lg::info(
      "[CUSTOM_SPATIAL_AUDIO] {} key='{}' id={} status={} source={} playing={} at-end={} "
      "queued={} paused={} spatial={} position={:.3f}",
      event, instance.key, id, status_name(instance.status), has_source, playing, at_end,
      instance.queued, instance.paused, instance.has_spatial_params, position);
}

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

StreamInstance& prepare_instance(s32 id, const std::string& key) {
  auto& instance = g_stream_instances[id];
  if (instance.key != key) {
    if (!instance.key.empty()) {
      log_stream_state("instance-replaced", id, instance);
    }
    instance = {};
    instance.key = key;
    instance.params.volume = 0x400;
    instance.params.fo_min = 5;
    instance.params.fo_max = 30;
    instance.params.fo_curve = 2;
    log_stream_state("instance-created", id, instance);
  }
  return instance;
}

void refresh_status(s32 id, StreamInstance& instance) {
  if (instance.status == CustomAudioStreamStatus::ACTIVE && !instance.paused && instance.source &&
      instance.source->is_at_end()) {
    log_stream_state("end-of-stream", id, instance);
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
  const auto volume =
      CalculateSpatializedVolume(&instance.params.trans, base_volume, instance.params.fo_curve,
                                 instance.params.fo_min, instance.params.fo_max);
  instance.source->set_stereo_volume(volume.left, volume.right);
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

  std::lock_guard<std::mutex> lock(g_stream_mutex);
  g_registered_streams[std::string(key)] = {full_path.string()};
  lg::info("[CUSTOM_SPATIAL_AUDIO] registered key='{}' path='{}'", key, full_path.string());
  return true;
}

CustomAudioStreamStatus GetCustomAudioStreamStatus(const char* key, s32 id) {
  if (!key || !key[0] || !id) {
    return CustomAudioStreamStatus::NOT_CUSTOM;
  }

  std::lock_guard<std::mutex> lock(g_stream_mutex);
  const auto registration = g_registered_streams.find(key);
  if (registration == g_registered_streams.end()) {
    return CustomAudioStreamStatus::NOT_CUSTOM;
  }

  const auto entry = g_stream_instances.find(id);
  if (entry == g_stream_instances.end() || entry->second.key != registration->first) {
    return CustomAudioStreamStatus::PENDING;
  }

  auto& instance = entry->second;
  refresh_status(id, instance);
  if (instance.last_reported_status != instance.status) {
    log_stream_state("status-reported-to-goal", id, instance);
    instance.last_reported_status = instance.status;
  }
  return instance.status;
}

s32 GetCustomAudioStreamPosition(s32 id) {
  std::lock_guard<std::mutex> lock(g_stream_mutex);
  const auto entry = g_stream_instances.find(id);
  if (entry == g_stream_instances.end()) {
    return -1;
  }

  auto& instance = entry->second;
  refresh_status(id, instance);
  if (!instance.source || instance.status == CustomAudioStreamStatus::FINISHED) {
    return -1;
  }
  const float position = instance.source->position_seconds();
  const s32 position_second = static_cast<s32>(std::floor(position));
  if (position >= 0.0f && position_second != instance.last_logged_position_second) {
    log_stream_state("playback-progress", id, instance);
    instance.last_logged_position_second = position_second;
  }
  return position < 0.0f ? -1 : static_cast<s32>(std::floor(position * 30.0f));
}

u32 UpdateCustomAudioStreamQueue(const char* const* keys, const u32* ids, u32 count) {
  std::lock_guard<std::mutex> lock(g_stream_mutex);
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
      auto& instance = prepare_instance(id, registration->first);
      if (!instance.queued) {
        instance.queued = true;
        log_stream_state("queue-selected", id, instance);
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
      log_stream_state("queue-ready-evicted", id, instance);
      entry = g_stream_instances.erase(entry);
      continue;
    }
    if (instance.status == CustomAudioStreamStatus::ACTIVE ||
        instance.status == CustomAudioStreamStatus::PENDING) {
      log_stream_state("queue-active-evicted", id, instance);
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

  std::lock_guard<std::mutex> lock(g_stream_mutex);
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
  const bool log_request =
      instance.start_request_count <= 8 || (instance.start_request_count % 60) == 0;
  if (log_request) {
    lg::info("[CUSTOM_SPATIAL_AUDIO] start-request count={}", instance.start_request_count);
    log_stream_state("start-request-state", id, instance);
  }
  if (instance.status == CustomAudioStreamStatus::ACTIVE ||
      instance.status == CustomAudioStreamStatus::PENDING) {
    if (log_request) {
      log_stream_state("start-request-noop", id, instance);
    }
    return true;
  }

  instance.status = CustomAudioStreamStatus::PENDING;
  log_stream_state("decoder-starting", id, instance);
  instance.source = std::make_unique<custom_audio::Source>();
  if (!instance.source->start(registration->second.full_path)) {
    lg::warn("Failed to decode custom audio '{}' at '{}'", key, registration->second.full_path);
    instance.source.reset();
    instance.status = CustomAudioStreamStatus::FINISHED;
    log_stream_state("decoder-failed", id, instance);
    return true;
  }

  instance.paused = false;
  instance.status = CustomAudioStreamStatus::ACTIVE;
  apply_spatial_volume(instance);
  log_stream_state("playback-started", id, instance);
  return true;
}

bool StopCustomAudioStream(const char* key, s32 id) {
  std::lock_guard<std::mutex> lock(g_stream_mutex);
  if (key && key[0] && !g_registered_streams.contains(key)) {
    return false;
  }

  const auto entry = g_stream_instances.find(id);
  if (entry == g_stream_instances.end()) {
    const bool registered_key = key && key[0] && g_registered_streams.contains(key);
    if (registered_key) {
      lg::info("[CUSTOM_SPATIAL_AUDIO] stop-request-without-instance key='{}' id={}", key, id);
    }
    return registered_key;
  }
  if ((!key || !key[0]) && entry->second.status == CustomAudioStreamStatus::FINISHED) {
    return false;
  }
  lg::info("[CUSTOM_SPATIAL_AUDIO] stop-request requested-key='{}' id={}",
           key && key[0] ? key : "<by-id>", id);
  log_stream_state("stop-request-state", id, entry->second);
  if (entry->second.source) {
    entry->second.source->stop();
    entry->second.source.reset();
  }
  entry->second.paused = false;
  entry->second.status = CustomAudioStreamStatus::FINISHED;
  return true;
}

bool PauseCustomAudioStream(s32 id) {
  std::lock_guard<std::mutex> lock(g_stream_mutex);
  const auto entry = g_stream_instances.find(id);
  if (entry == g_stream_instances.end() ||
      entry->second.status == CustomAudioStreamStatus::FINISHED) {
    return false;
  }
  if (entry->second.source && entry->second.status == CustomAudioStreamStatus::ACTIVE) {
    log_stream_state("pause-request", id, entry->second);
    entry->second.source->pause();
    entry->second.paused = true;
  }
  return true;
}

bool ContinueCustomAudioStream(s32 id) {
  std::lock_guard<std::mutex> lock(g_stream_mutex);
  const auto entry = g_stream_instances.find(id);
  if (entry == g_stream_instances.end() ||
      entry->second.status == CustomAudioStreamStatus::FINISHED) {
    return false;
  }
  if (entry->second.source && entry->second.status == CustomAudioStreamStatus::ACTIVE &&
      entry->second.paused) {
    log_stream_state("continue-request", id, entry->second);
    entry->second.source->resume();
    entry->second.paused = false;
    apply_spatial_volume(entry->second);
  }
  return true;
}

bool SetCustomAudioStreamParams(s32 id, const SoundParams& params) {
  std::lock_guard<std::mutex> lock(g_stream_mutex);
  const auto entry = g_stream_instances.find(id);
  if (entry == g_stream_instances.end() ||
      entry->second.status == CustomAudioStreamStatus::FINISHED) {
    return false;
  }
  const bool had_spatial_params = entry->second.has_spatial_params;
  apply_params(entry->second, params);
  if (!had_spatial_params && entry->second.has_spatial_params) {
    log_stream_state("first-spatial-params", id, entry->second);
  }
  return true;
}

void UpdateCustomAudioStreams() {
  std::lock_guard<std::mutex> lock(g_stream_mutex);
  for (auto& [id, instance] : g_stream_instances) {
    refresh_status(id, instance);
    apply_spatial_volume(instance);
  }
}

void PauseCustomAudioStreams() {
  std::lock_guard<std::mutex> lock(g_stream_mutex);
  for (auto& [id, instance] : g_stream_instances) {
    if (instance.source && instance.status == CustomAudioStreamStatus::ACTIVE && !instance.paused) {
      log_stream_state("group-pause", id, instance);
      instance.source->pause();
      instance.paused = true;
    }
  }
}

void ContinueCustomAudioStreams() {
  std::lock_guard<std::mutex> lock(g_stream_mutex);
  for (auto& [id, instance] : g_stream_instances) {
    if (instance.source && instance.status == CustomAudioStreamStatus::ACTIVE && instance.paused) {
      log_stream_state("group-continue", id, instance);
      instance.source->resume();
      instance.paused = false;
      apply_spatial_volume(instance);
    }
  }
}

void StopCustomAudioStreams() {
  std::lock_guard<std::mutex> lock(g_stream_mutex);
  for (auto& [id, instance] : g_stream_instances) {
    if (instance.status != CustomAudioStreamStatus::FINISHED) {
      log_stream_state("group-stop", id, instance);
    }
    if (instance.source) {
      instance.source->stop();
      instance.source.reset();
    }
    instance.paused = false;
    instance.status = CustomAudioStreamStatus::FINISHED;
  }
}

}  // namespace jak2
