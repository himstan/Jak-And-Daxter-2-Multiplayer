#include "gtest/gtest.h"
#include "lsp/lsp_util.h"
#include "lsp/protocol/progress_report.h"

TEST(LSPProgress, ParseProgressLineWithElapsedTime) {
  std::string line = "[  2%] [goalc   ] 0.050 goal_src/jak2/multiplayer/core/mp-types.gc";
  auto event = lsp_util::parse_compile_progress_line(line);
  ASSERT_TRUE(event.has_value());
  EXPECT_EQ(event->percentage, 2);
  EXPECT_EQ(event->phase, "goalc");
  ASSERT_TRUE(event->elapsed_seconds.has_value());
  EXPECT_NEAR(event->elapsed_seconds.value(), 0.050, 0.001);
  EXPECT_EQ(event->file_path, "goal_src/jak2/multiplayer/core/mp-types.gc");
}

TEST(LSPProgress, ParseProgressLineWithoutElapsedTime) {
  std::string line = "[  2%] [goalc   ]       goal_src/jak2/engine/math/euler-h.gc";
  auto event = lsp_util::parse_compile_progress_line(line);
  ASSERT_TRUE(event.has_value());
  EXPECT_EQ(event->percentage, 2);
  EXPECT_EQ(event->phase, "goalc");
  EXPECT_FALSE(event->elapsed_seconds.has_value());
  EXPECT_EQ(event->file_path, "goal_src/jak2/engine/math/euler-h.gc");
}

TEST(LSPProgress, ParseProgressLine100Percent) {
  std::string line = "[100%] [goalc   ] 0.123 goal_src/jak2/some-file.gc";
  auto event = lsp_util::parse_compile_progress_line(line);
  ASSERT_TRUE(event.has_value());
  EXPECT_EQ(event->percentage, 100);
  EXPECT_EQ(event->phase, "goalc");
  EXPECT_EQ(event->file_path, "goal_src/jak2/some-file.gc");
}

TEST(LSPProgress, ParseUnrelatedLine) {
  std::string line = "Reader error:";
  auto event = lsp_util::parse_compile_progress_line(line);
  EXPECT_FALSE(event.has_value());
}

TEST(LSPProgress, ParseAnsiSafe) {
  // Example with some ANSI escapes (not exhaustive, just to test stripping)
  std::string line = "\x1B[32m[  5%]\x1B[0m [goalc   ] file.gc";
  auto event = lsp_util::parse_compile_progress_line(line);
  ASSERT_TRUE(event.has_value());
  EXPECT_EQ(event->percentage, 5);
  EXPECT_EQ(event->phase, "goalc");
  EXPECT_EQ(event->file_path, "file.gc");
}

TEST(LSPProgress, JsonBeginNotification) {
  LSPSpec::ProgressNotificationPayload notification;
  notification.token = "test-token";
  
  LSPSpec::WorkDoneProgressBegin begin;
  begin.title = "OpenGOAL compilation";
  begin.message = "Starting compilation...";
  begin.percentage = 0;
  begin.cancellable = false;
  notification.beginValue = begin;

  json j = notification;
  EXPECT_EQ(j["token"], "test-token");
  EXPECT_EQ(j["value"]["kind"], "begin");
  EXPECT_EQ(j["value"]["title"], "OpenGOAL compilation");
  EXPECT_EQ(j["value"]["message"], "Starting compilation...");
  EXPECT_EQ(j["value"]["percentage"], 0);
}

TEST(LSPProgress, JsonReportNotification) {
  LSPSpec::ProgressNotificationPayload notification;
  notification.token = "test-token";

  LSPSpec::WorkDoneProgressReport report;
  report.message = "goal_src/jak2/engine/math/euler-h.gc";
  report.percentage = 2;
  notification.reportValue = report;

  json j = notification;
  EXPECT_EQ(j["token"], "test-token");
  EXPECT_EQ(j["value"]["kind"], "report");
  EXPECT_EQ(j["value"]["message"], "goal_src/jak2/engine/math/euler-h.gc");
  EXPECT_EQ(j["value"]["percentage"], 2);
}

TEST(LSPProgress, JsonEndNotification) {
  LSPSpec::ProgressNotificationPayload notification;
  notification.token = "test-token";

  LSPSpec::WorkDoneProgressEnd end;
  end.message = "Compilation complete";
  notification.endValue = end;

  json j = notification;
  EXPECT_EQ(j["token"], "test-token");
  EXPECT_EQ(j["value"]["kind"], "end");
  EXPECT_EQ(j["value"]["message"], "Compilation complete");
}

