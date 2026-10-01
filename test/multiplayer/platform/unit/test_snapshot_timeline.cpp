#include <cmath>

#include "game/multiplayer/platform/replication/snapshot_timeline.h"
#include "gtest/gtest.h"

TEST(SnapshotTimeline, RejectsDuplicateAndOlderSamplesIncludingZero) {
  using multiplayer::platform::replication::SnapshotTimeline;
  SnapshotTimeline timeline;
  EXPECT_FALSE(timeline.present(0, 0, 0, 0, 0).valid);
  ASSERT_TRUE(timeline.push({.sample_time_ms = 0}, 0, 20.0f));
  EXPECT_FALSE(timeline.push({.sample_time_ms = 0}, 1, 20.0f));
  EXPECT_FALSE(timeline.push({.sample_time_ms = UINT32_MAX}, 2, 20.0f));
  EXPECT_TRUE(timeline.push({.sample_time_ms = 1}, 3, 20.0f));
  timeline.reset();
  EXPECT_FALSE(timeline.present(3, 0, 0, 0, 0).valid);
  EXPECT_TRUE(timeline.push({.sample_time_ms = 0}, 4, 20.0f));
}

TEST(SnapshotTimeline, SmoothsOutputPositionsAndQuaternions) {
  using multiplayer::platform::replication::SnapshotTimeline;
  SnapshotTimeline timeline;
  ASSERT_TRUE(timeline.push({.sample_time_ms = 0, .velocity_valid = true}, 0, 100.0f));
  ASSERT_TRUE(timeline.present(0, 0, 0, 0, 0, 12.0f).valid);
  ASSERT_TRUE(timeline.push({.sample_time_ms = 100,
                             .position = {10.0f, 0.0f, 0.0f},
                             .quaternion = {0.0f, 0.0f, 1.0f, 0.0f},
                             .velocity_valid = true},
                            100, 100.0f));
  const auto presented = timeline.present(100, 0, 0, 0, 0, 12.0f);
  ASSERT_TRUE(presented.valid);
  const float alpha = 1.0f - std::exp(-1.2f);
  EXPECT_NEAR(presented.position[0], 10.0f * alpha, 0.001f);
  EXPECT_NEAR(presented.quaternion[2], std::sin(alpha * 1.57079632679f), 0.001f);
  EXPECT_NEAR(presented.quaternion[3], std::cos(alpha * 1.57079632679f), 0.001f);
}

TEST(SnapshotTimeline, InterpolatesJitterAndBoundsExtrapolation) {
  using multiplayer::platform::replication::SnapshotTimeline;
  SnapshotTimeline timeline;
  ASSERT_TRUE(timeline.push({.sample_time_ms = 1000,
                             .position = {0.0f, 0.0f, 0.0f},
                             .quaternion = {0.0f, 0.0f, 0.0f, 1.0f},
                             .velocity = {10.0f, 0.0f, 0.0f},
                             .velocity_valid = true},
                            1100, 20.0f));
  ASSERT_TRUE(timeline.push({.sample_time_ms = 1100,
                             .position = {1.0f, 0.0f, 0.0f},
                             .quaternion = {0.0f, 1.0f, 0.0f, 0.0f},
                             .velocity = {10.0f, 0.0f, 0.0f},
                             .velocity_valid = true},
                            1220, 20.0f));

  const auto interpolated = timeline.present(1290, 100, 66, 250, 400);
  ASSERT_TRUE(interpolated.valid);
  EXPECT_NEAR(interpolated.position[0], 0.5f, 0.001f);
  EXPECT_NEAR(std::abs(interpolated.quaternion[1]), 0.7071f, 0.001f);
  EXPECT_NEAR(std::abs(interpolated.quaternion[3]), 0.7071f, 0.001f);

  const auto extrapolated = timeline.present(2000, 100, 66, 250, 250);
  ASSERT_TRUE(extrapolated.valid);
  EXPECT_NEAR(extrapolated.position[0], 3.5f, 0.001f);
}

