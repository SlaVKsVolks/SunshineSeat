/**
 * @file tests/unit/test_nvhttp_launch.cpp
 * @brief Tests for the authenticated dual-display launch contract.
 */

#include "../tests_common.h"

#include <src/dual_idr_scheduler.h>
#include <src/dual_display_launch_contract.h>
#include <src/nvhttp.h>

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace nvhttp {
  bool should_reset_running_app_state_for_launch(int current_appid, int active_session_count, bool is_input_only);
}

TEST(NvHttpLaunchStateTest, ResetsRunningAppStateWhenNoActiveSessionsRemain) {
  EXPECT_TRUE(nvhttp::should_reset_running_app_state_for_launch(1, 0, false));
}

TEST(NvHttpLaunchStateTest, DoesNotResetRunningAppStateDuringActiveSession) {
  EXPECT_FALSE(nvhttp::should_reset_running_app_state_for_launch(1, 1, false));
}

TEST(NvHttpLaunchStateTest, DoesNotResetRunningAppStateForInputOnlyLaunch) {
  EXPECT_FALSE(nvhttp::should_reset_running_app_state_for_launch(1, 0, true));
}

TEST(NvHttpLaunchStateTest, DoesNotResetWhenNoRunningAppExists) {
  EXPECT_FALSE(nvhttp::should_reset_running_app_state_for_launch(0, 0, false));
}

namespace {
  dual_display_launch::request_t make_request() {
    return dual_display_launch::request_t {
      .schema_version = dual_display_launch::current_schema_version,
      .session_generation = 42,
      .display_pair_id = "pair-opaque",
      .topology_fingerprint = "topology-opaque",
      .input_mapping_version = dual_display_launch::current_input_mapping_version,
      .panes = {
        dual_display_launch::request_pane_t {"left", "client-left", 1920, 1080, 60, "PixelPerfect", "Sdr", "Bgra8Unorm", "Landscape"},
        dual_display_launch::request_pane_t {"right", "client-right", 1920, 1080, 60, "PixelPerfect", "Sdr", "Bgra8Unorm", "Landscape"},
      }
    };
  }

  std::array<dual_display_launch::observed_pane_t, 2> make_observed() {
    return {
      dual_display_launch::observed_pane_t {"left", "host-left"},
      dual_display_launch::observed_pane_t {"right", "host-right"},
    };
  }

  class test_pair_owner_t final: public dual_display_launch::prepared_pair_owner_t {
  public:
    explicit test_pair_owner_t(int &release_count):
        release_count_ {release_count} {}

    ~test_pair_owner_t() override {
      ++release_count_;
    }

  private:
    int &release_count_;
  };

  std::shared_ptr<dual_display_launch::prepared_pair_t> make_prepared_pair(int &release_count) {
    std::string failure;
    const auto manifest = dual_display_launch::make_manifest(
      make_request(),
      make_observed(),
      "2030-01-01T00:00:00Z",
      failure
    );
    EXPECT_TRUE(manifest.has_value());
    EXPECT_TRUE(failure.empty());

    auto pair = std::make_shared<dual_display_launch::prepared_pair_t>();
    pair->manifest = *manifest;
    pair->provider_display_names = {"\\\\.\\DISPLAY-PRIVATE-A", "\\\\.\\DISPLAY-PRIVATE-B"};
    pair->owner_identity = "paired-client-server-id";
    pair->owner = std::make_shared<test_pair_owner_t>(release_count);
    return pair;
  }
}

TEST(NvHttpLaunchContractTest, OptionalRequestMayBeAbsentWithoutChangingSingleDisplayPath) {
  std::string failure;

  const auto parsed = dual_display_launch::parse_optional_request(false, {}, failure);

  EXPECT_FALSE(parsed.has_value());
  EXPECT_TRUE(failure.empty());
}

