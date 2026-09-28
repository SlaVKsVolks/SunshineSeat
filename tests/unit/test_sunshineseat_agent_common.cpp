/**
 * @file tests/unit/test_sunshineseat_agent_common.cpp
 * @brief Test live-evidence session truth normalization for sunshineseatctl.
 */
#include "../tests_common.h"

#include <tools/sunshineseat_agent_common.h>

namespace {
  using sunshineseat_agent::input_telemetry_t;
  using sunshineseat_agent::input_health_snapshot_t;
  using sunshineseat_agent::input_recovery_action_t;
  using sunshineseat_agent::classify_input_recovery;
  using sunshineseat_agent::is_recent_stream_success_age;
  using sunshineseat_agent::is_stream_success_log_marker;
  using sunshineseat_agent::recent_stream_heartbeat_implies_active_session;
  using sunshineseat_agent::reconnect_phase_t;
  using sunshineseat_agent::decide_reconnect_policy;
  using sunshineseat_agent::reconnect_phase_deadline_ms;
  using sunshineseat_agent::apollo_restart_health_timeout;
  using sunshineseat_agent::adaptive_bitrate_sample_t;
  using sunshineseat_agent::decide_adaptive_bitrate;
  using sunshineseat_agent::audio_health_snapshot_t;
  using sunshineseat_agent::decide_audio_recovery;
  using sunshineseat_agent::normalize_stream_session_state_from_live_evidence;
  using sunshineseat_agent::parse_input_telemetry_lines;
  using sunshineseat_agent::resolve_session_truth;
  using sunshineseat_agent::session_truth_evidence_t;
  using sunshineseat_agent::stream_session_state_t;
  using sunshineseat_agent::stream_health_snapshot_t;
  using sunshineseat_agent::derive_stream_health;
  using sunshineseat_agent::make_stream_health_snapshot;
  using sunshineseat_agent::should_recover_session;
  using sunshineseat_agent::process_owner_snapshot_t;
  using sunshineseat_agent::seat_ownership_policy_t;
  using sunshineseat_agent::evaluate_seat_process_ownership;
  using sunshineseat_agent::scheduled_task_user_from_xml;
  using sunshineseat_agent::scheduled_task_user_matches_policy;
  using sunshineseat_agent::image_name_from_path;
}

TEST(SunshineSeatAgentCommonTest, DefaultSingleUserOwnershipPreservesUpstreamBehavior) {
  seat_ownership_policy_t policy;

  const auto decision = evaluate_seat_process_ownership(policy, {});

  EXPECT_TRUE(decision.allowed);
  EXPECT_EQ(decision.state, "default_single_user");
}

TEST(SunshineSeatAgentCommonTest, RemoteUserOwnershipRequiresAnExplicitTargetUser) {
  seat_ownership_policy_t policy;
  policy.role = "remote-user";

  const auto decision = evaluate_seat_process_ownership(policy, {});

  EXPECT_FALSE(decision.allowed);
  EXPECT_EQ(decision.reason, "target_user_required_for_non_single_user_role");
}

TEST(SunshineSeatAgentCommonTest, RemoteUserOwnershipAcceptsOneMatchingSidAndSession) {
  seat_ownership_policy_t policy;
  policy.role = "remote-user";
  policy.target_user = "TestUser";
  policy.target_sid = "S-1-5-21-100-200-300-1000";
  policy.target_session_id = 7;

  process_owner_snapshot_t process;
  process.pid = 42;
  process.session_id = 7;
  process.user_sid = policy.target_sid;
  process.query_succeeded = true;

  const auto decision = evaluate_seat_process_ownership(policy, {process});

  EXPECT_TRUE(decision.allowed);
  EXPECT_EQ(decision.state, "validated");
  EXPECT_EQ(decision.observed_sessions.size(), 1u);
}

TEST(SunshineSeatAgentCommonTest, OwnershipRejectsSidMismatchAndMultipleSessions) {
  seat_ownership_policy_t policy;
  policy.role = "remote-user";
  policy.target_user = "TestUser";
  policy.target_sid = "S-1-5-21-100-200-300-1000";

  process_owner_snapshot_t wrong_user;
  wrong_user.pid = 42;
  wrong_user.session_id = 7;
  wrong_user.user_sid = "S-1-5-21-100-200-300-2000";
  wrong_user.query_succeeded = true;

  process_owner_snapshot_t second_session;
  second_session.pid = 43;
  second_session.session_id = 8;
  second_session.user_sid = policy.target_sid;
  second_session.query_succeeded = true;

  const auto decision = evaluate_seat_process_ownership(policy, {wrong_user, second_session});

  EXPECT_FALSE(decision.allowed);
  EXPECT_EQ(decision.observed_sessions.size(), 2u);
  EXPECT_EQ(decision.blockers.size(), 2u);
}

