/**
 * @file tests/unit/test_stream.cpp
 * @brief Test src/stream.*
 */

#include "../../src/client_mic.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace stream {
  struct clipboard_message_t {
    std::uint8_t origin;
    std::uint64_t origin_id;
    std::uint64_t content_hash;
    std::string text;
  };

  struct clipboard_image_chunk_t {
    std::uint8_t origin;
    std::uint8_t format;
    std::uint64_t origin_id;
    std::uint64_t content_hash;
    std::uint64_t transfer_id;
    std::uint32_t total_length;
    std::uint32_t offset;
    std::string data;
  };

  std::uint64_t clipboard_text_hash(const std::string_view &text);
  std::uint64_t clipboard_image_hash(const std::string_view &image);
  bool decode_clipboard_message(const std::string_view &payload, clipboard_message_t &message);
  bool decode_clipboard_image_chunk(const std::string_view &payload, clipboard_image_chunk_t &chunk);
  bool should_apply_clipboard_update(std::uint64_t incoming_hash, std::uint64_t last_applied_hash);
  bool should_drop_video_packet(bool broadcast_shutdown, bool session_running);
  bool should_release_virtual_display_allocation(std::size_t active_sessions, std::size_t pending_launch_sessions);
  std::optional<int> decode_adaptive_bitrate_request(const std::string_view &payload);
  enum class adaptive_bitrate_action_e : std::uint8_t {
    ignore,
    defer,
    apply,
  };
  adaptive_bitrate_action_e evaluate_adaptive_bitrate_request(
    int current_kbps,
    int target_kbps,
    std::chrono::steady_clock::duration target_age,
    std::chrono::steady_clock::duration time_since_last_apply
  );
  struct adaptive_bitrate_policy_state_t {
    std::optional<int> pending_target_kbps;
    std::optional<std::chrono::steady_clock::time_point> pending_target_since;
    std::optional<std::chrono::steady_clock::time_point> last_applied_at;
  };
  std::optional<int> select_adaptive_bitrate_target(
    adaptive_bitrate_policy_state_t &state,
    int current_kbps,
    int target_kbps,
    std::chrono::steady_clock::time_point now
  );
  std::vector<uint8_t> concat_and_insert(uint64_t insert_size, uint64_t slice_size, const std::string_view &data1, const std::string_view &data2);
  std::pair<size_t, size_t> calculate_video_send_pacing(size_t blocksize, int target_bitrate_kbps, int fec_percentage);
  int effective_video_send_bitrate_kbps(int requested_bitrate_kbps, int configured_max_bitrate_kbps);
  bool should_enter_video_fec_fallback(std::chrono::steady_clock::duration elapsed);
  int effective_video_fec_percentage(int configured_fec_percentage, bool fallback_active);
  int effective_video_keyframe_fec_percentage(int configured_fec_percentage, bool fallback_active);
  size_t bounded_video_fec_parity_shards(size_t data_shards, size_t fec_percentage, size_t min_parity_shards);
  size_t bounded_video_fec_parity_shards(
    size_t data_shards,
    size_t fec_percentage,
    size_t min_parity_shards,
    size_t parity_capacity_floor
  );
  size_t dual_display_video_parity_capacity_floor(bool dual_display_video_tracks, size_t data_shards);
  std::chrono::steady_clock::duration calculate_video_send_frame_budget(size_t frame_packets_sent, size_t packets_per_ms);
  size_t calculate_video_send_frame_burst_packets_per_ms(size_t sustained_packets_per_ms, size_t frame_packets);
  size_t calculate_video_send_frame_batch_size(size_t packets_per_ms);
  size_t calculate_video_send_frame_batch_size(size_t packets_per_ms, bool dual_display_video_tracks);
  size_t calculate_video_send_frame_batch_size_for_frame(
    size_t packets_per_ms,
    bool dual_display_video_tracks,
    bool keyframe
  );
  std::chrono::steady_clock::time_point calculate_video_pacing_next_frame_start(
    std::chrono::steady_clock::time_point scheduled_start,
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::duration maximum_interframe_delay
  );
  std::chrono::steady_clock::time_point calculate_video_pacing_frame_start(
    std::chrono::steady_clock::time_point scheduled_start,
    std::chrono::steady_clock::time_point now
  );
  size_t calculate_video_send_catch_up_credit(
    std::chrono::steady_clock::duration late_by,
    size_t packets_per_ms,
    std::chrono::steady_clock::duration maximum_credit
  );
  size_t calculate_video_send_catch_up_burst_size(size_t catch_up_credit, size_t send_batch_size);
  bool should_log_video_pacing_late_warning(
    std::chrono::steady_clock::duration late_by,
    size_t frame_packets_sent,
    std::chrono::steady_clock::duration max_idle_credit,
    std::chrono::steady_clock::duration slow_late_threshold
  );
  bool should_emit_video_pacing_late_warning(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point last_warning,
    std::chrono::steady_clock::duration minimum_interval
  );
  bool should_log_video_send_frame_warning(
    std::chrono::steady_clock::duration elapsed,
    size_t frame_packets_sent,
    size_t packets_per_ms,
    std::chrono::steady_clock::duration minimum_warning_threshold,
    std::chrono::steady_clock::duration expected_send_slack
  );
  bool should_emit_video_stream_heartbeat(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point last_heartbeat,
    std::chrono::steady_clock::duration minimum_interval
  );
  std::string video_keyframe_send_summary(
    int track_index,
    std::uint64_t frame_index,
    std::size_t fec_blocks,
    std::size_t shards,
    std::size_t batches,
    std::size_t fallback_batches,
    std::uint16_t sequence_first,
    std::uint16_t sequence_last
  );
}  // namespace stream