TEST(NvHttpLaunchContractTest, DualLaunchUsesOnlyTheCertificateVerifiedClientIdentity) {
  EXPECT_EQ(
    nvhttp::select_launch_client_identity("request-controlled-uuid", "paired-client-server-id", true),
    "paired-client-server-id"
  );
  EXPECT_TRUE(nvhttp::select_launch_client_identity("request-controlled-uuid", {}, true).empty());

  // Ordinary single-display launches retain their existing requested-UUID
  // behavior; the dual-only binding must not widen that behavior change.
  EXPECT_EQ(
    nvhttp::select_launch_client_identity("request-controlled-uuid", "paired-client-server-id", false),
    "request-controlled-uuid"
  );
}

TEST(NvHttpLaunchContractTest, PreparedPairSkipsLegacyPerDeviceDisplayConfiguration) {
  int owner_release_count = 0;
  const auto pair = make_prepared_pair(owner_release_count);

  EXPECT_TRUE(nvhttp::should_apply_legacy_display_configuration(dual_display_launch::prepared_pair_ptr {}));
  EXPECT_FALSE(nvhttp::should_apply_legacy_display_configuration(pair));
}

TEST(NvHttpLaunchContractTest, PreparedDualPairSkipsBlockingEncoderReprobe) {
  EXPECT_TRUE(nvhttp::should_probe_encoders_after_display_preparation(false));
  EXPECT_FALSE(nvhttp::should_probe_encoders_after_display_preparation(true));
}

TEST(NvHttpLaunchContractTest, ValidRequestProducesVersionedHostObservedCompositeManifest) {
  const auto request = make_request();
  std::string failure;

  const auto manifest = dual_display_launch::make_manifest(
    request,
    make_observed(),
    "2030-01-01T00:00:00Z",
    failure
  );

  ASSERT_TRUE(manifest.has_value());
  EXPECT_TRUE(failure.empty());
  EXPECT_EQ(manifest->schema_version, 1);
  EXPECT_EQ(manifest->session_generation, request.session_generation);
  EXPECT_EQ(manifest->display_pair_id, request.display_pair_id);
  EXPECT_EQ(manifest->topology_fingerprint, request.topology_fingerprint);
  EXPECT_EQ(manifest->composite_width, 3840U);
  EXPECT_EQ(manifest->composite_height, 1080U);
  EXPECT_EQ(manifest->panes[0].host_display_identity, "host-left");
  EXPECT_EQ(manifest->panes[1].host_display_identity, "host-right");

  const auto json = dual_display_launch::serialize_manifest(*manifest);
  EXPECT_NE(json.find("\"hostDisplayIdentity\":\"host-left\""), std::string::npos);
  EXPECT_NE(json.find("\"compositeCanvasWidth\":3840"), std::string::npos);
}

TEST(NvHttpLaunchContractTest, TwoVideoTrackManifestCarriesDistinctRtpTracks) {
  auto request = make_request();
  request.video_transport = dual_display_launch::two_video_tracks_transport;
  request.video_tracks[0] = {0, "left", dual_display_launch::video_track_ssrc(0), 1920, 1080, 60, 8000};
  request.video_tracks[1] = {1, "right", dual_display_launch::video_track_ssrc(1), 1920, 1080, 60, 8000};
  std::string failure;

  const auto manifest = dual_display_launch::make_manifest(
    request,
    make_observed(),
    "2030-01-01T00:00:00Z",
    failure
  );

  ASSERT_TRUE(manifest.has_value());
  EXPECT_TRUE(failure.empty());
  EXPECT_EQ(manifest->video_transport, dual_display_launch::two_video_tracks_transport);
  EXPECT_EQ(manifest->video_tracks.size(), 2U);
  EXPECT_EQ(manifest->video_tracks[0].width, 1920U);
  EXPECT_EQ(manifest->video_tracks[0].height, 1080U);
  EXPECT_EQ(manifest->video_tracks[0].bitrate_kbps, 8000U);
  EXPECT_EQ(manifest->video_tracks[1].bitrate_kbps, 8000U);
  EXPECT_NE(manifest->video_tracks[0].rtp_ssrc, manifest->video_tracks[1].rtp_ssrc);

  const auto json = dual_display_launch::serialize_manifest(*manifest);
  EXPECT_NE(json.find("\"videoTransport\":\"TwoVideoTracks\""), std::string::npos);
  EXPECT_NE(json.find("\"videoTracks\""), std::string::npos);

  auto pair = std::make_shared<dual_display_launch::prepared_pair_t>();
  int owner_release_count = 0;
  pair->manifest = *manifest;
  pair->provider_display_names = {"\\\\.\\DISPLAY-PRIVATE-A", "\\\\.\\DISPLAY-PRIVATE-B"};
  pair->owner_identity = "paired-client-server-id";
  pair->owner = std::make_shared<test_pair_owner_t>(owner_release_count);
  EXPECT_TRUE(dual_display_launch::validate_dual_track_announcement(*pair, 1920, 1080, 60, 2, failure));
  EXPECT_FALSE(dual_display_launch::validate_dual_track_announcement(*pair, 3840, 1080, 60, 2, failure));
  EXPECT_EQ(failure, "dual_display_sdp_dimensions_mismatch");
}

