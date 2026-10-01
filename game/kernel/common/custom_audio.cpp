#include "custom_audio.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <list>
#include <map>
#include <mutex>
#include <ranges>
#include <thread>

#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "game/kernel/common/Ptr.h"
#include "game/kernel/common/kscheme.h"
#include "game/runtime.h"

#define MINIAUDIO_IMPLEMENTATION
namespace MiniAudioLib {
#if defined(__APPLE__)
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#include "third-party/miniaudio.h"
#undef _POSIX_C_SOURCE
#else
#define NOT_REAL_OLD_POSIX_C_SOURCE _POSIX_C_SOURCE
#include "third-party/miniaudio.h"
#define _POSIX_C_SOURCE NOT_REAL_OLD_POSIX_C_SOURCE
#undef NOT_REAL_OLD_POSIX_C_SOURCE
#endif
#else
#include "third-party/miniaudio.h"
#endif
}  // namespace MiniAudioLib

#include "common/symbols.h"

namespace {

MiniAudioLib::ma_engine g_engine;
std::map<std::string, std::list<MiniAudioLib::ma_sound>> g_sound_map;
MiniAudioLib::ma_sound* g_main_music_sound = nullptr;
std::mutex g_active_sounds_mutex;
std::mutex g_main_music_mutex;

u64 goal_bool(const bool value) {
  return value ? static_cast<u64>(s7.offset) + true_symbol_offset(g_game_version) : s7.offset;
}

std::string custom_audio_path(const std::string& relative_path) {
  return fs::path(file_util::get_jak_project_dir() / "custom_assets" /
                  game_version_names[g_game_version] / "audio" / relative_path)
      .string();
}

u64 play_mp3_internal(const u32 file_path_ptr, const u32 volume, const bool is_main_music) {
  const std::string file_path = Ptr<String>(file_path_ptr).c()->data();
  const std::string full_path = custom_audio_path(file_path);
  if (!file_util::file_exists(full_path)) {
    return goal_bool(false);
  }

  std::thread thread([=]() {
    std::cout << "Playing file: " << file_path << std::endl;
    MiniAudioLib::ma_sound sound;
    const auto result = ma_sound_init_from_file(&g_engine, full_path.c_str(), 0,
                                                               nullptr, nullptr, &sound);
    if (result != MiniAudioLib::MA_SUCCESS) {
      std::cout << "Failed to load: " << file_path << std::endl;
      return;
    }

    ma_sound_set_volume(&sound, static_cast<float>(volume) / 100.0f);
    if (is_main_music) {
      ma_sound_set_looping(&sound, MA_TRUE);
      std::lock_guard lock(g_main_music_mutex);
      g_main_music_sound = &sound;
    }

    ma_sound_start(&sound);
    if (!is_main_music) {
      std::lock_guard lock(g_active_sounds_mutex);
      g_sound_map[file_path].push_back(sound);
    }

    while (g_main_music_sound == &sound || ma_sound_is_playing(&sound)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ma_sound_stop(&sound);
    ma_sound_uninit(&sound);
    std::cout << "Finished playing file: " << file_path << std::endl;

    if (!is_main_music) {
      std::lock_guard lock(g_active_sounds_mutex);
      if (const auto entry = g_sound_map.find(file_path); entry != g_sound_map.end()) {
        entry->second.remove_if(
            [&](const MiniAudioLib::ma_sound& listed_sound) { return &sound == &listed_sound; });
      }
    }
  });

  thread.detach();
  return goal_bool(true);
}

}  // namespace

namespace custom_audio {

struct Source::Impl {
  MiniAudioLib::ma_sound sound{};
  bool initialized = false;
};

Source::Source() : m_impl(std::make_unique<Impl>()) {}

Source::~Source() {
  if (m_impl->initialized) {
    ma_sound_stop(&m_impl->sound);
    ma_sound_uninit(&m_impl->sound);
  }
}

bool Source::start(const std::string& path) const
{
  if (m_impl->initialized) {
    stop();
    ma_sound_uninit(&m_impl->sound);
    m_impl->initialized = false;
  }

  const auto result = ma_sound_init_from_file(
      &g_engine, path.c_str(), MiniAudioLib::MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, nullptr,
      &m_impl->sound);
  if (result != MiniAudioLib::MA_SUCCESS) {
    return false;
  }

  m_impl->initialized = true;
  ma_sound_set_pan_mode(&m_impl->sound, MiniAudioLib::ma_pan_mode_pan);
  ma_sound_set_volume(&m_impl->sound, 0.0f);
  return ma_sound_start(&m_impl->sound) == MiniAudioLib::MA_SUCCESS;
}

void Source::stop() const
{
  if (m_impl->initialized) {
    ma_sound_stop(&m_impl->sound);
  }
}

void Source::pause() const
{
  stop();
}

void Source::resume() const
{
  if (m_impl->initialized && !is_at_end()) {
    ma_sound_start(&m_impl->sound);
  }
}

void Source::set_stereo_volume(const u32 left, const u32 right) const
{
  if (!m_impl->initialized) {
    return;
  }

  constexpr float kMaxVolume = 16383.0f;
  constexpr float kPanStrength = 1.25f;
  const float left_volume = static_cast<float>(left);
  const float right_volume = static_cast<float>(right);
  const float combined_volume = left_volume + right_volume;
  const float volume = combined_volume / (2.0f * kMaxVolume);
  const float base_pan =
      combined_volume > 0.0f ? (right_volume - left_volume) / combined_volume : 0.0f;
  const float pan = std::clamp(base_pan * kPanStrength, -1.0f, 1.0f);

  ma_sound_set_volume(&m_impl->sound, volume);
  ma_sound_set_pan(&m_impl->sound, pan);
}

bool Source::is_playing() const {
  return m_impl->initialized && ma_sound_is_playing(&m_impl->sound);
}

bool Source::is_at_end() const {
  return m_impl->initialized && ma_sound_at_end(&m_impl->sound);
}

float Source::position_seconds() const {
  if (!m_impl->initialized) {
    return -1.0f;
  }
  float cursor = -1.0f;
  if (ma_sound_get_cursor_in_seconds(&m_impl->sound, &cursor) !=
      MiniAudioLib::MA_SUCCESS) {
    return -1.0f;
  }
  return cursor;
}

void initialize() {
#ifdef _WIN32
  ma_engine_uninit(&g_engine);
#endif
  auto config = MiniAudioLib::ma_engine_config_init();
  config.channels = 2;
  if (const auto result = ma_engine_init(&config, &g_engine); result != MiniAudioLib::MA_SUCCESS) {
    lg::error("[CUSTOM_AUDIO] Failed to initialize the stereo MiniAudio engine: {}",
              static_cast<int>(result));
    return;
  }
  lg::info("[CUSTOM_AUDIO] MiniAudio engine initialized with {} output channels",
           ma_engine_get_channels(&g_engine));
}

void set_master_volume(const float volume) {
  ma_engine_set_volume(&g_engine, volume);
}

}  // namespace custom_audio

void stopMP3(const u32 file_path_ptr) {
  const std::string file_path = Ptr<String>(file_path_ptr).c()->data();
  std::lock_guard lock(g_active_sounds_mutex);
  const auto entry = g_sound_map.find(file_path);
  if (entry == g_sound_map.end()) {
    return;
  }
  for (auto sound : entry->second) {
    ma_sound_stop(&sound);
  }
  entry->second.clear();
}

void stopAllSounds() {
  std::lock_guard lock(g_active_sounds_mutex);
  for (auto& sounds : g_sound_map | std::views::values) {
    for (auto sound : sounds) {
      ma_sound_stop(&sound);
    }
    sounds.clear();
  }
  g_sound_map.clear();
}

u64 playMP3(const u32 file_path_ptr, const u32 volume) {
  return play_mp3_internal(file_path_ptr, volume, false);
}

void stopMainMusic() {
  std::lock_guard lock(g_main_music_mutex);
  if (g_main_music_sound && ma_sound_is_playing(g_main_music_sound)) {
    ma_sound_stop(g_main_music_sound);
    g_main_music_sound = nullptr;
  }
}

void playMainMusic(const u32 file_path_ptr, const u32 volume) {
  stopMainMusic();
  play_mp3_internal(file_path_ptr, volume, true);
}

void pauseMainMusic() {
  std::lock_guard lock(g_main_music_mutex);
  if (g_main_music_sound && ma_sound_is_playing(g_main_music_sound)) {
    ma_sound_stop(g_main_music_sound);
  }
}

void resumeMainMusic() {
  std::lock_guard lock(g_main_music_mutex);
  if (g_main_music_sound && !ma_sound_is_playing(g_main_music_sound)) {
    ma_sound_start(g_main_music_sound);
  }
}

void changeMainMusicVolume(const u32 volume) {
  std::lock_guard lock(g_main_music_mutex);
  if (g_main_music_sound) {
    ma_sound_set_volume(g_main_music_sound, static_cast<float>(volume) / 100.0f);
  }
}
