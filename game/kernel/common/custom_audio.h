#pragma once

#include <memory>
#include <string>

#include "common/common_types.h"

namespace custom_audio {

void initialize();
void set_master_volume(float volume);

class Source {
 public:
  Source();
  ~Source();

  Source(const Source&) = delete;
  Source& operator=(const Source&) = delete;

  bool start(const std::string& path);
  void stop();
  void pause();
  void resume();
  void set_stereo_volume(u32 left, u32 right);
  bool is_playing() const;
  bool is_at_end() const;
  float position_seconds() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

}  // namespace custom_audio

u64 playMP3(u32 file_path, u32 volume);
void stopMP3(u32 file_path);
void stopAllSounds();
void playMainMusic(u32 file_path, u32 volume);
void pauseMainMusic();
void stopMainMusic();
void resumeMainMusic();
void changeMainMusicVolume(u32 volume);