TEST(NvHttpLaunchContractTest, DualTrackIdrRequestRemainsInFlightUntilAKeyPacketIsEmitted) {
  using namespace std::chrono_literals;

  const auto start = dual_idr::scheduler_t::time_point {};
  dual_idr::scheduler_t scheduler {40ms, 16ms};
  const auto generation = scheduler.request_recovery(start);
  ASSERT_NE(generation, 0U);

  const auto first_claim = scheduler.claim_due_idr(0, start);
  ASSERT_TRUE(first_claim.has_value());
  scheduler.complete_idr(*first_claim, false, start + 1ms);

  // AMF may accept a keyed AVFrame but return EAGAIN without an AVPacket. The
  // next encoder iteration must stay keyed until a key packet is observed.
  const auto retry_claim = scheduler.claim_due_idr(0, start + 1ms);
  ASSERT_TRUE(retry_claim.has_value());
  EXPECT_EQ(retry_claim->generation, generation);
  EXPECT_FALSE(scheduler.claim_due_idr(1, start + 1ms).has_value());

  scheduler.complete_idr(*retry_claim, true, start + 2ms);
  EXPECT_FALSE(scheduler.claim_due_idr(1, start + 41ms).has_value());
  EXPECT_TRUE(scheduler.claim_due_idr(1, start + 42ms).has_value());
}

TEST(NvHttpLaunchContractTest, DualTrackIdrDuplicatesAreCoalescedAfterSuccessfulPair) {
  using namespace std::chrono_literals;

  const auto start = dual_idr::scheduler_t::time_point {};
  dual_idr::scheduler_t scheduler {40ms, 16ms};
  const auto first_generation = scheduler.request_recovery(start);
  ASSERT_NE(first_generation, 0U);

  const auto leader = scheduler.claim_due_idr(0, start);
  ASSERT_TRUE(leader.has_value());
  scheduler.complete_idr(*leader, true, start);

  const auto follower = scheduler.claim_due_idr(1, start + 40ms);
  ASSERT_TRUE(follower.has_value());
  scheduler.complete_idr(*follower, true, start + 40ms);

  const auto quiet_until = start + 40ms + dual_idr::recovery_quiet_period;
  for (const auto duplicate_at : {start + 100ms, start + 2s, quiet_until - 1ms}) {
    EXPECT_EQ(scheduler.request_recovery(duplicate_at), first_generation);
    EXPECT_FALSE(scheduler.has_pending_recovery());
    EXPECT_FALSE(scheduler.claim_due_idr(0, duplicate_at).has_value());
    EXPECT_FALSE(scheduler.claim_due_idr(1, duplicate_at).has_value());
  }

  const auto next_generation = scheduler.request_recovery(quiet_until);
  EXPECT_NE(next_generation, first_generation);
  EXPECT_FALSE(scheduler.claim_due_idr(0, quiet_until).has_value());
  EXPECT_TRUE(scheduler.claim_due_idr(1, quiet_until).has_value());
}