namespace video {
  bool should_accept_single_track_idr_request(
    bool idr_pending,
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point last_idr_request,
    std::chrono::steady_clock::duration quiet_period
  );
  bool should_drop_stale_video_frame(
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp,
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::duration maximum_age
  );
  bool should_reinitialize_stalled_video_capture(
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp,
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::duration reinitialize_age
  );
  bool should_reinitialize_stalled_video_capture(
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp,
    std::chrono::steady_clock::time_point now,
    std::optional<std::chrono::steady_clock::time_point> stale_since,
    std::chrono::steady_clock::duration reinitialize_age
  );
  int video_stage_severity(std::chrono::steady_clock::duration elapsed);
  bool should_abandon_video_encode_generation(std::chrono::steady_clock::duration elapsed);
  bool should_abort_encoder_teardown_wait(
    bool shutdown_requested,
    std::chrono::steady_clock::duration elapsed,
    std::chrono::steady_clock::duration maximum_wait
  );
  bool should_skip_avcodec_teardown_flush(bool stream_shutdown, int stage_severity);
}  // namespace video

namespace input {
  bool should_continue_key_repeat_for(
    std::chrono::steady_clock::duration elapsed,
    std::chrono::steady_clock::duration maximum_duration
  );
}

namespace rtsp_stream {
  bool pending_launch_clients_match(
    std::string_view existing_unique_id,
    std::string_view existing_client_cert,
    std::string_view existing_remote_address,
    std::string_view incoming_unique_id,
    std::string_view incoming_client_cert,
    std::string_view incoming_remote_address
  );
  bool pending_launch_session_is_current(bool pending_superseded);
}  // namespace rtsp_stream

#include "../tests_common.h"

TEST(VideoReliabilityTests, DropsCapturedFramesOlderThanTheLatencyBudget) {
  const auto now = std::chrono::steady_clock::time_point {1s};

  EXPECT_TRUE(video::should_drop_stale_video_frame(now - 101ms, now, 100ms));
  EXPECT_FALSE(video::should_drop_stale_video_frame(now - 100ms, now, 100ms));
  EXPECT_FALSE(video::should_drop_stale_video_frame(now + 1ms, now, 100ms));
  EXPECT_FALSE(video::should_drop_stale_video_frame(std::nullopt, now, 100ms));
}

TEST(VideoReliabilityTests, ReinitializesCaptureForLongSurfaceStarvation) {
  const auto now = std::chrono::steady_clock::time_point {10s};

  EXPECT_FALSE(video::should_reinitialize_stalled_video_capture(now - 999ms, now, 1s));
  EXPECT_TRUE(video::should_reinitialize_stalled_video_capture(now - 1001ms, now, 1s));
  EXPECT_FALSE(video::should_reinitialize_stalled_video_capture(std::nullopt, now, 1s));
}

TEST(VideoReliabilityTests, RequiresPersistentStaleSurfaceBeforeCaptureReinitialization) {
  const auto now = std::chrono::steady_clock::time_point {20s};

  EXPECT_FALSE(video::should_reinitialize_stalled_video_capture(now - 10s, now, now - 100ms, 3s));
  EXPECT_FALSE(video::should_reinitialize_stalled_video_capture(now - 1500ms, now, now - 1500ms, 3s));
  EXPECT_TRUE(video::should_reinitialize_stalled_video_capture(now - 3001ms, now, now - 3001ms, 3s));
  EXPECT_FALSE(video::should_reinitialize_stalled_video_capture(now - 10s, now, std::nullopt, 3s));
}

TEST(VideoReliabilityTests, ClassifiesStageStallsAtOperationalThresholds) {
  EXPECT_EQ(video::video_stage_severity(249ms), 0);
  EXPECT_EQ(video::video_stage_severity(250ms), 1);
  EXPECT_EQ(video::video_stage_severity(499ms), 1);
  EXPECT_EQ(video::video_stage_severity(500ms), 2);
  EXPECT_EQ(video::video_stage_severity(1499ms), 2);
  EXPECT_EQ(video::video_stage_severity(1500ms), 3);
}

TEST(VideoReliabilityTests, KeepsTransientEncoderPausesWithinTheCurrentGeneration) {
  EXPECT_FALSE(video::should_abandon_video_encode_generation(1500ms));
  EXPECT_FALSE(video::should_abandon_video_encode_generation(2601ms));
  EXPECT_TRUE(video::should_abandon_video_encode_generation(5000ms));
}

TEST(VideoReliabilityTests, AbortsEncoderTeardownWaitOnShutdownOrTimeout) {
  EXPECT_FALSE(video::should_abort_encoder_teardown_wait(false, 9999ms, 10s));
  EXPECT_TRUE(video::should_abort_encoder_teardown_wait(false, 10s, 10s));
  EXPECT_TRUE(video::should_abort_encoder_teardown_wait(true, 1ms, 10s));
}

TEST(VideoReliabilityTests, SkipsAvcodecFlushOnlyForShutdownOrAbandonedGeneration) {
  EXPECT_FALSE(video::should_skip_avcodec_teardown_flush(false, 0));
  EXPECT_FALSE(video::should_skip_avcodec_teardown_flush(false, 1));
  EXPECT_FALSE(video::should_skip_avcodec_teardown_flush(false, 2));
  EXPECT_TRUE(video::should_skip_avcodec_teardown_flush(false, 3));
  EXPECT_TRUE(video::should_skip_avcodec_teardown_flush(true, 0));
}