TEST(SnapshotTimeline, UsesVelocityForQuantizedInterpolation) {
  using multiplayer::platform::replication::SnapshotTimeline;
  SnapshotTimeline timeline;
  ASSERT_TRUE(timeline.push({.sample_time_ms = 1000,
                             .position = {0.0f, 0.0f, 0.0f},
                             .velocity = {40960.0f, 0.0f, 0.0f},
                             .velocity_valid = true},
                            1000, 100000.0f));
  ASSERT_TRUE(timeline.push({.sample_time_ms = 1100,
                             .position = {4608.0f, 0.0f, 0.0f},
                             .velocity = {40960.0f, 0.0f, 0.0f},
                             .velocity_valid = true},
                            1100, 100000.0f));

  const auto presented = timeline.present(1025, 0, 0, 0, 0);
  ASSERT_TRUE(presented.valid);
  EXPECT_NEAR(presented.position[0], 1152.0f, 0.001f);
  EXPECT_NEAR(presented.velocity[0], 40960.0f, 0.001f);
}

TEST(SnapshotTimeline, SynchronizesRepeatedQuantizedPositions) {
  using multiplayer::platform::replication::SnapshotTimeline;
  SnapshotTimeline timeline;
  constexpr float velocity = 4096.0f;
  ASSERT_TRUE(timeline.push({.sample_time_ms = 1000,
                             .position = {0.0f, 0.0f, 0.0f},
                             .velocity = {velocity, 0.0f, 0.0f},
                             .velocity_valid = true},
                            1000, 100000.0f));
  ASSERT_TRUE(timeline.push({.sample_time_ms = 1066,
                             .position = {512.0f, 0.0f, 0.0f},
                             .velocity = {velocity, 0.0f, 0.0f},
                             .velocity_valid = true},
                            1066, 100000.0f));
  ASSERT_TRUE(timeline.push({.sample_time_ms = 1132,
                             .position = {512.0f, 0.0f, 0.0f},
                             .velocity = {velocity, 0.0f, 0.0f},
                             .velocity_valid = true},
                            1132, 100000.0f));

  const auto first = timeline.present(1000, 0, 0, 0, 0);
  const auto at_repeated_endpoint = timeline.present(1066, 0, 0, 0, 0);
  const auto after_repeated_endpoint = timeline.present(1082, 0, 0, 0, 0);
  ASSERT_TRUE(first.valid);
  ASSERT_TRUE(at_repeated_endpoint.valid);
  ASSERT_TRUE(after_repeated_endpoint.valid);
  EXPECT_GT(at_repeated_endpoint.position[0], first.position[0]);
  EXPECT_FLOAT_EQ(after_repeated_endpoint.position[0], at_repeated_endpoint.position[0]);
}

TEST(SnapshotTimeline, UsesObservedSenderCadence) {
  using multiplayer::platform::replication::SnapshotTimeline;
  SnapshotTimeline timeline;
  constexpr float velocity = 1000.0f;
  ASSERT_TRUE(timeline.push({.sample_time_ms = 0,
                             .position = {0.0f, 0.0f, 0.0f},
                             .velocity = {velocity, 0.0f, 0.0f},
                             .velocity_valid = true},
                            0, 100000.0f));
  ASSERT_TRUE(timeline.push({.sample_time_ms = 250,
                             .position = {250.0f, 0.0f, 0.0f},
                             .velocity = {velocity, 0.0f, 0.0f},
                             .velocity_valid = true},
                            250, 100000.0f));
  ASSERT_TRUE(timeline.push({.sample_time_ms = 500,
                             .position = {500.0f, 0.0f, 0.0f},
                             .velocity = {velocity, 0.0f, 0.0f},
                             .velocity_valid = true},
                            500, 100000.0f));

  const auto presented = timeline.present(700, 50, 66, 400, 400);
  ASSERT_TRUE(presented.valid);
  EXPECT_NEAR(presented.position[0], 450.0f, 0.001f);
}