TEST(SunshineSeatAgentCommonTest, OwnershipRejectsUnexpectedListenerImage) {
  seat_ownership_policy_t policy;
  policy.role = "remote-user";
  policy.target_user = "TestUser";
  policy.target_sid = "S-1-5-21-100-200-300-1000";
  policy.allowed_process_images = {"sunshine.exe", "sunshine-sidecar.exe"};

  process_owner_snapshot_t process;
  process.pid = 44;
  process.session_id = 7;
  process.user_sid = policy.target_sid;
  process.image_name = "unrelated.exe";
  process.query_succeeded = true;

  const auto decision = evaluate_seat_process_ownership(policy, {process});

  EXPECT_FALSE(decision.allowed);
  EXPECT_EQ(decision.reason, "process_image_not_allowed:pid=44");
}

TEST(SunshineSeatAgentCommonTest, ProcessImageNameIsReducedToTheExecutableName) {
  EXPECT_EQ(image_name_from_path(R"(C:\Program Files\Sunshine\sunshine-sidecar.exe)"), "sunshine-sidecar.exe");
}

TEST(SunshineSeatAgentCommonTest, ScheduledTaskXmlOwnershipIsParsedAndMatched) {
  const std::string xml =
    "<Task><Principals><Principal><UserId>DESKTOP-TEST\\TestUser</UserId></Principal></Principals></Task>";
  EXPECT_EQ(scheduled_task_user_from_xml(xml), "DESKTOP-TEST\\TestUser");

  seat_ownership_policy_t policy;
  policy.role = "remote-user";
  policy.target_user = "TestUser";
  policy.target_sid = "S-1-5-21-100-200-300-1000";
  EXPECT_TRUE(scheduled_task_user_matches_policy(policy, scheduled_task_user_from_xml(xml)));
  EXPECT_FALSE(scheduled_task_user_matches_policy(policy, "DESKTOP-TEST\\OtherUser"));

  policy.target_user = "DESKTOP-TEST\\TestUser";
  EXPECT_TRUE(scheduled_task_user_matches_policy(policy, "TestUser"));
}

TEST(SunshineSeatAgentCommonTest, SessionTruthPrefersRecentMediaOverUnknownLogCounters) {
  session_truth_evidence_t evidence;
  evidence.recent_udp_or_media_evidence = true;

  const auto truth = resolve_session_truth(evidence);

  EXPECT_EQ(truth.state, "active");
  EXPECT_EQ(truth.confidence, "high");
  EXPECT_TRUE(truth.media_evidence.active);
  EXPECT_FALSE(truth.conflict);
}

TEST(SunshineSeatAgentCommonTest, RecentStreamSuccessNormalizesUnknownLogCountersFromHighConfidenceLiveEvidence) {
  stream_session_state_t state;
  state.active_session_count = -1;
  state.last_client_event = "unknown";
  state.virtual_display_active = false;
  state.media_udp_listener_count = 0;

  normalize_stream_session_state_from_live_evidence(state, true, true, true, false);

  EXPECT_EQ(state.active_session_count, 1);
  EXPECT_EQ(state.active_session_source, "session_truth");
  EXPECT_EQ(state.last_client_event, "CLIENT CONNECTED");
  EXPECT_EQ(state.last_client_event_source, "session_truth");
  EXPECT_TRUE(state.live_stream_evidence);
  EXPECT_EQ(state.session_truth.state, "active");
  EXPECT_EQ(state.session_truth.confidence, "high");
  EXPECT_EQ(state.status_consistency, "consistent");
}

TEST(SunshineSeatAgentCommonTest, ConfidentIdleTruthNormalizesUnknownLogCounters) {
  stream_session_state_t state;
  state.active_session_count = -1;
  state.last_client_event = "unknown";
  state.media_udp_listener_count = 0;

  normalize_stream_session_state_from_live_evidence(state, false, false, false, false);

  EXPECT_EQ(state.session_truth.state, "idle");
  EXPECT_EQ(state.active_session_count, 0);
  EXPECT_EQ(state.active_session_source, "session_truth");
  EXPECT_EQ(state.last_client_event, "idle");
  EXPECT_EQ(state.last_client_event_source, "session_truth");
}