TEST(VideoReliabilityTests, FecFallbackDisablesOnlyAfterSendPathPressure) {
  EXPECT_FALSE(stream::should_enter_video_fec_fallback(49ms));
  EXPECT_TRUE(stream::should_enter_video_fec_fallback(50ms));
  EXPECT_EQ(stream::effective_video_fec_percentage(20, false), 20);
  EXPECT_EQ(stream::effective_video_fec_percentage(20, true), 0);
  EXPECT_EQ(stream::effective_video_fec_percentage(1, false), 0);
  EXPECT_EQ(stream::effective_video_fec_percentage(1, true), 0);
  EXPECT_EQ(stream::effective_video_fec_percentage(300, false), 255);
}

TEST(VideoReliabilityTests, DoesNotAddImplicitKeyframeFecToTheNoParityProfile) {
  EXPECT_EQ(stream::effective_video_keyframe_fec_percentage(1, false), 0);
  EXPECT_EQ(stream::effective_video_keyframe_fec_percentage(20, false), 20);
  EXPECT_EQ(stream::effective_video_keyframe_fec_percentage(1, true), 0);
}

TEST(VideoReliabilityTests, CoalescesSingleTrackIdrRequestsDuringQuietPeriod) {
  using namespace std::chrono_literals;

  const auto start = std::chrono::steady_clock::time_point {};
  const auto never_requested = std::chrono::steady_clock::time_point::min();

  EXPECT_TRUE(video::should_accept_single_track_idr_request(true, start, never_requested, 1s));
  EXPECT_FALSE(video::should_accept_single_track_idr_request(true, start + 999ms, start, 1s));
  EXPECT_TRUE(video::should_accept_single_track_idr_request(true, start + 1s, start, 1s));
  EXPECT_FALSE(video::should_accept_single_track_idr_request(false, start + 2s, start, 1s));
}

TEST(VideoReliabilityTests, DropsQueuedVideoAfterShutdownOrSessionTermination) {
  EXPECT_FALSE(stream::should_drop_video_packet(false, true));
  EXPECT_TRUE(stream::should_drop_video_packet(true, true));
  EXPECT_TRUE(stream::should_drop_video_packet(false, false));
}

TEST(VideoReliabilityTests, PendingRtspLaunchKeepsPreflightVirtualDisplayAlive) {
  EXPECT_FALSE(stream::should_release_virtual_display_allocation(0, 1));
  EXPECT_FALSE(stream::should_release_virtual_display_allocation(1, 0));
  EXPECT_TRUE(stream::should_release_virtual_display_allocation(0, 0));
}

TEST(VideoReliabilityTests, BoundsFecParityToAvoidMinimumShardInflation) {
  EXPECT_EQ(stream::bounded_video_fec_parity_shards(0, 8, 2), 0U);
  EXPECT_EQ(stream::bounded_video_fec_parity_shards(14, 8, 2), 1U);
  EXPECT_EQ(stream::bounded_video_fec_parity_shards(100, 8, 2), 8U);
  EXPECT_EQ(stream::bounded_video_fec_parity_shards(14, 1, 2), 1U);
  EXPECT_EQ(stream::bounded_video_fec_parity_shards(1, 8, 2), 1U);
  EXPECT_EQ(stream::bounded_video_fec_parity_shards(14, 0, 2), 0U);
}

TEST(VideoReliabilityTests, GivesDualDisplaySmallFramesFourParityShards) {
  const auto dual_floor = stream::dual_display_video_parity_capacity_floor(true, 12);
  EXPECT_EQ(dual_floor, 4U);
  EXPECT_EQ(stream::bounded_video_fec_parity_shards(12, 8, 1, dual_floor), 4U);
  EXPECT_EQ(stream::bounded_video_fec_parity_shards(13, 8, 1, dual_floor), 4U);
  EXPECT_EQ(stream::bounded_video_fec_parity_shards(14, 8, 1, dual_floor), 4U);
  EXPECT_EQ(stream::bounded_video_fec_parity_shards(14, 8, 1), 1U);
  EXPECT_EQ(stream::bounded_video_fec_parity_shards(14, 0, 1, dual_floor), 0U);
  EXPECT_EQ(stream::dual_display_video_parity_capacity_floor(true, 1), 1U);
  EXPECT_EQ(stream::dual_display_video_parity_capacity_floor(false, 14), 1U);
}

TEST(VideoReliabilityTests, DocumentsTinyFecBlockMinimumMultiplier) {
  const auto parity_shards = stream::bounded_video_fec_parity_shards(1, 8, 2);
  ASSERT_EQ(parity_shards, 1U);
  EXPECT_EQ((100U * parity_shards) / 1U, 100U);
}

TEST(VideoReliabilityTests, ReportsTrackSpecificKeyframeSendBoundaries) {
  EXPECT_EQ(
    stream::video_keyframe_send_summary(1, 71, 2, 16, 4, 0, 65530, 9),
    "Video keyframe send summary [track=1, frame=71, fec_blocks=2, shards=16, batches=4, fallback_batches=0, sequence_first=65530, sequence_last=9]"
  );
}

TEST(InputReliabilityTests, StopsSyntheticKeyRepeatAfterWatchdogBudget) {
  using namespace std::chrono_literals;

  EXPECT_TRUE(input::should_continue_key_repeat_for(1999ms, 2000ms));
  EXPECT_FALSE(input::should_continue_key_repeat_for(2000ms, 2000ms));
  EXPECT_FALSE(input::should_continue_key_repeat_for(5000ms, 2000ms));
}

