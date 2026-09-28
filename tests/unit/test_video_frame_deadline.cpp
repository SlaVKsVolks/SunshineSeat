#include "src/video_frame_deadline.h"

#include <gtest/gtest.h>

using namespace std::chrono_literals;

TEST(VideoFrameDeadline, EncodeWorkDoesNotExtendEveryFrameInterval) {
  const auto epoch = std::chrono::steady_clock::time_point {};
  const auto period = 16666667ns;
  auto deadline = epoch + period;
  for (int frame = 1; frame <= 600; ++frame) {
    const auto started = deadline + 200us;
    const auto finished = started + 8ms;
    deadline = video::advance_minimum_frame_deadline(deadline, started, period, false);
    EXPECT_EQ(deadline, epoch + (frame + 1) * period);
    EXPECT_LT(deadline - finished, 9ms);
    EXPECT_GT(deadline - finished, 8ms);
  }
}

TEST(VideoFrameDeadline, FreshCaptureStartsANewCadence) {
  const auto epoch = std::chrono::steady_clock::time_point {};
  EXPECT_EQ(video::advance_minimum_frame_deadline(epoch + 16ms, epoch + 10ms, 16ms, true), epoch + 26ms);
}

TEST(VideoFrameDeadline, LateFreshCaptureDoesNotAccumulateConvertOrWakeupDelay) {
  const auto epoch = std::chrono::steady_clock::time_point {};
  const auto period = 16666667ns;
  auto deadline = epoch + period;
  for (int frame = 1; frame <= 600; ++frame) {
    const auto started = deadline + 1ms;
    deadline = video::advance_minimum_frame_deadline(deadline, started, period, true);
    ASSERT_EQ(deadline, epoch + (frame + 1) * period);
  }
}

TEST(VideoFrameDeadline, LongStallDoesNotCauseCatchUpBurst) {
  const auto epoch = std::chrono::steady_clock::time_point {};
  EXPECT_EQ(video::advance_minimum_frame_deadline(epoch + 16ms, epoch + 100ms, 16ms, false), epoch + 116ms);
}

TEST(VideoFrameDeadline, OneFullMissedIntervalRebases) {
  const auto epoch = std::chrono::steady_clock::time_point {};
  EXPECT_EQ(video::advance_minimum_frame_deadline(epoch + 16ms, epoch + 32ms, 16ms, false), epoch + 48ms);
}