TEST(SunshineSeatAgentCommonTest, MissingUdpPortsWarnOnlyDuringAnActiveSession) {
  EXPECT_FALSE(sunshineseat_agent::missing_udp_warning_required("idle", 3));
  EXPECT_FALSE(sunshineseat_agent::missing_udp_warning_required("active", 0));
  EXPECT_TRUE(sunshineseat_agent::missing_udp_warning_required("active", 1));
}

TEST(SunshineSeatAgentCommonTest, SessionTruthKeepsListenerOnlyUdpNonActive) {
  session_truth_evidence_t evidence;
  evidence.listener_only_udp = true;

  const auto truth = resolve_session_truth(evidence);

  EXPECT_EQ(truth.state, "unknown");
  EXPECT_EQ(truth.confidence, "low");
  EXPECT_TRUE(truth.listener_evidence.present);
  EXPECT_FALSE(truth.listener_evidence.active);
  EXPECT_FALSE(truth.has_high_confidence_source);
}

TEST(SunshineSeatAgentCommonTest, SessionTruthFlagsCtlAndMediaDisagreementWithoutRecovery) {
  session_truth_evidence_t evidence;
  evidence.ctl_reports_active = true;

  const auto truth = resolve_session_truth(evidence);

  EXPECT_EQ(truth.state, "active");
  EXPECT_TRUE(truth.conflict);
  EXPECT_EQ(truth.conflict_reason, "ctl_active_without_recent_media_or_stream_success");
  EXPECT_FALSE(should_recover_session(true, truth));
}

TEST(SunshineSeatAgentCommonTest, ClientConnectedWithoutMediaOrStreamSuccessIsCtlConflict) {
  stream_session_state_t state;
  state.active_session_count = 1;
  state.last_client_event = "CLIENT CONNECTED";

  normalize_stream_session_state_from_live_evidence(state, true, false, false);

  EXPECT_EQ(state.session_truth.state, "active");
  EXPECT_EQ(state.session_truth.confidence, "medium");
  EXPECT_TRUE(state.session_truth.conflict);
  EXPECT_EQ(state.session_truth.conflict_reason, "ctl_active_without_recent_media_or_stream_success");
}

TEST(SunshineSeatAgentCommonTest, PendingLaunchAfterDisconnectCannotReuseOldClientEvidence) {
  stream_session_state_t state;
  state.active_session_count = 1;
  state.last_client_event = "CLIENT CONNECTED";

  normalize_stream_session_state_from_live_evidence(state, false, true, true);

  EXPECT_EQ(state.session_truth.state, "idle");
  EXPECT_FALSE(state.live_stream_evidence);
  EXPECT_EQ(state.status_consistency, "pending_launch_without_current_client_connect");
}

TEST(SunshineSeatAgentCommonTest, SessionTruthUsesSharedStreamSuccessEvidenceWindow) {
  EXPECT_TRUE(is_recent_stream_success_age(15));
  EXPECT_FALSE(is_recent_stream_success_age(16));

  session_truth_evidence_t at_boundary;
  at_boundary.ctl_reports_active = true;
  at_boundary.recent_stream_success = is_recent_stream_success_age(15);
  const auto boundary_truth = resolve_session_truth(at_boundary);
  EXPECT_EQ(boundary_truth.state, "active");
  EXPECT_EQ(boundary_truth.confidence, "high");
  EXPECT_FALSE(boundary_truth.conflict);

  session_truth_evidence_t after_window;
  after_window.ctl_reports_active = true;
  after_window.recent_stream_success = is_recent_stream_success_age(16);
  const auto expired_truth = resolve_session_truth(after_window);
  EXPECT_EQ(expired_truth.state, "active");
  EXPECT_EQ(expired_truth.confidence, "medium");
  EXPECT_TRUE(expired_truth.conflict);
}

TEST(SunshineSeatAgentCommonTest, StreamHeartbeatIsARecentSuccessMarker) {
  EXPECT_TRUE(is_stream_success_log_marker(
    "[2026-07-20 09:48:18.357]: Info: Video stream_heartbeat [frame=12125, packets=3]"));
  EXPECT_TRUE(is_stream_success_log_marker(
    "[2026-07-20 09:48:18.357]: Info: Capture format [DXGI_FORMAT_B8G8R8A8_UNORM]"));
  EXPECT_FALSE(is_stream_success_log_marker(
    "[2026-07-20 09:48:18.357]: Warning: Video pacing_late_diagnostic [frame=12125]"));
}

