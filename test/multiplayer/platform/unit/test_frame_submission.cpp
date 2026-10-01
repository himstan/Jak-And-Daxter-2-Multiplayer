#include <algorithm>
#include <array>
#include <vector>

#include "game/multiplayer/platform/session/frame_submission.h"
#include "gtest/gtest.h"

namespace {
using namespace multiplayer::platform;

struct Submission {
  FrameSendResult run(std::span<const ConnectionId> recipients,
                      FrameKind kind = FrameKind::GAMEPLAY,
                      TransportLane lane = TransportLane::GAMEPLAY_RELIABLE) {
    const std::array<uint8_t, 1> frame = {0x42};
    return submit_frame(
        recipients, frame, kind, lane,
        [this, lane](ConnectionId connection, auto bytes, auto selected_lane) {
          EXPECT_EQ(bytes.size(), 1u);
          EXPECT_EQ(bytes.front(), 0x42);
          EXPECT_EQ(selected_lane, lane);
          attempted.push_back(connection);
          return std::ranges::find(rejected, connection) == rejected.end();
        },
        [this](ConnectionId connection) { closed.push_back(connection); });
  }

  std::vector<ConnectionId> rejected;
  std::vector<ConnectionId> attempted;
  std::vector<ConnectionId> closed;
};

TEST(FrameSubmission, NoRecipientsIsDistinctFromRejection) {
  Submission submission;
  EXPECT_EQ(submission.run({}), FrameSendResult::NO_RECIPIENTS);
  EXPECT_TRUE(submission.attempted.empty());
  EXPECT_TRUE(submission.closed.empty());
}

TEST(FrameSubmission, QueuesEveryRecipientOnce) {
  Submission submission;
  const std::array<ConnectionId, 2> recipients = {1, 2};
  EXPECT_EQ(submission.run(recipients), FrameSendResult::QUEUED);
  EXPECT_EQ(submission.attempted, (std::vector<ConnectionId>{1, 2}));
  EXPECT_TRUE(submission.closed.empty());
}

TEST(FrameSubmission, RejectsAndClosesAllFailedReliableRecipients) {
  Submission submission = {.rejected = {1, 2}};
  const std::array<ConnectionId, 2> recipients = {1, 2};
  EXPECT_EQ(submission.run(recipients), FrameSendResult::REJECTED);
  EXPECT_EQ(submission.attempted, (std::vector<ConnectionId>{1, 2}));
  EXPECT_EQ(submission.closed, (std::vector<ConnectionId>{1, 2}));
}

TEST(FrameSubmission, PartialReliableBroadcastDoesNotRepeatAcceptedSubmissions) {
  const std::array<ConnectionId, 2> recipients = {1, 2};
  for (const auto failed : recipients) {
    Submission submission = {.rejected = {failed}};
    EXPECT_EQ(submission.run(recipients), FrameSendResult::QUEUED);
    EXPECT_EQ(submission.attempted, (std::vector<ConnectionId>{1, 2}));
    EXPECT_EQ(submission.closed, (std::vector<ConnectionId>{failed}));
  }
}

TEST(FrameSubmission, UnreliableRejectionLeavesConnectionsOpen) {
  const std::array<ConnectionId, 2> recipients = {1, 2};
  for (const auto lane : {TransportLane::REALTIME_CRITICAL, TransportLane::REALTIME_NORMAL,
                          TransportLane::REALTIME_BULK}) {
    Submission submission = {.rejected = {1, 2}};
    EXPECT_EQ(submission.run(recipients, FrameKind::GAMEPLAY, lane), FrameSendResult::REJECTED);
    EXPECT_EQ(submission.attempted, (std::vector<ConnectionId>{1, 2}));
    EXPECT_TRUE(submission.closed.empty());
  }
}

TEST(FrameSubmission, ControlAndBootstrapRejectionKeepTheirExistingClosePolicy) {
  const std::array<ConnectionId, 1> recipients = {1};
  for (const auto kind : {FrameKind::CONTROL, FrameKind::BOOTSTRAP}) {
    Submission submission = {.rejected = {1}};
    EXPECT_EQ(submission.run(recipients, kind, TransportLane::CONTROL_RELIABLE),
              FrameSendResult::REJECTED);
    EXPECT_TRUE(submission.closed.empty());
  }
}

}  // namespace
