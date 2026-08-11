#pragma once

#include "common/common_types.h"

#include "game/overlord/common/ssound.h"

namespace jak2 {

enum class CustomAudioStreamStatus : s32 {
  NOT_CUSTOM = 0,
  READY = 1,
  PENDING = 2,
  ACTIVE = 3,
  FINISHED = 4,
};

bool RegisterCustomAudioStream(const char* key, const char* relative_path);
CustomAudioStreamStatus GetCustomAudioStreamStatus(const char* key, s32 id);
s32 GetCustomAudioStreamPosition(s32 id);

u32 UpdateCustomAudioStreamQueue(const char* const* keys, const u32* ids, u32 count);

bool StartCustomAudioStream(const char* key, s32 id);
bool StopCustomAudioStream(const char* key, s32 id);
bool PauseCustomAudioStream(s32 id);
bool ContinueCustomAudioStream(s32 id);
bool SetCustomAudioStreamParams(s32 id, const SoundParams& params);

void UpdateCustomAudioStreams();
void PauseCustomAudioStreams();
void ContinueCustomAudioStreams();
void StopCustomAudioStreams();

}  // namespace jak2