TEST(SunshineSeatAgentCommonTest, RecentHeartbeatCanProveCurrentStreamWhenTailOmitsBoundary) {
  EXPECT_TRUE(recent_stream_heartbeat_implies_active_session(true, false));
  EXPECT_FALSE(recent_stream_heartbeat_implies_active_session(false, false));
  EXPECT_FALSE(recent_stream_heartbeat_implies_active_session(true, true));
}

TEST(SunshineSeatAgentCommonTest, InputTelemetryClassifiesDispatchObservedWhenRecentDispatchExists) {
  const auto now_epoch_ms = 1'000'000LL;
  const auto state = parse_input_telemetry_lines({
    "session_generation=4",
    "snapshot_epoch_ms=995000",
    "accepted_total=12",
    "dispatched_total=9",
    "permission_blocked_total=1",
    "last_accepted_epoch_ms=999500",
    "last_dispatched_epoch_ms=999400",
    "last_permission_blocked_epoch_ms=998000",
    "last_accepted_family=mouse",
    "last_dispatched_family=mouse",
    "last_permission_blocked_family=keyboard",
  }, now_epoch_ms);

  EXPECT_TRUE(state.available);
  EXPECT_EQ(state.session_generation, 4u);
  EXPECT_EQ(state.accepted_total, 12u);
  EXPECT_EQ(state.dispatched_total, 9u);
  EXPECT_EQ(state.permission_blocked_total, 1u);
  EXPECT_EQ(state.snapshot_age_seconds, 5);
  EXPECT_EQ(state.last_accepted_age_seconds, 0);
  EXPECT_EQ(state.last_dispatched_age_seconds, 0);
  EXPECT_EQ(state.status, "dispatch_observed");
}

TEST(SunshineSeatAgentCommonTest, InputTelemetryClassifiesAcceptedNotDispatchedWithoutSuccessfulDispatch) {
  const auto now_epoch_ms = 1'000'000LL;
  const auto state = parse_input_telemetry_lines({
    "session_generation=5",
    "snapshot_epoch_ms=999000",
    "accepted_total=3",
    "dispatched_total=0",
    "permission_blocked_total=0",
    "last_accepted_epoch_ms=999000",
    "last_accepted_family=keyboard",
  }, now_epoch_ms);

  EXPECT_TRUE(state.available);
  EXPECT_EQ(state.status, "accepted_not_dispatched");
  EXPECT_EQ(state.last_accepted_family, "keyboard");
}

TEST(SunshineSeatAgentCommonTest, InputTelemetryClassifiesStaleSnapshotWhenSnapshotIsTooOld) {
  const auto now_epoch_ms = 1'000'000LL;
  const auto state = parse_input_telemetry_lines({
    "session_generation=6",
    "snapshot_epoch_ms=1000",
    "accepted_total=8",
    "dispatched_total=8",
    "permission_blocked_total=0",
    "last_accepted_epoch_ms=900",
    "last_dispatched_epoch_ms=800",
  }, now_epoch_ms);

  EXPECT_TRUE(state.available);
  EXPECT_EQ(state.status, "stale_snapshot");
  EXPECT_GT(state.snapshot_age_seconds, 300);
}

TEST(SunshineSeatAgentCommonTest, InputRecoveryRequiresProbeFailureAndProtectsPermissionBlockedInput) {
  input_health_snapshot_t stale;
  stale.active_stream = true;
  stale.accepted_age_seconds = 8;
  stale.dispatched_age_seconds = 8;
  EXPECT_EQ(classify_input_recovery(stale).action, input_recovery_action_t::observe);

  stale.probe_failed = true;
  EXPECT_EQ(classify_input_recovery(stale).action, input_recovery_action_t::reinitialize);
  EXPECT_TRUE(classify_input_recovery(stale).safe_to_reinitialize);

  input_health_snapshot_t accepted_but_not_dispatched;
  accepted_but_not_dispatched.active_stream = true;
  accepted_but_not_dispatched.probe_failed = true;
  accepted_but_not_dispatched.accepted_age_seconds = 1;
  accepted_but_not_dispatched.dispatched_age_seconds = 8;
  EXPECT_EQ(classify_input_recovery(accepted_but_not_dispatched).action, input_recovery_action_t::reinitialize);

  stale.permission_blocked = true;
  EXPECT_EQ(classify_input_recovery(stale).action, input_recovery_action_t::blocked_by_permission);
  EXPECT_FALSE(classify_input_recovery(stale).safe_to_reinitialize);
}

