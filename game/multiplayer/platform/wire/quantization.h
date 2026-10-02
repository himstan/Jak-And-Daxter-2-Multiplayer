#pragma once

#include <array>
#include <cmath>

#include "game/multiplayer/platform/wire/serialize_codec.h"

namespace multiplayer::platform::wire {

inline constexpr float kPositionMin = -16777216.0f;
inline constexpr float kPositionMax = 16777216.0f;
inline constexpr float kPositionResolution = 64.0f;
inline constexpr float kVehiclePositionResolution = 64.0f;
inline constexpr float kLinearVelocityMin = -524288.0f;
inline constexpr float kLinearVelocityMax = 524288.0f;
inline constexpr float kLinearVelocityResolution = 16.0f;
inline constexpr float kAngularVelocityMin = -131072.0f;
inline constexpr float kAngularVelocityMax = 131072.0f;
inline constexpr float kAngularVelocityResolution = 8.0f;
inline constexpr float kAngleMin = -32768.0f;
inline constexpr float kAngleMax = 32768.0f;
inline constexpr float kAngleResolution = 2.0f;
inline constexpr float kQuaternionMin = -1.0f;
inline constexpr float kQuaternionMax = 1.0f;
inline constexpr float kQuaternionResolution = 1.0f / 1024.0f;
inline constexpr float kUnitMin = 0.0f;
inline constexpr float kUnitMax = 1.0f;
inline constexpr float kUnitResolution = 1.0f / 255.0f;
inline constexpr float kTrajectoryDurationMin = 0.0f;
inline constexpr float kTrajectoryDurationMax = 65535.0f;
inline constexpr float kTrajectoryDurationResolution = 1.0f / 256.0f;

inline bool finite_and_in_range(const float value, const float minimum, const float maximum) {
  return std::isfinite(value) && value >= minimum && value <= maximum;
}

template <size_t Size>
bool finite_and_in_range(const std::array<float, Size>& values,
                         const float minimum,
                         const float maximum) {
  for (const float value : values) {
    if (!finite_and_in_range(value, minimum, maximum)) {
      return false;
    }
  }
  return true;
}

inline float canonical_angle(const float value) {
  if (!std::isfinite(value)) {
    return value;
  }
  constexpr float full_turn = 65536.0f;
  constexpr float half_turn = 32768.0f;
  float wrapped = std::fmod(value + half_turn, full_turn);
  if (wrapped < 0.0f) {
    wrapped += full_turn;
  }
  return wrapped - half_turn;
}

template <typename Stream>
bool serialize_position(Stream& stream, float& value) {
  serialize_compressed_float(stream, value, kPositionMin, kPositionMax, kPositionResolution);
  return true;
}

template <typename Stream>
bool serialize_linear_velocity(Stream& stream, float& value) {
  serialize_compressed_float(stream, value, kLinearVelocityMin, kLinearVelocityMax,
                             kLinearVelocityResolution);
  return true;
}

template <typename Stream>
bool serialize_angular_velocity(Stream& stream, float& value) {
  serialize_compressed_float(stream, value, kAngularVelocityMin, kAngularVelocityMax,
                             kAngularVelocityResolution);
  return true;
}

template <typename Stream>
bool serialize_angle(Stream& stream, float& value) {
  serialize_compressed_float(stream, value, kAngleMin, kAngleMax, kAngleResolution);
  return true;
}

template <typename Stream>
bool serialize_quaternion_component(Stream& stream, float& value) {
  serialize_compressed_float(stream, value, kQuaternionMin, kQuaternionMax, kQuaternionResolution);
  return true;
}

template <typename Stream>
bool serialize_unit(Stream& stream, float& value) {
  serialize_compressed_float(stream, value, kUnitMin, kUnitMax, kUnitResolution);
  return true;
}

template <typename Stream>
bool serialize_trajectory_duration(Stream& stream, float& value) {
  serialize_compressed_float(stream, value, kTrajectoryDurationMin, kTrajectoryDurationMax,
                             kTrajectoryDurationResolution);
  return true;
}

template <typename Stream, size_t Size>
bool serialize_position_array(Stream& stream, std::array<float, Size>& values) {
  for (float& value : values) {
    if (!serialize_position(stream, value)) {
      return false;
    }
  }
  return true;
}

template <typename Stream, size_t Size>
bool serialize_quaternion_array(Stream& stream, std::array<float, Size>& values) {
  for (float& value : values) {
    if (!serialize_quaternion_component(stream, value)) {
      return false;
    }
  }
  return true;
}

template <typename Stream, size_t Size>
bool serialize_linear_velocity_array(Stream& stream, std::array<float, Size>& values) {
  for (float& value : values) {
    if (!serialize_linear_velocity(stream, value)) {
      return false;
    }
  }
  return true;
}

template <typename Stream, size_t Size>
bool serialize_angular_velocity_array(Stream& stream, std::array<float, Size>& values) {
  for (float& value : values) {
    if (!serialize_angular_velocity(stream, value)) {
      return false;
    }
  }
  return true;
}

template <typename Stream>
bool serialize_vehicle_position(Stream& stream, float& value) {
  serialize_compressed_float(stream, value, kPositionMin, kPositionMax, kVehiclePositionResolution);
  return true;
}

template <typename Stream, size_t Size>
bool serialize_vehicle_position_array(Stream& stream, std::array<float, Size>& values) {
  for (float& value : values) {
    if (!serialize_vehicle_position(stream, value)) {
      return false;
    }
  }
  return true;
}

template <typename Stream>
bool serialize_vehicle_quaternion(Stream& stream, float& value) {
  return serialization::serialize_f32(stream, value);
}

template <typename Stream, size_t Size>
bool serialize_vehicle_quaternion_array(Stream& stream, std::array<float, Size>& values) {
  for (float& value : values) {
    if (!serialize_vehicle_quaternion(stream, value)) {
      return false;
    }
  }
  return true;
}

template <size_t Size>
bool valid_position_array(const std::array<float, Size>& values) {
  return finite_and_in_range(values, kPositionMin, kPositionMax);
}

template <size_t Size>
bool valid_quaternion_array(const std::array<float, Size>& values) {
  return finite_and_in_range(values, kQuaternionMin, kQuaternionMax);
}

template <size_t Size>
bool valid_linear_velocity_array(const std::array<float, Size>& values) {
  return finite_and_in_range(values, kLinearVelocityMin, kLinearVelocityMax);
}

template <size_t Size>
bool valid_angular_velocity_array(const std::array<float, Size>& values) {
  return finite_and_in_range(values, kAngularVelocityMin, kAngularVelocityMax);
}

inline bool valid_angle(const float value) {
  return finite_and_in_range(value, kAngleMin, kAngleMax);
}

inline bool valid_unit(const float value) {
  return finite_and_in_range(value, kUnitMin, kUnitMax);
}

inline bool valid_trajectory_duration(const float value) {
  return finite_and_in_range(value, kTrajectoryDurationMin, kTrajectoryDurationMax);
}

}  // namespace multiplayer::platform::wire
