#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>

namespace multiplayer::platform::replication {

struct TransformSample {
  uint32_t sample_time_ms = 0;
  std::array<float, 3> position = {};
  std::array<float, 4> quaternion = {0.0f, 0.0f, 0.0f, 1.0f};
  std::array<float, 3> velocity = {};
  bool velocity_valid = false;
  bool velocity_estimated = false;
};

struct PresentedTransform {
  std::array<float, 3> position = {};
  std::array<float, 4> quaternion = {0.0f, 0.0f, 0.0f, 1.0f};
  std::array<float, 3> velocity = {};
  bool velocity_valid = false;
  bool valid = false;
};

class SnapshotTimeline {
 public:
  static constexpr size_t kOffsetWindowSize = 32;

  bool push(TransformSample sample, const uint64_t arrival_time_ms, const float snap_distance) {
    if (!samples_.empty() && !newer(sample.sample_time_ms, samples_.back().sample.sample_time_ms))
      return false;
    if (!samples_.empty() &&
        distance(samples_.back().sample.position, sample.position) > snap_distance) {
      reset();
    }
    if (!samples_.empty()) {
      const auto& [prev_sample, prev_arrival_time_ms] = samples_.back();
      const uint32_t sender_interval = sample.sample_time_ms - prev_sample.sample_time_ms;
      const uint64_t arrival_interval = arrival_time_ms - prev_arrival_time_ms;
      const uint64_t variation = sender_interval > arrival_interval
                                     ? sender_interval - arrival_interval
                                     : arrival_interval - sender_interval;
      sender_interval_ms_ = sender_interval_ms_ == 0
                                ? sender_interval
                                : (sender_interval_ms_ * 7 + sender_interval) / 8;
      jitter_ms_ = static_cast<uint32_t>(std::min<uint64_t>(
          jitter_ms_ == 0 ? variation : (jitter_ms_ * 7ull + variation) / 8, UINT32_MAX));
      if (!sample.velocity_valid && sender_interval != 0) {
        estimate_velocity(sample, prev_sample, sample, sender_interval);
      }
      if (samples_.size() >= 2 && samples_.back().sample.velocity_estimated) {
        const auto& before = samples_[samples_.size() - 2].sample;
        estimate_velocity(samples_.back().sample, before, sample,
                          sample.sample_time_ms - before.sample_time_ms);
      }
    }
    update_clock_offset(
        static_cast<int32_t>(static_cast<uint32_t>(arrival_time_ms) - sample.sample_time_ms));
    samples_.push_back({.sample = sample, .arrival_time_ms = arrival_time_ms});
    while (samples_.size() > 8)
      samples_.pop_front();
    return true;
  }