TEST(SunshineSeatAgentCommonTest, ReconnectPolicyUsesStageDeadlinesBackoffAndCircuitBreaker) {
  EXPECT_EQ(reconnect_phase_deadline_ms(reconnect_phase_t::control_authenticated), 10000);
  EXPECT_EQ(reconnect_phase_deadline_ms(reconnect_phase_t::session_initializing), 20000);
  EXPECT_EQ(reconnect_phase_deadline_ms(reconnect_phase_t::media_establishing), 30000);
  EXPECT_EQ(reconnect_phase_deadline_ms(reconnect_phase_t::cleanup_wait), 60000);
  EXPECT_EQ(decide_reconnect_policy(0, 1, false, false).action, "restart");
  EXPECT_EQ(decide_reconnect_policy(0, 1, false, false).next_delay_ms, 10000);
  EXPECT_EQ(decide_reconnect_policy(5, 1, false, false).action, "circuit_open");
  EXPECT_EQ(decide_reconnect_policy(0, 1, true, false).action, "suppress");
}

TEST(SunshineSeatAgentCommonTest, ApolloRestartDoesNotFailBeforeLauncherStartupWindow) {
  const auto launcher_preflight = std::chrono::seconds {31};
  const auto launcher_startup_window = std::chrono::seconds {180};
  const auto observed_healthy = std::chrono::seconds {35};

  EXPECT_LT(observed_healthy, apollo_restart_health_timeout());
  EXPECT_GE(apollo_restart_health_timeout(), launcher_preflight + launcher_startup_window);
}

TEST(SunshineSeatAgentCommonTest, AdaptiveBitrateAndAudioRecoveryRemainConservative) {
  adaptive_bitrate_sample_t bad;
  bad.current_bitrate = 7000;
  bad.loss_percent = 5.0;
  bad.consecutive_bad_samples = 2;
  EXPECT_EQ(decide_adaptive_bitrate(bad).action, "decrease");
  EXPECT_LT(decide_adaptive_bitrate(bad).target_bitrate, 7000);

  audio_health_snapshot_t audio;
  audio.clock_offset_ms = 120.0;
  EXPECT_EQ(decide_audio_recovery(audio, true).action, "reopen_endpoint");
  audio.endpoint_id_hash = "endpoint";
  EXPECT_EQ(decide_audio_recovery(audio, true).action, "resync_clock");
}

TEST(SunshineSeatAgentCommonTest, AdaptiveBitrateRequiresTwoFivePercentLossSamplesRegardlessOfFps) {
  adaptive_bitrate_sample_t sample;
  sample.current_bitrate = 20000;
  sample.loss_percent = 5.0;
  sample.render_fps = 60.0;
  sample.consecutive_bad_samples = 1;

  auto decision = decide_adaptive_bitrate(sample, 3000, 20000);
  EXPECT_EQ(decision.action, "hold");
  EXPECT_EQ(decision.target_bitrate, 20000);

  sample.consecutive_bad_samples = 2;
  sample.render_fps = 20.0;
  decision = decide_adaptive_bitrate(sample, 3000, 20000);
  EXPECT_EQ(decision.action, "decrease");
  EXPECT_EQ(decision.target_bitrate, 18000);
}

TEST(SunshineSeatAgentCommonTest, AdaptiveBitrateRequires180CleanSecondsAndUsesTwoPercentSteps) {
  adaptive_bitrate_sample_t sample;
  sample.current_bitrate = 10000;
  sample.clean_duration_seconds = 179;

  auto decision = decide_adaptive_bitrate(sample, 3000, 20000);
  EXPECT_EQ(decision.action, "hold");
  EXPECT_EQ(decision.target_bitrate, 10000);

  sample.clean_duration_seconds = 180;
  decision = decide_adaptive_bitrate(sample, 3000, 20000);
  EXPECT_EQ(decision.action, "increase");
  EXPECT_EQ(decision.target_bitrate, 10200);
}

TEST(SunshineSeatAgentCommonTest, AdaptiveBitratePreservesQualityCeiling) {
  adaptive_bitrate_sample_t sample;
  sample.current_bitrate = 19900;
  sample.clean_duration_seconds = 180;

  const auto decision = decide_adaptive_bitrate(sample, 3000, 20000);
  EXPECT_EQ(decision.action, "increase");
  EXPECT_EQ(decision.target_bitrate, 20000);
}