TEST(NvHttpLaunchContractTest, MalformedOrUnknownRequestIsRejected) {
  std::string failure;

  EXPECT_FALSE(dual_display_launch::parse_optional_request(true, "not-json", failure).has_value());
  EXPECT_EQ(failure, "malformed_dual_display_request");

  failure.clear();
  EXPECT_FALSE(dual_display_launch::parse_optional_request(true, R"({"schemaVersion":99})", failure).has_value());
  EXPECT_EQ(failure, "unsupported_schema_version");
}

TEST(NvHttpLaunchContractTest, DuplicateIdentitiesAndMismatchedObservedPairAreRejected) {
  auto request = make_request();
  request.panes[1].client_display_id = request.panes[0].client_display_id;
  EXPECT_EQ(dual_display_launch::validate_request(request), "duplicate_client_display_identity");

  request = make_request();
  auto observed = make_observed();
  observed[1].logical_display_id = "unexpected";
  std::string failure;
  EXPECT_FALSE(dual_display_launch::make_manifest(request, observed, "2030-01-01T00:00:00Z", failure).has_value());
  EXPECT_EQ(failure, "observed_logical_identity_mismatch");

  observed = make_observed();
  observed[1].host_display_identity = observed[0].host_display_identity;
  failure.clear();
  EXPECT_FALSE(dual_display_launch::make_manifest(request, observed, "2030-01-01T00:00:00Z", failure).has_value());
  EXPECT_EQ(failure, "duplicate_host_display_identity");
}

TEST(NvHttpLaunchContractTest, CompositeDimensionOverflowIsRejected) {
  auto request = make_request();
  request.panes[0].width = 7680;
  request.panes[1].width = 1;

  EXPECT_EQ(dual_display_launch::validate_request(request), "composite_dimension_overflow");
}

TEST(NvHttpLaunchContractTest, EmptyObservationFailsClosedWithoutSynthesizingHostIdentity) {
  const auto request = make_request();
  std::string failure;
  const auto empty = std::array<dual_display_launch::observed_pane_t, 2> {
    dual_display_launch::observed_pane_t {},
    dual_display_launch::observed_pane_t {},
  };

  EXPECT_FALSE(dual_display_launch::make_manifest(request, empty, "2030-01-01T00:00:00Z", failure).has_value());
  EXPECT_EQ(failure, "missing_host_display_identity");
}

TEST(NvHttpLaunchContractTest, SingleDisplayPreparationDoesNotInvokeThePairProvider) {
  int provider_calls = 0;
  std::string failure;
  const nvhttp::dual_display_allocation_context_t context {
    .no_active_sessions = true,
    .no_pending_launch_sessions = true,
    .input_only = false,
    .permission_granted = true,
    .encoder_supports_dual_display_capture = true,
    .authenticated_client_identity = "paired-client-server-id",
  };

  const auto pair = nvhttp::prepare_dual_display_pair_after_gates(
    std::nullopt,
    context,
    [&](const dual_display_launch::request_t &, std::string_view, std::string &) {
      ++provider_calls;
      return std::shared_ptr<dual_display_launch::prepared_pair_t> {};
    },
    failure
  );

  EXPECT_FALSE(pair);
  EXPECT_EQ(provider_calls, 0);
  EXPECT_TRUE(failure.empty());
}