  PresentedTransform present(const uint64_t now_ms,
                             const uint32_t nominal_interval_ms,
                             const uint32_t minimum_delay_ms,
                             const uint32_t maximum_delay_ms,
                             const uint32_t maximum_extrapolation_ms,
                             const float output_smoothing_rate = 0.0f) {
    if (samples_.empty() || !offset_valid_)
      return {};
    const uint32_t interval = sender_interval_ms_ == 0 ? nominal_interval_ms : sender_interval_ms_;
    const uint64_t desired_delay = static_cast<uint64_t>(interval) + jitter_ms_ * 2ull;
    const uint32_t delay =
        std::clamp(desired_delay > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(desired_delay),
                   minimum_delay_ms, maximum_delay_ms);
    const uint32_t ideal_target =
        static_cast<uint32_t>(now_ms) - static_cast<uint32_t>(clock_offset_ms_) - delay;
    if (presentation_time_valid_) {
      const uint32_t elapsed =
          static_cast<uint32_t>(std::min<uint64_t>(now_ms - last_present_time_ms_, UINT32_MAX));
      const uint32_t projected_target = presentation_time_ms_ + elapsed;
      const int32_t phase_error = static_cast<int32_t>(ideal_target - projected_target);
      const int32_t maximum_slew = elapsed == 0
                                       ? 0
                                       : static_cast<int32_t>(std::max<uint32_t>(
                                             1, phase_error < 0 ? elapsed / 2 : elapsed / 10));
      const int32_t adjustment = std::clamp(phase_error, -maximum_slew, maximum_slew);
      presentation_time_ms_ =
          static_cast<uint32_t>(static_cast<int64_t>(projected_target) + adjustment);
    } else {
      presentation_time_ms_ = ideal_target;
      presentation_time_valid_ = true;
    }
    last_present_time_ms_ = now_ms;
    const uint32_t target = presentation_time_ms_;
    if (!newer(target, samples_.front().sample.sample_time_ms))
      return smooth_output(from_sample(samples_.front().sample), now_ms, output_smoothing_rate);

    for (size_t index = 1; index < samples_.size(); ++index) {
      const auto& left = samples_[index - 1].sample;
      if (const auto& right = samples_[index].sample;
          newer(right.sample_time_ms, target) || right.sample_time_ms == target) {
        const uint32_t span = right.sample_time_ms - left.sample_time_ms;
        const float alpha = span == 0
                                ? 1.0f
                                : std::clamp(static_cast<float>(target - left.sample_time_ms) /
                                                 static_cast<float>(span),
                                             0.0f, 1.0f);

        return smooth_output(interpolate(left, right, alpha), now_ms, output_smoothing_rate);
      }
    }
    const auto& newest = samples_.back().sample;
    const uint32_t lead_ms =
        newer(target, newest.sample_time_ms)
            ? std::min(target - newest.sample_time_ms, maximum_extrapolation_ms)
            : 0;
    PresentedTransform result = from_sample(newest);
    const float lead_seconds = static_cast<float>(lead_ms) / 1000.0f;
    for (size_t axis = 0; axis < result.position.size(); ++axis)
      result.position[axis] += result.velocity[axis] * lead_seconds;
    return smooth_output(result, now_ms, output_smoothing_rate);
  }

  void reset() {
    samples_.clear();
    jitter_ms_ = 0;
    sender_interval_ms_ = 0;
    clock_offset_ms_ = 0;
    offset_valid_ = false;
    offset_samples_.clear();
    presentation_time_ms_ = 0;
    last_present_time_ms_ = 0;
    presentation_time_valid_ = false;
    presented_position_ = {};
    last_output_time_ms_ = 0;
    presented_quaternion_ = {0.0f, 0.0f, 0.0f, 1.0f};
    presented_transform_valid_ = false;
  }

 private:
  struct Entry {
    TransformSample sample;
    uint64_t arrival_time_ms = 0;
  };

  static bool newer(const uint32_t value, const uint32_t reference) {
    return static_cast<int32_t>(value - reference) > 0;
  }

  static float distance(const std::array<float, 3>& left, const std::array<float, 3>& right) {
    float squared = 0.0f;
    for (size_t axis = 0; axis < left.size(); ++axis) {
      const float delta = left[axis] - right[axis];
      squared += delta * delta;
    }
    return std::sqrt(squared);
  }

  static std::array<float, 4> normalize(std::array<float, 4> value) {
    float magnitude = 0.0f;
    for (const auto component : value)
      magnitude += component * component;
    magnitude = std::sqrt(magnitude);
    if (magnitude <= 0.000001f)
      return {0.0f, 0.0f, 0.0f, 1.0f};
    for (auto& component : value)
      component /= magnitude;
    return value;
  }

  static std::array<float, 4> slerp(std::array<float, 4> left,
                                    std::array<float, 4> right,
                                    const float alpha) {
    left = normalize(left);
    right = normalize(right);
    float dot = 0.0f;
    for (size_t index = 0; index < left.size(); ++index)
      dot += left[index] * right[index];
    if (dot < 0.0f) {
      for (auto& value : right)
        value = -value;
      dot = -dot;
    }
    std::array<float, 4> result = {};
    if (dot > 0.9995f) {
      for (size_t index = 0; index < result.size(); ++index)
        result[index] = left[index] + (right[index] - left[index]) * alpha;
    } else {
      const float theta = std::acos(std::clamp(dot, -1.0f, 1.0f));
      const float inverse_sine = 1.0f / std::sin(theta);
      const float left_weight = std::sin((1.0f - alpha) * theta) * inverse_sine;
      const float right_weight = std::sin(alpha * theta) * inverse_sine;
      for (size_t index = 0; index < result.size(); ++index)
        result[index] = left[index] * left_weight + right[index] * right_weight;
    }
    float magnitude = 0.0f;
    for (const auto value : result)
      magnitude += value * value;
    magnitude = std::sqrt(magnitude);
    if (magnitude > 0.0f) {
      for (auto& value : result)
        value /= magnitude;
    }
    return result;
  }