TEST(SnapshotTimeline, NormalizesInterpolationQuaternions) {
  using multiplayer::platform::replication::SnapshotTimeline;
  SnapshotTimeline timeline;
  ASSERT_TRUE(timeline.push(
      {.sample_time_ms = 0, .quaternion = {0.0f, 0.0f, 0.0f, 2.0f}, .velocity_valid = true}, 0,
      100000.0f));
  ASSERT_TRUE(timeline.push(
      {.sample_time_ms = 100, .quaternion = {0.0f, 3.0f, 0.0f, 0.0f}, .velocity_valid = true}, 100,
      100000.0f));

  const auto presented = timeline.present(50, 0, 0, 0, 0);
  ASSERT_TRUE(presented.valid);
  EXPECT_NEAR(presented.quaternion[1], 0.707106f, 0.001f);
  EXPECT_NEAR(presented.quaternion[3], 0.707106f, 0.001f);
}

TEST(SnapshotTimeline, HandlesWrapInferenceAndTeleportReset) {
  using multiplayer::platform::replication::SnapshotTimeline;
  SnapshotTimeline timeline;
  const uint64_t clock_base = uint64_t{1} << 32;
  ASSERT_TRUE(timeline.push({.sample_time_ms = UINT32_MAX - 19, .position = {0.0f, 0.0f, 0.0f}},
                            clock_base + 50, 20.0f));
  ASSERT_TRUE(timeline.push({.sample_time_ms = 10, .position = {3.0f, 0.0f, 0.0f}}, clock_base + 80,
                            20.0f));
  const auto wrapped = timeline.present(clock_base + 93, 33, 33, 150, 250);
  ASSERT_TRUE(wrapped.valid);
  EXPECT_NEAR(wrapped.position[0], 1.0f, 0.001f);
  EXPECT_NEAR(wrapped.velocity[0], 100.0f / 3.0f, 0.001f);

  ASSERT_TRUE(timeline.push({.sample_time_ms = 20, .position = {100.0f, 0.0f, 0.0f}},
                            clock_base + 90, 20.0f));
  const auto snapped = timeline.present(clock_base + 123, 33, 33, 150, 250);
  ASSERT_TRUE(snapped.valid);
  EXPECT_FLOAT_EQ(snapped.position[0], 100.0f);
  EXPECT_FLOAT_EQ(snapped.velocity[0], 0.0f);
}

TEST(SnapshotTimeline, NeverRewindsWhenAdaptiveDelayIncreases) {
  using multiplayer::platform::replication::SnapshotTimeline;
  using multiplayer::platform::replication::TransformSample;
  SnapshotTimeline timeline;
  ASSERT_TRUE(timeline.push({.sample_time_ms = 1000,
                             .position = {0.0f, 0.0f, 0.0f},
                             .velocity = {10.0f, 0.0f, 0.0f},
                             .velocity_valid = true},
                            1100, 20.0f));
  ASSERT_TRUE(timeline.push({.sample_time_ms = 1033,
                             .position = {0.33f, 0.0f, 0.0f},
                             .velocity = {10.0f, 0.0f, 0.0f},
                             .velocity_valid = true},
                            1133, 20.0f));

  const auto before_jitter = timeline.present(1200, 33, 33, 150, 250);
  ASSERT_TRUE(before_jitter.valid);
  ASSERT_TRUE(timeline.push({.sample_time_ms = 1066,
                             .position = {0.66f, 0.0f, 0.0f},
                             .velocity = {10.0f, 0.0f, 0.0f},
                             .velocity_valid = true},
                            1266, 20.0f));

  const auto after_jitter = timeline.present(1267, 33, 33, 150, 250);
  ASSERT_TRUE(after_jitter.valid);
  EXPECT_GT(after_jitter.position[0], before_jitter.position[0]);
  EXPECT_LT(after_jitter.position[0] - before_jitter.position[0], 0.7f);
}