TEST(SunshineSeatAgentCommonTest, StreamHealthContractSeparatesHealthyDiagnosticAndRecoverableFailures) {
  stream_health_snapshot_t healthy;
  healthy.active_session = true;
  healthy.media_heartbeat_age_ms = 5'000;
  healthy.fps = 60.0;
  EXPECT_EQ(derive_stream_health(healthy).state, "healthy");
  EXPECT_FALSE(derive_stream_health(healthy).recovery_eligible);

  auto encoder_stall = healthy;
  encoder_stall.media_heartbeat_age_ms = 30'000;
  encoder_stall.encode_stage_ms = 9'000;
  EXPECT_EQ(derive_stream_health(encoder_stall).state, "severe");
  EXPECT_EQ(derive_stream_health(encoder_stall).reason, "media_heartbeat_stale");
  EXPECT_TRUE(derive_stream_health(encoder_stall).recovery_eligible);

  auto pacing_only = healthy;
  pacing_only.pacing_sleep_ms = 350;
  EXPECT_EQ(derive_stream_health(pacing_only).state, "diagnostic");
  EXPECT_EQ(derive_stream_health(pacing_only).reason, "pacing_diagnostic_only");
  EXPECT_FALSE(derive_stream_health(pacing_only).recovery_eligible);

  stream_health_snapshot_t stale_reconnect;
  stale_reconnect.stale_reconnect_loop = true;
  EXPECT_EQ(derive_stream_health(stale_reconnect).state, "severe");
  EXPECT_EQ(derive_stream_health(stale_reconnect).reason, "stale_reconnect_loop");
  EXPECT_TRUE(derive_stream_health(stale_reconnect).recovery_eligible);

  auto input_failure = healthy;
  input_failure.input_healthy = false;
  EXPECT_EQ(derive_stream_health(input_failure).state, "suspected");
  EXPECT_EQ(derive_stream_health(input_failure).reason, "input_unhealthy");

  auto display_failure = healthy;
  display_failure.virtual_display_healthy = false;
  EXPECT_EQ(derive_stream_health(display_failure).state, "severe");
  EXPECT_EQ(derive_stream_health(display_failure).reason, "virtual_display_unhealthy");
}

TEST(SunshineSeatAgentCommonTest, StreamHealthSnapshotUsesMediaTruthInsteadOfTransientRtsp) {
  stream_session_state_t session;
  session.session_truth.state = "active";
  session.live_stream_evidence = true;
  session.virtual_display_active = true;
  session.virtual_display_name = R"(\\.\DISPLAY6)";
  session.media_heartbeat_age_seconds = 5;
  session.remote_tcp_live = false;
  session.rtsp_live = false;

  input_telemetry_t input;
  input.available = true;
  input.status = "dispatch_observed";

  const auto snapshot = make_stream_health_snapshot(session, input);
  EXPECT_TRUE(snapshot.active_session);
  EXPECT_EQ(snapshot.media_heartbeat_age_ms, 5'000);
  EXPECT_TRUE(snapshot.rtsp_healthy);
  EXPECT_TRUE(snapshot.virtual_display_healthy);
  EXPECT_TRUE(snapshot.input_healthy);
  EXPECT_EQ(derive_stream_health(snapshot).state, "healthy");
}

TEST(SunshineSeatAgentCommonTest, DesktopProviderStreamDoesNotRequireVirtualDisplay) {
  stream_session_state_t session;
  session.display_provider = "desktop";
  session.session_truth.state = "active";
  session.live_stream_evidence = true;
  session.virtual_display_active = false;
  session.virtual_display_name = "none";
  session.media_heartbeat_age_seconds = 5;

  input_telemetry_t input;
  input.available = true;
  input.status = "dispatch_observed";

  const auto snapshot = make_stream_health_snapshot(session, input);
  EXPECT_TRUE(snapshot.virtual_display_healthy);
  EXPECT_EQ(derive_stream_health(snapshot).state, "healthy");
}

TEST(SunshineSeatAgentCommonTest, IdleInputSnapshotDoesNotMarkActiveStreamUnhealthy) {
  stream_session_state_t session;
  session.session_truth.state = "active";
  session.live_stream_evidence = true;
  session.virtual_display_active = true;
  session.media_heartbeat_age_seconds = 5;

  input_telemetry_t input;
  input.available = true;
  input.status = "stale_snapshot";

  const auto snapshot = make_stream_health_snapshot(session, input);
  EXPECT_TRUE(snapshot.input_healthy);
  EXPECT_EQ(derive_stream_health(snapshot).state, "healthy");
}
