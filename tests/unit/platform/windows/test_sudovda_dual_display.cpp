/**
 * @file tests/unit/platform/windows/test_sudovda_dual_display.cpp
 * @brief Tests for generation-bound virtual display pair ownership.
 */
#ifdef _WIN32
  #include <winsock2.h>
#endif

#include "../../../tests_common.h"

#ifdef _WIN32

  #include <array>
  #include <cstdint>
  #include <memory>
  #include <src/platform/windows/sudovda_dual_display.h>
  #include <src/rtsp.h>
  #include <src/stream.h>
  #include <string>

namespace {
  platf::sudovda::dual::display_pair_request_t make_request() {
    platf::sudovda::dual::display_pair_request_t request;
    request.session_generation = 9;
    request.display_pair_id = "pair-9";
    request.logical_display_ids = {"display-a", "display-b"};
    for (auto &display_request : request.display_requests) {
      display_request.width = 1920;
      display_request.height = 1080;
      display_request.refresh_rate = 60;
    }
    return request;
  }

  dual_display_launch::request_t make_launch_request() {
    return dual_display_launch::request_t {
      .schema_version = dual_display_launch::current_schema_version,
      .session_generation = 9,
      .display_pair_id = "pair-9",
      .topology_fingerprint = "topology-opaque",
      .input_mapping_version = dual_display_launch::current_input_mapping_version,
      .panes = {
        dual_display_launch::request_pane_t {"display-a", "client-a", 1920, 1080, 60, "PixelPerfect", "Sdr", "Bgra8Unorm", "Landscape"},
        dual_display_launch::request_pane_t {"display-b", "client-b", 1920, 1080, 60, "PixelPerfect", "Sdr", "Bgra8Unorm", "Landscape"},
      }
    };
  }

  platf::sudovda::dual::display_pair_allocation_t make_allocation(
    const std::string &logical_id,
    const std::string &host_identity,
    const std::string &provider_name,
    int &release_count
  ) {
    return platf::sudovda::dual::display_pair_allocation_t {
      {logical_id, host_identity, provider_name},
      [&release_count] {
        ++release_count;
      }
    };
  }
}  // namespace

TEST(SudoVdaDualDisplayTest, PairRequestRequiresTwoDistinctLogicalDisplaysAndValidModes) {
  auto request = make_request();

  EXPECT_FALSE(platf::sudovda::dual::validate_request(request).has_value());
  request.session_generation = 0;
  const auto error = platf::sudovda::dual::validate_request(request);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(*error, "invalid_session_generation");
}

TEST(SudoVdaDualDisplayTest, SecondAllocationFailureRollsBackFirstAllocation) {
  const auto request = make_request();
  int release_count = 0;
  std::string failure;

  const auto lease = platf::sudovda::dual::display_pair_lease_t::acquire(
    request,
    [&](const std::size_t index, const platf::sudovda::display_request_t &) -> std::optional<platf::sudovda::dual::display_pair_allocation_t> {
      if (index == 1) {
        return std::nullopt;
      }
      return make_allocation("display-a", "host-a", "SudoVDA-A", release_count);
    },
    failure
  );

  EXPECT_FALSE(lease.has_value());
  EXPECT_EQ(failure, "display_b_allocation_failed");
  EXPECT_EQ(release_count, 1);
}

TEST(SudoVdaDualDisplayTest, TransientFirstAllocationFailureIsRetriedOnce) {
  const auto request = make_request();
  int first_pane_attempts = 0;
  int release_count = 0;
  std::string failure;

  auto lease = platf::sudovda::dual::display_pair_lease_t::acquire(
    request,
    [&](const std::size_t index, const platf::sudovda::display_request_t &) -> std::optional<platf::sudovda::dual::display_pair_allocation_t> {
      if (index == 0 && ++first_pane_attempts == 1) {
        return std::nullopt;
      }
      return make_allocation(
        request.logical_display_ids[index],
        "host-" + std::to_string(index),
        "SudoVDA-" + std::to_string(index),
        release_count
      );
    },
    failure
  );

  ASSERT_TRUE(lease.has_value());
  EXPECT_TRUE(failure.empty());
  EXPECT_EQ(first_pane_attempts, 2);
  lease->release();
  EXPECT_EQ(release_count, 2);
}