TEST(NvHttpLaunchContractTest, PairProviderRunsOnlyAfterSessionInputAndPermissionGatesPass) {
  const auto request = std::optional<dual_display_launch::request_t> {make_request()};
  int provider_calls = 0;
  int owner_release_count = 0;
  std::string failure;
  const auto provider = [&](const dual_display_launch::request_t &, const std::string_view owner_identity, std::string &) {
    ++provider_calls;
    EXPECT_EQ(owner_identity, "paired-client-server-id");
    return make_prepared_pair(owner_release_count);
  };

  auto context = nvhttp::dual_display_allocation_context_t {
    .no_active_sessions = false,
    .no_pending_launch_sessions = true,
    .input_only = false,
    .permission_granted = true,
    .encoder_supports_dual_display_capture = true,
    .authenticated_client_identity = nvhttp::select_launch_client_identity(
      "request-controlled-uuid",
      "paired-client-server-id",
      true
    ),
  };
  EXPECT_FALSE(nvhttp::prepare_dual_display_pair_after_gates(request, context, provider, failure));
  EXPECT_EQ(failure, "dual_display_active_session_conflict");
  EXPECT_EQ(provider_calls, 0);

  context.no_active_sessions = true;
  context.input_only = true;
  failure.clear();
  EXPECT_FALSE(nvhttp::prepare_dual_display_pair_after_gates(request, context, provider, failure));
  EXPECT_EQ(failure, "dual_display_input_only_unsupported");
  EXPECT_EQ(provider_calls, 0);

  context.input_only = false;
  context.no_pending_launch_sessions = false;
  failure.clear();
  EXPECT_FALSE(nvhttp::prepare_dual_display_pair_after_gates(request, context, provider, failure));
  EXPECT_EQ(failure, "dual_display_pending_session_conflict");
  EXPECT_EQ(provider_calls, 0);

  context.no_pending_launch_sessions = true;
  context.permission_granted = false;
  failure.clear();
  EXPECT_FALSE(nvhttp::prepare_dual_display_pair_after_gates(request, context, provider, failure));
  EXPECT_EQ(failure, "dual_display_permission_denied");
  EXPECT_EQ(provider_calls, 0);

  context.permission_granted = true;
  failure.clear();
  const auto pair = nvhttp::prepare_dual_display_pair_after_gates(request, context, provider, failure);
  ASSERT_TRUE(pair);
  EXPECT_TRUE(failure.empty());
  EXPECT_EQ(provider_calls, 1);
  EXPECT_EQ(pair->owner_identity, "paired-client-server-id");
}

TEST(NvHttpLaunchContractTest, DisplayAllocationFailureIsPreservedWithoutNetworkFallback) {
  const auto request = std::optional<dual_display_launch::request_t> {make_request()};
  const nvhttp::dual_display_allocation_context_t context {
    .no_active_sessions = true,
    .no_pending_launch_sessions = true,
    .input_only = false,
    .permission_granted = true,
    .encoder_supports_dual_display_capture = true,
    .authenticated_client_identity = "paired-client-server-id",
  };
  int provider_calls = 0;
  std::string failure;

  const auto pair = nvhttp::prepare_dual_display_pair_after_gates(
    request,
    context,
    [&](const dual_display_launch::request_t &, std::string_view, std::string &provider_failure) {
      ++provider_calls;
      provider_failure = "display_a_allocation_failed";
      return std::shared_ptr<dual_display_launch::prepared_pair_t> {};
    },
    failure
  );

  EXPECT_FALSE(pair);
  EXPECT_EQ(provider_calls, 1);
  EXPECT_EQ(failure, "display_a_allocation_failed");
}

TEST(NvHttpLaunchContractTest, PreparedPairKeepsPrivateCaptureNamesOutOfThePublicManifest) {
  int owner_release_count = 0;
  auto pair = make_prepared_pair(owner_release_count);

  EXPECT_FALSE(dual_display_launch::validate_prepared_pair(*pair).has_value());
  const auto response_manifest = dual_display_launch::serialize_manifest(pair->manifest);
  EXPECT_EQ(response_manifest.find("DISPLAY-PRIVATE-A"), std::string::npos);
  EXPECT_EQ(response_manifest.find("paired-client-server-id"), std::string::npos);
  EXPECT_NE(response_manifest.find("hostDisplayIdentity"), std::string::npos);
}

TEST(NvHttpLaunchContractTest, CompositeSdpMustMatchThePreparedPairProfile) {
  int owner_release_count = 0;
  const auto pair = make_prepared_pair(owner_release_count);
  std::string failure;

  EXPECT_TRUE(dual_display_launch::validate_composite_announcement(*pair, 3840, 1080, 60, failure));
  EXPECT_TRUE(failure.empty());

  EXPECT_FALSE(dual_display_launch::validate_composite_announcement(*pair, 1920, 1080, 60, failure));
  EXPECT_EQ(failure, "dual_display_sdp_dimensions_mismatch");
}