TEST(LSPProgress, ParseTask1Inputs) {
  // Test input 1:
  // [  0%] [goalc   ]       goal_src/jak2/kernel/gcommon.gc
  {
    std::string line = "[  0%] [goalc   ]       goal_src/jak2/kernel/gcommon.gc";
    auto event = lsp_util::parse_compile_progress_line(line);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->percentage, 0);
    EXPECT_EQ(event->phase, "goalc");
    EXPECT_FALSE(event->elapsed_seconds.has_value());
    EXPECT_EQ(event->file_path, "goal_src/jak2/kernel/gcommon.gc");
  }

  // Test input 2:
  // [  2%] [goalc   ] 0.001 goal_src/jak2/engine/math/euler-h.gc
  {
    std::string line = "[  2%] [goalc   ] 0.001 goal_src/jak2/engine/math/euler-h.gc";
    auto event = lsp_util::parse_compile_progress_line(line);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->percentage, 2);
    EXPECT_EQ(event->phase, "goalc");
    ASSERT_TRUE(event->elapsed_seconds.has_value());
    EXPECT_NEAR(event->elapsed_seconds.value(), 0.001, 0.0001);
    EXPECT_EQ(event->file_path, "goal_src/jak2/engine/math/euler-h.gc");
  }

  // Test input with ANSI:
  // [  2%] [goalc   ] [38;2;255;255;000m0.071 [0mgoal_src/jak2/multiplayer/core/mp-types.gc
  {
    // The ANSI escapes in standard format include \x1B before the brackets.
    std::string line = "[  2%] [goalc   ] \x1B[38;2;255;255;000m0.071 \x1B[0mgoal_src/jak2/multiplayer/core/mp-types.gc";
    auto event = lsp_util::parse_compile_progress_line(line);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->percentage, 2);
    EXPECT_EQ(event->phase, "goalc");
    ASSERT_TRUE(event->elapsed_seconds.has_value());
    EXPECT_NEAR(event->elapsed_seconds.value(), 0.071, 0.0001);
    EXPECT_EQ(event->file_path, "goal_src/jak2/multiplayer/core/mp-types.gc");
  }

  // Test input:
  // [100%] [goalc   ]       goal_src/jak2/levels/outro/credits.gc
  {
    std::string line = "[100%] [goalc   ]       goal_src/jak2/levels/outro/credits.gc";
    auto event = lsp_util::parse_compile_progress_line(line);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->percentage, 100);
    EXPECT_EQ(event->phase, "goalc");
    EXPECT_FALSE(event->elapsed_seconds.has_value());
    EXPECT_EQ(event->file_path, "goal_src/jak2/levels/outro/credits.gc");
  }
}

TEST(LSPProgress, ProgressLifecycle) {
  std::vector<lsp_util::CompileProgressTracker::ProgressEvent> events;
  auto callback = [&](const lsp_util::CompileProgressTracker::ProgressEvent& event) {
    events.push_back(event);
  };

  lsp_util::CompileProgressTracker tracker("jak2", callback, true);
  tracker.start("Starting OpenGOAL indexing");

  // Send chunks simulating a build output stream
  tracker.handle_chunk("[  0%] [goalc   ]       goal_src/jak2/kernel/gcommon.gc\r");
  tracker.handle_chunk("[  1%] [goalc   ]       goal_src/jak2/kernel/pskernel.gc\n");
  // Test repeat percentage (should be throttled / not emitted again)
  tracker.handle_chunk("[  1%] [goalc   ] 0.005 goal_src/jak2/kernel/pskernel.gc\n");
  tracker.handle_chunk("[  2%] [goalc   ] 0.050 goal_src/jak2/multiplayer/core/mp-types.gc\n");
  tracker.handle_chunk("[ 99%] [goalc   ]       goal_src/jak2/levels/outro/credits-h.gc\r");
  // Test ANSI stripping in the middle of a chunk
  tracker.handle_chunk("[100%] [goalc   ] \x1B[38;2;255;255;000m0.071 \x1B[0mgoal_src/jak2/levels/outro/credits.gc\n");

  tracker.finish("OpenGOAL indexing complete");

  // Verify outgoing messages
  // We expect:
  // 1. create
  // 2. begin (percentage 0)
  // 3. report (percentage 1)
  // 4. report (percentage 2)
  // 5. report (percentage 99)
  // 6. report (percentage 100)
  // 7. end
  ASSERT_EQ(events.size(), 7);

  EXPECT_EQ(events[0].kind, "create");
  EXPECT_EQ(events[0].message, "Starting OpenGOAL indexing");
  EXPECT_FALSE(events[0].token.empty());

  std::string token = events[0].token;

  EXPECT_EQ(events[1].kind, "begin");
  EXPECT_EQ(events[1].token, token);
  EXPECT_EQ(events[1].percentage, 0);
  EXPECT_EQ(events[1].message, "Starting OpenGOAL indexing");

  EXPECT_EQ(events[2].kind, "report");
  EXPECT_EQ(events[2].token, token);
  EXPECT_EQ(events[2].percentage, 1);
  EXPECT_EQ(events[2].message, "goal_src/jak2/kernel/pskernel.gc");

  EXPECT_EQ(events[3].kind, "report");
  EXPECT_EQ(events[3].token, token);
  EXPECT_EQ(events[3].percentage, 2);
  EXPECT_EQ(events[3].message, "goal_src/jak2/multiplayer/core/mp-types.gc");

  EXPECT_EQ(events[4].kind, "report");
  EXPECT_EQ(events[4].token, token);
  EXPECT_EQ(events[4].percentage, 99);
  EXPECT_EQ(events[4].message, "goal_src/jak2/levels/outro/credits-h.gc");

  EXPECT_EQ(events[5].kind, "report");
  EXPECT_EQ(events[5].token, token);
  EXPECT_EQ(events[5].percentage, 100);
  EXPECT_EQ(events[5].message, "goal_src/jak2/levels/outro/credits.gc");

  EXPECT_EQ(events[6].kind, "end");
  EXPECT_EQ(events[6].token, token);
  EXPECT_EQ(events[6].message, "OpenGOAL indexing complete");
}

TEST(LSPProgress, ProgressLifecycleDisabled) {
  std::vector<lsp_util::CompileProgressTracker::ProgressEvent> events;
  auto callback = [&](const lsp_util::CompileProgressTracker::ProgressEvent& event) {
    events.push_back(event);
  };

  lsp_util::CompileProgressTracker tracker("jak2", callback, false);
  tracker.start("Starting OpenGOAL indexing");
  tracker.handle_chunk("[  0%] [goalc   ]       goal_src/jak2/kernel/gcommon.gc\n");
  tracker.finish("OpenGOAL indexing complete");

  EXPECT_TRUE(events.empty());
}