TEST(SudoVdaDualDisplayTest, ProvisionalProviderNameCollisionWaitsForCommittedTopologyRemap) {
  const auto request = make_request();
  int release_count = 0;
  std::string failure;

  auto lease = platf::sudovda::dual::display_pair_lease_t::acquire(
    request,
    [&](const std::size_t index, const platf::sudovda::display_request_t &) -> std::optional<platf::sudovda::dual::display_pair_allocation_t> {
      return make_allocation(
        request.logical_display_ids[index],
        "host-" + std::to_string(index),
        "provisional-provider-name",
        release_count
      );
    },
    failure
  );

  ASSERT_TRUE(lease.has_value());
  EXPECT_TRUE(failure.empty());
  EXPECT_FALSE(platf::sudovda::dual::pair_identity_matches(request, lease->panes()));
  EXPECT_FALSE(lease->remap_provider_display_names({"same-provider", "same-provider"}));
  EXPECT_TRUE(lease->remap_provider_display_names({R"(\\.\DISPLAY16)", R"(\\.\DISPLAY17)"}));
  EXPECT_TRUE(platf::sudovda::dual::pair_identity_matches(request, lease->panes()));
  lease->release();
  EXPECT_EQ(release_count, 2);
}

TEST(SudoVdaDualDisplayTest, CommittedPairRemapsHostIdentityAfterTopologyInventoryChurn) {
  const auto request = make_request();
  int release_count = 0;
  std::string failure;

  auto lease = platf::sudovda::dual::display_pair_lease_t::acquire(
    request,
    [&](const std::size_t index, const platf::sudovda::display_request_t &) -> std::optional<platf::sudovda::dual::display_pair_allocation_t> {
      return make_allocation(
        request.logical_display_ids[index],
        "stale-host-" + std::to_string(index),
        "SudoVDA-" + std::to_string(index),
        release_count
      );
    },
    failure
  );

  ASSERT_TRUE(lease.has_value());
  EXPECT_TRUE(lease->remap_host_display_identities({"current-host-left", "current-host-right"}));
  EXPECT_EQ(lease->panes()[0].host_display_identity, "current-host-left");
  EXPECT_EQ(lease->panes()[1].host_display_identity, "current-host-right");
  lease->release();
  EXPECT_EQ(release_count, 2);
}

TEST(SudoVdaDualDisplayTest, CommittedPairTeardownIsReverseOrderedAndIdempotent) {
  const auto request = make_request();
  std::array<int, 2> release_order {-1, -1};
  int release_count = 0;
  std::string failure;
  auto lease = platf::sudovda::dual::display_pair_lease_t::acquire(
    request,
    [&](const std::size_t index, const platf::sudovda::display_request_t &) -> std::optional<platf::sudovda::dual::display_pair_allocation_t> {
      return platf::sudovda::dual::display_pair_allocation_t {
        {request.logical_display_ids[index], "host-" + std::to_string(index), "SudoVDA-" + std::to_string(index)},
        [&release_order, &release_count, index] {
          release_order[release_count++] = static_cast<int>(index);
        }
      };
    },
    failure
  );

  ASSERT_TRUE(lease.has_value());
  EXPECT_TRUE(lease->committed());
  lease->release();
  lease->release();
  EXPECT_TRUE(lease->closed());
  EXPECT_EQ(release_count, 2);
  EXPECT_EQ(release_order, (std::array<int, 2> {1, 0}));
}