  static PresentedTransform from_sample(const TransformSample& sample) {
    return {.position = sample.position,
            .quaternion = sample.quaternion,
            .velocity = sample.velocity,
            .velocity_valid = sample.velocity_valid,
            .valid = true};
  }

  PresentedTransform smooth_output(PresentedTransform result,
                                   const uint64_t now_ms,
                                   const float smoothing_rate) {
    if (smoothing_rate <= 0.0f) {
      presented_position_ = result.position;
      presented_quaternion_ = result.quaternion;
      last_output_time_ms_ = now_ms;
      presented_transform_valid_ = result.valid;
      return result;
    }
    if (!result.valid) {
      presented_transform_valid_ = false;
      return result;
    }
    if (!presented_transform_valid_) {
      presented_position_ = result.position;
      presented_quaternion_ = normalize(result.quaternion);
      last_output_time_ms_ = now_ms;
      presented_transform_valid_ = true;
      result.position = presented_position_;
      result.quaternion = presented_quaternion_;
      return result;
    }
    const uint64_t elapsed_ms = now_ms - last_output_time_ms_;
    if (elapsed_ms != 0) {
      const float dt = static_cast<float>(elapsed_ms) / 1000.0f;
      const float alpha = 1.0f - std::exp(-smoothing_rate * dt);
      for (size_t axis = 0; axis < result.position.size(); ++axis) {
        if (result.velocity_valid) {
          const float predicted = presented_position_[axis] + result.velocity[axis] * dt;
          presented_position_[axis] = predicted + (result.position[axis] - predicted) * alpha;
        } else {
          presented_position_[axis] += (result.position[axis] - presented_position_[axis]) * alpha;
        }
      }
      presented_quaternion_ = slerp(presented_quaternion_, result.quaternion, alpha);
      last_output_time_ms_ = now_ms;
    }
    result.position = presented_position_;
    result.quaternion = presented_quaternion_;
    return result;
  }

  static PresentedTransform interpolate(const TransformSample& left,
                                        const TransformSample& right,
                                        const float alpha) {
    auto result = from_sample(right);
    for (size_t axis = 0; axis < result.position.size(); ++axis) {
      result.position[axis] =
          left.position[axis] + (right.position[axis] - left.position[axis]) * alpha;
      result.velocity[axis] =
          left.velocity[axis] + (right.velocity[axis] - left.velocity[axis]) * alpha;
    }
    result.velocity_valid = left.velocity_valid && right.velocity_valid;
    result.quaternion = slerp(left.quaternion, right.quaternion, alpha);
    return result;
  }

  static void estimate_velocity(TransformSample& target,
                                const TransformSample& previous,
                                const TransformSample& current,
                                const uint32_t interval_ms) {
    if (interval_ms == 0)
      return;
    const float inverse_seconds = 1000.0f / static_cast<float>(interval_ms);
    for (size_t axis = 0; axis < target.velocity.size(); ++axis)
      target.velocity[axis] = (current.position[axis] - previous.position[axis]) * inverse_seconds;
    target.velocity_valid = true;
    target.velocity_estimated = true;
  }

  void update_clock_offset(const int32_t offset) {
    offset_samples_.push_back(offset);
    while (offset_samples_.size() > kOffsetWindowSize)
      offset_samples_.pop_front();
    clock_offset_ms_ = *std::ranges::min_element(offset_samples_);
    offset_valid_ = true;
  }

  std::deque<Entry> samples_;
  uint32_t jitter_ms_ = 0;
  uint32_t sender_interval_ms_ = 0;
  int32_t clock_offset_ms_ = 0;
  bool offset_valid_ = false;
  std::deque<int32_t> offset_samples_;
  uint32_t presentation_time_ms_ = 0;
  uint64_t last_present_time_ms_ = 0;
  bool presentation_time_valid_ = false;
  std::array<float, 3> presented_position_ = {};
  uint64_t last_output_time_ms_ = 0;
  std::array<float, 4> presented_quaternion_ = {0.0f, 0.0f, 0.0f, 1.0f};
  bool presented_transform_valid_ = false;
};

}  // namespace multiplayer::platform::replication
