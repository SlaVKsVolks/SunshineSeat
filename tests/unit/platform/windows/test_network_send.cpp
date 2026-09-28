/**
 * @file tests/unit/platform/windows/test_network_send.cpp
 * @brief Test Windows video send fallback policy.
 */
#ifdef _WIN32

#include <WinSock2.h>

#include <gtest/gtest.h>

#include <src/platform/windows/misc.h>

TEST(WindowsSendReliabilityTests, RetriesWithoutAncillaryControlForInvalidMetadata) {
  EXPECT_TRUE(platf::should_retry_send_without_control(WSAEINVAL));
  EXPECT_FALSE(platf::should_retry_send_without_control(WSAEWOULDBLOCK));
  EXPECT_FALSE(platf::should_retry_send_without_control(WSAENOBUFS));
}

TEST(WindowsSendReliabilityTests, ReportsTransientQueuePressureForKeyframeBatches) {
  EXPECT_TRUE(platf::should_report_keyframe_batch_queue_pressure(true, WSAEWOULDBLOCK));
  EXPECT_TRUE(platf::should_report_keyframe_batch_queue_pressure(true, WSAENOBUFS));
  EXPECT_TRUE(platf::should_report_keyframe_batch_queue_pressure(true, WSAETIMEDOUT));
}

TEST(WindowsSendReliabilityTests, KeepsNonKeyframeQueuePressureOnQuietDropPath) {
  EXPECT_FALSE(platf::should_report_keyframe_batch_queue_pressure(false, WSAEWOULDBLOCK));
  EXPECT_FALSE(platf::should_report_keyframe_batch_queue_pressure(false, WSAENOBUFS));
  EXPECT_FALSE(platf::should_report_keyframe_batch_queue_pressure(false, WSAETIMEDOUT));
}

TEST(WindowsSendReliabilityTests, DoesNotMisclassifyMetadataErrorsAsQueuePressure) {
  EXPECT_FALSE(platf::should_report_keyframe_batch_queue_pressure(true, WSAEINVAL));
}

TEST(WindowsSendReliabilityTests, FallsBackBeforeMultiPacketUdpSegmentationOffload) {
  EXPECT_FALSE(platf::should_fallback_to_unbatched_windows_udp_send(1));
  EXPECT_TRUE(platf::should_fallback_to_unbatched_windows_udp_send(2));
  EXPECT_TRUE(platf::should_fallback_to_unbatched_windows_udp_send(8));
}

#endif