TEST(SudoVdaDualDisplayTest, CurrentIdentityMustMatchBothLogicalAndObservedProviderIdentities) {
  const auto request = make_request();
  int release_count = 0;
  std::string failure;
  auto lease = platf::sudovda::dual::display_pair_lease_t::acquire(
    request,
    [&](const std::size_t index, const platf::sudovda::display_request_t &) -> std::optional<platf::sudovda::dual::display_pair_allocation_t> {
      return make_allocation(
        request.logical_display_ids[index],
        "host-" + std::to_string(index),
        "SudoVDA-" + std::to_string(index),
        release_count
      );
    },
    failure
  );

  ASSERT_TRUE(lease.has_value());
  auto observed = lease->panes();
  EXPECT_TRUE(lease->matches_current_identity(observed));
  observed[1].host_display_identity = "wrong-host";
  EXPECT_FALSE(lease->matches_current_identity(observed));
}

TEST(SudoVdaDualDisplayTest, PartialDuplicationProbeDefersToAuthoritativePairCapture) {
  const std::array<std::string, 2> current_names {R"(\\.\DISPLAY16)", R"(\\.\DISPLAY17)"};
  const std::vector<std::string> capturable_names {R"(\\.\DISPLAY17)"};

  EXPECT_EQ(
    platf::sudovda::dual::classify_capture_preflight(current_names, capturable_names),
    platf::sudovda::dual::capture_preflight_e::duplication_probe_partial
  );
}

TEST(SudoVdaDualDisplayTest, CompleteDuplicationProbeIsReady) {
  const std::array<std::string, 2> current_names {R"(\\.\DISPLAY16)", R"(\\.\DISPLAY17)"};
  const std::vector<std::string> capturable_names {R"(\\.\DISPLAY16)", R"(\\.\DISPLAY17)"};

  EXPECT_EQ(
    platf::sudovda::dual::classify_capture_preflight(current_names, capturable_names),
    platf::sudovda::dual::capture_preflight_e::ready
  );
}

TEST(SudoVdaDualDisplayTest, MissingBothDuplicationProbesStillFailsClosed) {
  const std::array<std::string, 2> current_names {R"(\\.\DISPLAY16)", R"(\\.\DISPLAY17)"};

  EXPECT_EQ(
    platf::sudovda::dual::classify_capture_preflight(current_names, {}),
    platf::sudovda::dual::capture_preflight_e::duplication_probe_unavailable
  );
}

TEST(SudoVdaDualDisplayTest, MissingOrDuplicateCurrentIdentityStillFailsClosed) {
  EXPECT_EQ(
    platf::sudovda::dual::classify_capture_preflight({R"(\\.\DISPLAY16)", ""}, {}),
    platf::sudovda::dual::capture_preflight_e::identity_unavailable
  );
  EXPECT_EQ(
    platf::sudovda::dual::classify_capture_preflight(
      {R"(\\.\DISPLAY16)", R"(\\.\DISPLAY16)"},
      {R"(\\.\DISPLAY16)"}
    ),
    platf::sudovda::dual::capture_preflight_e::duplicate_output
  );
}

TEST(SudoVdaDualDisplayTest, PersistentPaneRequestsAreBoundToTheServerKnownOwner) {
  const auto launch_request = make_launch_request();
  const auto prepared = platf::sudovda::dual::make_pair_request(
    launch_request,
    "paired-client-server-id"
  );

  ASSERT_TRUE(prepared.has_value());
  EXPECT_EQ(prepared->logical_display_ids[0], "display-a");
  EXPECT_EQ(prepared->logical_display_ids[1], "display-b");
  EXPECT_NE(prepared->display_requests[0].serial_number, prepared->display_requests[1].serial_number);

  auto altered_request = launch_request;
  altered_request.panes[0].client_display_id = "attacker-controlled-client-id";
  const auto altered = platf::sudovda::dual::make_pair_request(
    altered_request,
    "paired-client-server-id"
  );
  ASSERT_TRUE(altered.has_value());
  EXPECT_EQ(altered->display_requests[0].serial_number, prepared->display_requests[0].serial_number);
}