TEST(RtspPendingLaunchTests, SupersedesRetriesFromTheSamePairedClient) {
  EXPECT_TRUE(rtsp_stream::pending_launch_clients_match("client-a", "cert-a", "203.0.113.10", "client-a", "cert-a", "203.0.113.11"));
  EXPECT_TRUE(rtsp_stream::pending_launch_clients_match("", "cert-a", "203.0.113.10", "", "cert-a", "203.0.113.11"));
}

TEST(RtspPendingLaunchTests, DoesNotMergeDifferentPairedClientsSharingAnAddress) {
  EXPECT_FALSE(rtsp_stream::pending_launch_clients_match("client-a", "cert-a", "203.0.113.10", "client-b", "cert-b", "203.0.113.10"));
  EXPECT_FALSE(rtsp_stream::pending_launch_clients_match("", "", "203.0.113.10", "", "", "203.0.113.10"));
}

TEST(RtspPendingLaunchTests, SupersededRetriesDoNotBlockFreshAllocation) {
  EXPECT_TRUE(rtsp_stream::pending_launch_session_is_current(false));
  EXPECT_FALSE(rtsp_stream::pending_launch_session_is_current(true));
}

TEST(ConcatAndInsertTests, ConcatNoInsertionTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(0, 2, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatLargeStrideTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(1, sizeof(b1) + sizeof(b2) + 1, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {0, 'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatSmallStrideTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(1, 1, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {0, 'a', 0, 'b', 0, 'c', 0, 'd', 0, 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ClipboardSyncTests, HashIsStableAndLoopGuardRejectsTheSameContent) {
  const auto hash = stream::clipboard_text_hash("hello");

  EXPECT_EQ(hash, stream::clipboard_text_hash("hello"));
  EXPECT_NE(hash, stream::clipboard_text_hash("goodbye"));
  EXPECT_FALSE(stream::should_apply_clipboard_update(hash, hash));
  EXPECT_TRUE(stream::should_apply_clipboard_update(hash, 0));
}

TEST(ClipboardSyncTests, DecodesBoundedUtf8Envelope) {
  std::string payload;
  payload.push_back('\x01');  // version
  payload.push_back('\x02');  // client origin
  payload.append("\0\0", 2);

  const std::uint64_t origin_id = 0x0102030405060708ULL;
  const std::uint64_t content_hash = stream::clipboard_text_hash("Olá");
  const std::uint32_t length = 4;
  for (std::uint64_t value : {origin_id, content_hash}) {
    for (int shift = 0; shift < 64; shift += 8) {
      payload.push_back(static_cast<char>((value >> shift) & 0xff));
    }
  }
  for (int shift = 0; shift < 32; shift += 8) {
    payload.push_back(static_cast<char>((length >> shift) & 0xff));
  }
  payload.append("Ol\xc3\xa1", length);

  stream::clipboard_message_t message {};
  ASSERT_TRUE(stream::decode_clipboard_message(payload, message));
  EXPECT_EQ(message.origin, 2);
  EXPECT_EQ(message.origin_id, origin_id);
  EXPECT_EQ(message.content_hash, content_hash);
  EXPECT_EQ(message.text, "Olá");

  std::fill(payload.begin() + 4, payload.begin() + 12, '\0');
  EXPECT_FALSE(stream::decode_clipboard_message(payload, message));
}

TEST(AdaptiveBitrateControlTests, DecodesBoundedLittleEndianBitrateRequest) {
  const std::string payload {"\x20\x4e\x00\x00", 4};

  const auto bitrate = stream::decode_adaptive_bitrate_request(payload);

  ASSERT_TRUE(bitrate.has_value());
  EXPECT_EQ(*bitrate, 20000);
}

TEST(AdaptiveBitrateControlTests, RejectsMalformedBitrateRequest) {
  EXPECT_FALSE(stream::decode_adaptive_bitrate_request(std::string_view {"\x20\x4e", 2}).has_value());
  EXPECT_FALSE(stream::decode_adaptive_bitrate_request(std::string_view {"\x00\x00\x00\x00", 4}).has_value());
}

TEST(AdaptiveBitrateControlTests, IgnoresChangesSmallerThanTheDeadband) {
  using namespace std::chrono_literals;

  EXPECT_EQ(
    stream::evaluate_adaptive_bitrate_request(20000, 20400, 2min, 2min),
    stream::adaptive_bitrate_action_e::ignore
  );
}

TEST(AdaptiveBitrateControlTests, DefersNonCriticalChangesUntilTheTargetHasDwelled) {
  using namespace std::chrono_literals;

  EXPECT_EQ(
    stream::evaluate_adaptive_bitrate_request(20000, 22000, 29s, 2min),
    stream::adaptive_bitrate_action_e::defer
  );
  EXPECT_EQ(
    stream::evaluate_adaptive_bitrate_request(20000, 22000, 30s, 2min),
    stream::adaptive_bitrate_action_e::apply
  );
}

TEST(AdaptiveBitrateControlTests, KeepsAConfirmedNonCriticalChangeInCooldown) {
  using namespace std::chrono_literals;

  EXPECT_EQ(
    stream::evaluate_adaptive_bitrate_request(18000, 19800, 45s, 59s),
    stream::adaptive_bitrate_action_e::defer
  );
  EXPECT_EQ(
    stream::evaluate_adaptive_bitrate_request(18000, 19800, 45s, 60s),
    stream::adaptive_bitrate_action_e::apply
  );
}

TEST(AdaptiveBitrateControlTests, AppliesAnUrgentTenPercentReductionImmediately) {
  using namespace std::chrono_literals;

  EXPECT_EQ(
    stream::evaluate_adaptive_bitrate_request(20000, 18000, 0s, 0s),
    stream::adaptive_bitrate_action_e::apply
  );
}

TEST(AdaptiveBitrateControlTests, PreservesAStableCandidateAcrossDwellAndCooldown) {
  using namespace std::chrono_literals;

  stream::adaptive_bitrate_policy_state_t state;
  const auto started = std::chrono::steady_clock::time_point {};

  EXPECT_FALSE(stream::select_adaptive_bitrate_target(state, 20000, 22000, started).has_value());
  EXPECT_FALSE(stream::select_adaptive_bitrate_target(state, 20000, 22000, started + 29s).has_value());
  EXPECT_EQ(stream::select_adaptive_bitrate_target(state, 20000, 22000, started + 30s), 22000);

  EXPECT_FALSE(stream::select_adaptive_bitrate_target(state, 22000, 24200, started + 31s).has_value());
  EXPECT_FALSE(stream::select_adaptive_bitrate_target(state, 22000, 24200, started + 89s).has_value());
  EXPECT_EQ(stream::select_adaptive_bitrate_target(state, 22000, 24200, started + 90s), 24200);
}

TEST(AdaptiveBitrateControlTests, AllowsUrgentReductionDuringCooldown) {
  using namespace std::chrono_literals;

  stream::adaptive_bitrate_policy_state_t state;
  const auto started = std::chrono::steady_clock::time_point {};
  ASSERT_FALSE(stream::select_adaptive_bitrate_target(state, 20000, 22000, started).has_value());
  ASSERT_EQ(stream::select_adaptive_bitrate_target(state, 20000, 22000, started + 30s), 22000);

  EXPECT_EQ(stream::select_adaptive_bitrate_target(state, 22000, 19800, started + 31s), 19800);
}

TEST(ClipboardSyncTests, RejectsInvalidUtf8AndOversizedPayload) {
  std::string invalid = "\x01\x02\0\0";
  invalid.append(16, '\0');
  invalid.append("\x02\xc0\xaf", 3);

  stream::clipboard_message_t message {};
  EXPECT_FALSE(stream::decode_clipboard_message(invalid, message));

  std::string oversized(24, '\0');
  oversized[0] = 1;
  oversized[1] = 2;
  const std::uint32_t length = 65537;
  for (int shift = 0; shift < 32; shift += 8) {
    oversized[20 + shift / 8] = static_cast<char>((length >> shift) & 0xff);
  }
  EXPECT_FALSE(stream::decode_clipboard_message(oversized, message));
}

TEST(ClipboardImageSyncTests, DecodesBoundedBmpChunkAndRejectsTampering) {
  const std::string image {
    "BM\x3a\x00\x00\x00\x00\x00\x00\x00\x36\x00\x00\x00"
    "\x28\x00\x00\x00\x01\x00\x00\x00\x01\x00\x00\x00\x01\x00"
    "\x18\x00\x00\x00\x00\x00\x04\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xff\x00\x00\x00",
    58
  };
  std::string payload(40, '\0');
  payload[0] = 1;
  payload[1] = 1;
  payload[2] = 1;
  const std::uint64_t origin_id = 0x0102030405060708ULL;
  const std::uint64_t content_hash = stream::clipboard_image_hash(image);
  const std::uint64_t transfer_id = 0x1112131415161718ULL;
  const std::uint32_t total_length = static_cast<std::uint32_t>(image.size());
  const std::uint32_t offset = 0;
  const std::uint16_t chunk_length = static_cast<std::uint16_t>(image.size());
  for (int shift = 0; shift < 64; shift += 8) {
    payload[4 + shift / 8] = static_cast<char>((origin_id >> shift) & 0xff);
    payload[12 + shift / 8] = static_cast<char>((content_hash >> shift) & 0xff);
    payload[20 + shift / 8] = static_cast<char>((transfer_id >> shift) & 0xff);
  }
  for (int shift = 0; shift < 32; shift += 8) {
    payload[28 + shift / 8] = static_cast<char>((total_length >> shift) & 0xff);
    payload[32 + shift / 8] = static_cast<char>((offset >> shift) & 0xff);
  }
  payload[36] = static_cast<char>(chunk_length & 0xff);
  payload[37] = static_cast<char>((chunk_length >> 8) & 0xff);
  payload.append(image);

  stream::clipboard_image_chunk_t chunk {};
  ASSERT_TRUE(stream::decode_clipboard_image_chunk(payload, chunk));
  EXPECT_EQ(chunk.origin, 1);
  EXPECT_EQ(chunk.format, 1);
  EXPECT_EQ(chunk.origin_id, origin_id);
  EXPECT_EQ(chunk.content_hash, content_hash);
  EXPECT_EQ(chunk.transfer_id, transfer_id);
  EXPECT_EQ(chunk.total_length, total_length);
  EXPECT_EQ(chunk.offset, offset);
  EXPECT_EQ(chunk.data, image);

  payload[36] = static_cast<char>(chunk_length - 1);
  EXPECT_FALSE(stream::decode_clipboard_image_chunk(payload, chunk));
}

TEST(ClipboardImageSyncTests, RejectsNonBmpAndOversizedChunks) {
  std::string payload(40, '\0');
  payload[0] = 1;
  payload[1] = 2;
  payload[2] = 1;
  payload[36] = 1;
  payload.append("x", 1);

  stream::clipboard_image_chunk_t chunk {};
  EXPECT_FALSE(stream::decode_clipboard_image_chunk(payload, chunk));
}

TEST(ClipboardImageSyncTests, AllowsFullHdBmpTransportSize) {
  constexpr std::uint32_t full_hd_bmp_size = 8 * 1024 * 1024;
  std::string payload(40, '\0');
  payload[0] = 1;
  payload[1] = 2;
  payload[2] = 1;
  payload[4] = 1;
  payload[20] = 1;
  for (int shift = 0; shift < 32; shift += 8) {
    payload[28 + shift / 8] = static_cast<char>((full_hd_bmp_size >> shift) & 0xff);
  }
  payload[36] = 1;
  payload.append("x", 1);

  stream::clipboard_image_chunk_t chunk {};
  ASSERT_TRUE(stream::decode_clipboard_image_chunk(payload, chunk));
  EXPECT_EQ(chunk.total_length, full_hd_bmp_size);
  EXPECT_EQ(chunk.offset, 0);
  EXPECT_EQ(chunk.data, "x");
}

TEST(ClientMicProtocolTests, DecodesBoundedOpusEnvelope) {
  std::string payload(client_mic::MESSAGE_HEADER_SIZE, '\0');
  payload[0] = static_cast<char>(client_mic::MESSAGE_VERSION);
  payload[1] = static_cast<char>(client_mic::CODEC_OPUS);
  payload[2] = static_cast<char>(client_mic::CHANNELS_MONO);
  const std::uint32_t sequence = 42;
  const std::uint16_t encoded_length = 3;
  for (int shift = 0; shift < 32; shift += 8) {
    payload[4 + shift / 8] = static_cast<char>((sequence >> shift) & 0xff);
  }
  payload[8] = static_cast<char>(client_mic::FRAME_SAMPLES & 0xff);
  payload[9] = static_cast<char>((client_mic::FRAME_SAMPLES >> 8) & 0xff);
  payload[10] = static_cast<char>(encoded_length & 0xff);
  payload[11] = static_cast<char>((encoded_length >> 8) & 0xff);
  for (int shift = 0; shift < 32; shift += 8) {
    payload[12 + shift / 8] = static_cast<char>((client_mic::SAMPLE_RATE >> shift) & 0xff);
  }
  payload.append("abc", encoded_length);

  client_mic::frame_t frame {};
  ASSERT_TRUE(client_mic::decode_frame(payload, frame));
  EXPECT_EQ(frame.sequence, sequence);
  EXPECT_EQ(frame.sample_count, client_mic::FRAME_SAMPLES);
  EXPECT_EQ(frame.encoded, "abc");
}

TEST(ClientMicProtocolTests, RejectsMalformedOrOversizedEnvelope) {
  client_mic::frame_t frame {};
  EXPECT_FALSE(client_mic::decode_frame(std::string(client_mic::MESSAGE_HEADER_SIZE - 1, '\0'), frame));

  std::string payload(client_mic::MESSAGE_HEADER_SIZE + 1, '\0');
  payload[0] = static_cast<char>(client_mic::MESSAGE_VERSION);
  payload[1] = static_cast<char>(client_mic::CODEC_OPUS);
  payload[2] = static_cast<char>(client_mic::CHANNELS_MONO);
  payload[8] = static_cast<char>(client_mic::FRAME_SAMPLES & 0xff);
  payload[9] = static_cast<char>((client_mic::FRAME_SAMPLES >> 8) & 0xff);
  payload[10] = 2;  // Declared length does not match the wire payload.
  payload[12] = static_cast<char>(client_mic::SAMPLE_RATE & 0xff);
  payload[13] = static_cast<char>((client_mic::SAMPLE_RATE >> 8) & 0xff);
  payload[14] = static_cast<char>((client_mic::SAMPLE_RATE >> 16) & 0xff);
  payload[15] = static_cast<char>((client_mic::SAMPLE_RATE >> 24) & 0xff);
  EXPECT_FALSE(client_mic::decode_frame(payload, frame));
}

TEST(ClientMicRoutingTests, MatchesVirtualCableCaptureCompanion) {
  EXPECT_TRUE(client_mic::is_matching_capture_endpoint("Moonlight Output (VB-Audio Virtual Cable)", "VB-Audio Virtual Cable", "CABLE Output (VB-Audio Virtual Cable)", "VB-Audio Virtual Cable"));
}

TEST(ClientMicRoutingTests, MatchesDifferentlyRenamedEndpointsSharingOneAdapter) {
  EXPECT_TRUE(client_mic::is_matching_capture_endpoint_with_adapter("Moonlight Stream Audio Output (VB-Audio Virtual Cable)", "Moonlight Stream Audio Output", "VB-Audio Virtual Cable", "Moonlight Stream Microphone (VB-Audio Virtual Cable)", "Moonlight Stream Microphone", "VB-Audio Virtual Cable"));
}

TEST(ClientMicRoutingTests, DoesNotMatchAnUnrelatedPhysicalMicrophone) {
  EXPECT_FALSE(client_mic::is_matching_capture_endpoint("Speakers (Realtek High Definition Audio)", "Realtek High Definition Audio", "Microphone (2- fifine Microphone)", "fifine Microphone"));
}

TEST(ClientMicRoutingTests, RecognizesAmdVoiceProcessedCaptureEndpoint) {
  EXPECT_TRUE(client_mic::is_amd_voice_capture_endpoint("Microphone (AMD Streaming Audio Device)", "AMD Streaming Audio Device", "AMD Streaming Audio Device"));
  EXPECT_FALSE(client_mic::is_amd_voice_capture_endpoint("Microphone (2- fifine Microphone)", "fifine Microphone", "fifine Microphone"));
}

TEST(ClientMicRoutingTests, RecognizesKrispProcessedCaptureEndpoint) {
  EXPECT_TRUE(client_mic::is_krisp_capture_endpoint("Microphone (Krisp Microphone)", "Krisp Microphone", "Krisp Microphone"));
  EXPECT_FALSE(client_mic::is_krisp_capture_endpoint("Microphone (2- fifine Microphone)", "fifine Microphone", "fifine Microphone"));
}

TEST(ClientMicRoutingTests, BlocksUnisolatedPolicyBeforeHostDefaultMutation) {
  EXPECT_TRUE(client_mic::allows_routing("sunshine"));
  EXPECT_FALSE(client_mic::allows_routing("isolated"));
  EXPECT_FALSE(client_mic::allows_routing("disabled"));
  EXPECT_FALSE(client_mic::allows_routing("unexpected"));
}

TEST(VideoSendPacingTests, UsesBitrateAwarePacingForWanSizedPackets) {
  auto [packets_in_1ms, send_batch_size] = stream::calculate_video_send_pacing(700, 6000, 20);
  EXPECT_EQ(packets_in_1ms, 2);
  EXPECT_EQ(send_batch_size, 2);
}

TEST(VideoSendPacingTests, CapsPacingToTheConfiguredEncoderBitrate) {
  EXPECT_EQ(stream::effective_video_send_bitrate_kbps(14988, 2500), 2500);
  EXPECT_EQ(stream::effective_video_send_bitrate_kbps(2500, 2500), 2500);
  EXPECT_EQ(stream::effective_video_send_bitrate_kbps(6000, 0), 6000);
}

TEST(VideoSendPacingTests, KeepsSteadySmallFramesWithinTheirPacingWindow) {
  auto [packets_in_1ms, send_batch_size] = stream::calculate_video_send_pacing(1500, 50000, 20);
  EXPECT_EQ(packets_in_1ms, 5);
  EXPECT_EQ(send_batch_size, 5);
  EXPECT_EQ(stream::calculate_video_send_frame_budget(5, packets_in_1ms), std::chrono::milliseconds(1));
}

TEST(VideoSendPacingTests, BoundsLargeFramesToOneFrameBurstWindowWithoutChangingTheSustainedRate) {
  EXPECT_EQ(stream::calculate_video_send_frame_burst_packets_per_ms(1, 1), 1u);
  EXPECT_EQ(stream::calculate_video_send_frame_burst_packets_per_ms(1, 128), 8u);
  EXPECT_EQ(stream::calculate_video_send_frame_burst_packets_per_ms(1, 185), 8u);
  EXPECT_EQ(stream::calculate_video_send_frame_burst_packets_per_ms(5, 5), 5u);
  EXPECT_EQ(stream::calculate_video_send_frame_burst_packets_per_ms(12, 185), 12u);
  EXPECT_EQ(stream::calculate_video_send_frame_budget(185, 8), std::chrono::milliseconds(24));
}

TEST(VideoSendPacingTests, UsesBoundedMultiMillisecondBatchesToAvoidTimerOversleep) {
  EXPECT_EQ(stream::calculate_video_send_frame_batch_size(0), 0u);
  EXPECT_EQ(stream::calculate_video_send_frame_batch_size(1), 4u);
  EXPECT_EQ(stream::calculate_video_send_frame_batch_size(2), 8u);
  EXPECT_EQ(stream::calculate_video_send_frame_batch_size(8), 32u);
  EXPECT_EQ(stream::calculate_video_send_frame_batch_size(12), 32u);
}

TEST(VideoSendPacingTests, CapsDualDisplayBatchesBeforeFecParityCanShareABurst) {
  EXPECT_EQ(stream::calculate_video_send_frame_batch_size(0, true), 0u);
  EXPECT_EQ(stream::calculate_video_send_frame_batch_size(2, true), 4u);
  EXPECT_EQ(stream::calculate_video_send_frame_batch_size(8, true), 4u);
  EXPECT_EQ(stream::calculate_video_send_frame_batch_size(8, false), 32u);
}

TEST(VideoSendPacingTests, CapsKeyframeBatchesToAvoidClientLossBursts) {
  EXPECT_EQ(stream::calculate_video_send_frame_batch_size_for_frame(0, false, true), 0u);
  EXPECT_EQ(stream::calculate_video_send_frame_batch_size_for_frame(2, false, true), 2u);
  EXPECT_EQ(stream::calculate_video_send_frame_batch_size_for_frame(2, true, true), 2u);
  EXPECT_EQ(stream::calculate_video_send_frame_batch_size_for_frame(2, false, false), 8u);
}

TEST(VideoSendPacingTests, LimitsPacingDebtToOneShortInterframeDelay) {
  using namespace std::chrono_literals;

  const auto now = std::chrono::steady_clock::time_point {100ms};
  EXPECT_EQ(
    stream::calculate_video_pacing_next_frame_start(now + 500ms, now, 4ms),
    now + 4ms
  );
  EXPECT_EQ(
    stream::calculate_video_pacing_next_frame_start(now + 2ms, now, 4ms),
    now + 2ms
  );
  EXPECT_EQ(
    stream::calculate_video_pacing_next_frame_start(now - 10ms, now, 4ms),
    now - 10ms
  );
}

TEST(VideoStreamHeartbeatTests, EmitsInitiallyAndThenRateLimitsHealthyFrames) {
  using namespace std::chrono_literals;

  const auto start = std::chrono::steady_clock::now();
  EXPECT_TRUE(stream::should_emit_video_stream_heartbeat(start, std::chrono::steady_clock::time_point::min(), 10s));
  EXPECT_FALSE(stream::should_emit_video_stream_heartbeat(start + 9s, start, 10s));
  EXPECT_TRUE(stream::should_emit_video_stream_heartbeat(start + 10s, start, 10s));
}

TEST(VideoSendPacingTests, CapsBurstyLargeFrameCatchUpCreditAndBurstSize) {
  using namespace std::chrono_literals;

  auto [packets_in_1ms, send_batch_size] = stream::calculate_video_send_pacing(1500, 50000, 20);
  const auto catch_up_credit = stream::calculate_video_send_catch_up_credit(75ms, packets_in_1ms, 20ms);
  const auto catch_up_burst_size = stream::calculate_video_send_catch_up_burst_size(catch_up_credit, send_batch_size);

  EXPECT_EQ(packets_in_1ms, 5);
  EXPECT_EQ(send_batch_size, 5);
  EXPECT_EQ(catch_up_credit, 100u);
  EXPECT_EQ(catch_up_burst_size, 5u);
  EXPECT_LE(catch_up_credit, packets_in_1ms * 20);
  EXPECT_LE(catch_up_burst_size, send_batch_size);
  EXPECT_LE(send_batch_size, packets_in_1ms);
}

TEST(VideoSendPacingTests, FallsBackToLegacyBehaviorWithoutBitrate) {
  auto [packets_in_1ms, send_batch_size] = stream::calculate_video_send_pacing(700, 0, 60);
  EXPECT_GT(packets_in_1ms, 64u);
  EXPECT_EQ(send_batch_size, 64u);
}

TEST(VideoSendPacingTests, StartsRecentOverdueFrameAtCurrentTime) {
  using namespace std::chrono_literals;

  const auto now = std::chrono::steady_clock::time_point {50ms};
  const auto scheduled_start = now - 12ms;

  EXPECT_EQ(
    stream::calculate_video_pacing_frame_start(scheduled_start, now),
    now
  );
}

TEST(VideoSendPacingTests, StartsOverdueFrameAtCurrentTime) {
  using namespace std::chrono_literals;

  const auto now = std::chrono::steady_clock::time_point {50ms};
  const auto scheduled_start = now - 80ms;

  EXPECT_EQ(
    stream::calculate_video_pacing_frame_start(scheduled_start, now),
    now
  );
}

TEST(VideoSendPacingTests, CapsFutureFrameScheduleToShortInterframeDelay) {
  using namespace std::chrono_literals;

  const auto now = std::chrono::steady_clock::time_point {50ms};
  const auto scheduled_start = now + 5ms;

  EXPECT_EQ(
    stream::calculate_video_pacing_frame_start(scheduled_start, now),
    now + 4ms
  );
}

TEST(VideoSendPacingTests, SuppressesExpectedFirstBatchIdleCreditLateness) {
  using namespace std::chrono_literals;

  EXPECT_FALSE(stream::should_log_video_pacing_late_warning(20ms, 0, 20ms, 10ms));
}

TEST(VideoSendPacingTests, LogsFirstBatchWhenLateBeyondIdleCreditWindow) {
  using namespace std::chrono_literals;

  EXPECT_TRUE(stream::should_log_video_pacing_late_warning(31ms, 0, 20ms, 10ms));
}

TEST(VideoSendPacingTests, KeepsInFrameLateWarningSensitive) {
  using namespace std::chrono_literals;

  EXPECT_TRUE(stream::should_log_video_pacing_late_warning(12ms, 3, 20ms, 10ms));
}

TEST(VideoSendPacingTests, EmitsFirstDiagnosticPacingWarningImmediately) {
  using namespace std::chrono_literals;

  const auto now = std::chrono::steady_clock::time_point {100ms};
  EXPECT_TRUE(stream::should_emit_video_pacing_late_warning(now, std::chrono::steady_clock::time_point::min(), 1s));
}

TEST(VideoSendPacingTests, RateLimitsRepeatedDiagnosticPacingWarnings) {
  using namespace std::chrono_literals;

  const auto last_warning = std::chrono::steady_clock::time_point {100ms};
  EXPECT_FALSE(stream::should_emit_video_pacing_late_warning(last_warning + 999ms, last_warning, 1s));
  EXPECT_TRUE(stream::should_emit_video_pacing_late_warning(last_warning + 1s, last_warning, 1s));
}

TEST(VideoSendPacingTests, CalculatesFrameBudgetFromPacketsPerMs) {
  using namespace std::chrono_literals;

  EXPECT_EQ(stream::calculate_video_send_frame_budget(250, 3), 84ms);
  EXPECT_EQ(stream::calculate_video_send_frame_budget(3, 3), 1ms);
}

TEST(VideoSendPacingTests, SuppressesExpectedLargeFrameDurationWithinBudgetSlack) {
  using namespace std::chrono_literals;

  EXPECT_FALSE(stream::should_log_video_send_frame_warning(63ms, 250, 3, 40ms, 20ms));
}

TEST(VideoSendPacingTests, LogsLargeFrameDurationWhenItOvershootsBudgetSlack) {
  using namespace std::chrono_literals;

  EXPECT_TRUE(stream::should_log_video_send_frame_warning(105ms, 250, 3, 40ms, 20ms));
}

TEST(VideoSendPacingTests, KeepsShortFrameWarningsConservative) {
  using namespace std::chrono_literals;

  EXPECT_FALSE(stream::should_log_video_send_frame_warning(25ms, 9, 3, 40ms, 20ms));
  EXPECT_TRUE(stream::should_log_video_send_frame_warning(45ms, 9, 3, 40ms, 20ms));
}