TEST(SudoVdaDualDisplayTest, PreparedPairKeepsOneLeaseAliveThroughStreamConfiguration) {
  const auto pair_request = make_request();
  std::array<int, 2> release_order {-1, -1};
  int release_count = 0;
  std::string failure;
  auto lease = platf::sudovda::dual::display_pair_lease_t::acquire(
    pair_request,
    [&](const std::size_t index, const platf::sudovda::display_request_t &) -> std::optional<platf::sudovda::dual::display_pair_allocation_t> {
      return platf::sudovda::dual::display_pair_allocation_t {
        {pair_request.logical_display_ids[index], "host-" + std::to_string(index), "SudoVDA-" + std::to_string(index)},
        [&release_order, &release_count, index] {
          release_order[release_count++] = static_cast<int>(index);
        }
      };
    },
    failure
  );
  ASSERT_TRUE(lease.has_value());

  auto prepared_pair = platf::sudovda::dual::make_prepared_pair(
    make_launch_request(),
    "paired-client-server-id",
    std::move(*lease),
    "2030-01-01T00:00:00Z",
    failure
  );
  ASSERT_TRUE(prepared_pair);
  EXPECT_TRUE(failure.empty());
  EXPECT_EQ(prepared_pair->provider_display_names, (std::array<std::string, 2> {"SudoVDA-0", "SudoVDA-1"}));

  {
    video::config_t monitor {};
    monitor.dual_display_pair = prepared_pair;
    EXPECT_TRUE(video::requires_dual_display_compositor(monitor));
    EXPECT_EQ(video::dual_display_capture_backend_pending_state, "dual_display_pair_capture_backend_pending");
    stream::config_t stream_config {};
    stream_config.monitor = monitor;
    prepared_pair.reset();
    monitor.dual_display_pair.reset();

    EXPECT_EQ(release_count, 0);
    EXPECT_TRUE(stream_config.monitor.dual_display_pair);
  }

  EXPECT_EQ(release_count, 2);
  EXPECT_EQ(release_order, (std::array<int, 2> {1, 0}));
}

TEST(SudoVdaDualDisplayTest, PendingTimeoutCannotReleaseThePairBeforeActiveTeardown) {
  const auto pair_request = make_request();
  std::array<int, 2> release_order {-1, -1};
  int release_count = 0;
  std::string failure;
  auto lease = platf::sudovda::dual::display_pair_lease_t::acquire(
    pair_request,
    [&](const std::size_t index, const platf::sudovda::display_request_t &) -> std::optional<platf::sudovda::dual::display_pair_allocation_t> {
      return platf::sudovda::dual::display_pair_allocation_t {
        {pair_request.logical_display_ids[index], "host-" + std::to_string(index), "SudoVDA-" + std::to_string(index)},
        [&release_order, &release_count, index] {
          release_order[release_count++] = static_cast<int>(index);
        }
      };
    },
    failure
  );
  ASSERT_TRUE(lease.has_value());

  auto prepared_pair = platf::sudovda::dual::make_prepared_pair(
    make_launch_request(),
    "paired-client-server-id",
    std::move(*lease),
    "2030-01-01T00:00:00Z",
    failure
  );
  ASSERT_TRUE(prepared_pair);

  {
    stream::config_t active_stream {};
    active_stream.monitor.dual_display_pair = prepared_pair;
    auto pending_timeout_reference = prepared_pair;
    prepared_pair.reset();

    // RTSP timeout removes only its pending reference. The active stream owns
    // the exact same RAII pair until normal teardown.
    pending_timeout_reference.reset();
    EXPECT_EQ(release_count, 0);
  }

  EXPECT_EQ(release_count, 2);
  EXPECT_EQ(release_order, (std::array<int, 2> {1, 0}));
}

#endif
