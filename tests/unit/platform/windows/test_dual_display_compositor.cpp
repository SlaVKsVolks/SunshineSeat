/**
 * @file tests/unit/platform/windows/test_dual_display_compositor.cpp
 * @brief Tests for the two-pane D3D11 capture policy primitives.
 */
#include "../../../tests_common.h"

#ifdef _WIN32

  #include <src/platform/windows/dual_display_compositor.h>

  #include <array>
  #include <chrono>
  #include <cstdint>
  #include <vector>

namespace {
  using namespace std::chrono_literals;
  using platf::dual_display::capture_route_e;
  using platf::dual_display::frame_policy_e;
  using platf::dual_display::pane_cache_t;
  using platf::dual_display::pane_contract_t;
  using platf::dual_display::pane_observation_t;
  using platf::dual_display::pair_validation_e;

  std::array<pane_contract_t, platf::dual_display::display_pair_size> exact_contract() {
    return {{
      {"SudoVDA-A", 1920, 1080, 60, true, true, true},
      {"SudoVDA-B", 1920, 1080, 60, true, true, true},
    }};
  }

  std::array<pane_observation_t, platf::dual_display::display_pair_size> exact_observation() {
    return {{
      {"SudoVDA-A", 0xA11CE, true, 1920, 1080, 60, true, true, true},
      {"SudoVDA-B", 0xA11CE, true, 1920, 1080, 60, true, true, true},
    }};
  }
}

TEST(DualDisplayCompositorTest, AcceptsOnlyTheBoundedV1PairGeometry) {
  const auto result = platf::dual_display::validate_pair_capture(
    exact_contract(),
    exact_observation()
  );

  ASSERT_EQ(result.state, pair_validation_e::accepted);
  EXPECT_EQ(result.geometry.width, 3840);
  EXPECT_EQ(result.geometry.height, 1080);
  EXPECT_EQ(result.geometry.panes[0], (platf::dual_display::rect_t {0, 0, 1920, 1080}));
  EXPECT_EQ(result.geometry.panes[1], (platf::dual_display::rect_t {1920, 0, 1920, 1080}));
}

TEST(DualDisplayCompositorTest, KeepsPaneEncoderDimensionsSeparateFromCompositeAllocation) {
  const auto plan = platf::dual_display::multi_capture_surface_plan();

  EXPECT_EQ(plan.encoder.width, 1920);
  EXPECT_EQ(plan.encoder.height, 1080);
  EXPECT_EQ(plan.composite.width, 3840);
  EXPECT_EQ(plan.composite.height, 1080);
}

TEST(DualDisplayCompositorTest, AcceptsEquivalentDxgiRefreshRateRepresentations) {
  EXPECT_TRUE(platf::dual_display::matches_pane_refresh_rate(60, 1));
  EXPECT_TRUE(platf::dual_display::matches_pane_refresh_rate(59940, 1000));
  EXPECT_TRUE(platf::dual_display::matches_pane_refresh_rate(60000, 1000));
  EXPECT_FALSE(platf::dual_display::matches_pane_refresh_rate(0, 0));
  EXPECT_FALSE(platf::dual_display::matches_pane_refresh_rate(30000, 1000));
}

TEST(DualDisplayCompositorTest, RejectsDuplicatePreparedProviderNames) {
  auto contract = exact_contract();
  contract[1].provider_display_name = contract[0].provider_display_name;

  EXPECT_EQ(
    platf::dual_display::validate_pair_capture(contract, exact_observation()).state,
    pair_validation_e::duplicate_provider_name
  );
}

TEST(DualDisplayCompositorTest, RejectsCrossAdapterOutputsBeforeCreatingDuplications) {
  auto observation = exact_observation();
  observation[1].adapter_identity = 0xB22CE;

  EXPECT_EQ(
    platf::dual_display::validate_pair_capture(exact_contract(), observation).state,
    pair_validation_e::cross_adapter
  );
}

TEST(DualDisplayCompositorTest, RejectsDetachedOutput) {
  auto observation = exact_observation();
  observation[1].attached_to_desktop = false;

  EXPECT_EQ(
    platf::dual_display::validate_pair_capture(exact_contract(), observation).state,
    pair_validation_e::output_detached
  );
}

TEST(DualDisplayCompositorTest, RejectsManifestOutputModeMismatch) {
  auto observation = exact_observation();
  observation[1].width = 2560;

  EXPECT_EQ(
    platf::dual_display::validate_pair_capture(exact_contract(), observation).state,
    pair_validation_e::manifest_output_mode_mismatch
  );
}

TEST(DualDisplayCompositorTest, RejectsHdrOrNonBgraOutput) {
  auto observation = exact_observation();
  observation[1].sdr = false;

  EXPECT_EQ(
    platf::dual_display::validate_pair_capture(exact_contract(), observation).state,
    pair_validation_e::unsupported_output_mode
  );
}

TEST(DualDisplayCompositorTest, WaitsForBothPaneCachesBeforeEmittingAComposite) {
  const auto now = std::chrono::steady_clock::now();
  const std::array<pane_cache_t, platf::dual_display::display_pair_size> cache {{
    {true, now, now},
    {false, std::nullopt, std::nullopt},
  }};

  EXPECT_EQ(
    platf::dual_display::evaluate_pair_frame(cache, now).state,
    frame_policy_e::first_frame_pending
  );
}

TEST(DualDisplayCompositorTest, UsesTheMaximumFreshPaneTimestamp) {
  const auto now = std::chrono::steady_clock::now();
  const auto first_timestamp = now - 8ms;
  const std::array<pane_cache_t, platf::dual_display::display_pair_size> cache {{
    {true, first_timestamp, first_timestamp},
    {true, now, now},
  }};

  const auto result = platf::dual_display::evaluate_pair_frame(cache, now);

  ASSERT_EQ(result.state, frame_policy_e::ready);
  ASSERT_TRUE(result.timestamp.has_value());
  EXPECT_EQ(*result.timestamp, now);
}

TEST(DualDisplayCompositorTest, UsesCacheCopyTimeForFreshnessWhenDxgiPresentTimeIsBatched) {
  const auto now = std::chrono::steady_clock::now();
  const auto old_present = now - 300ms;
  const std::array<pane_cache_t, platf::dual_display::display_pair_size> cache {{
    {true, now, old_present},
    {true, now, now},
  }};

  const auto result = platf::dual_display::evaluate_pair_frame(cache, now);

  EXPECT_EQ(result.state, frame_policy_e::skew_exceeded);
  ASSERT_TRUE(result.timestamp.has_value());
  EXPECT_EQ(*result.timestamp, now);
}

TEST(DualDisplayCompositorTest, RejectsAStalePaneCache) {
  const auto now = std::chrono::steady_clock::now();
  const std::array<pane_cache_t, platf::dual_display::display_pair_size> cache {{
    {true, now - platf::dual_display::maximum_pane_age - 1ms, now - platf::dual_display::maximum_pane_age - 1ms},
    {true, now, now},
  }};

  EXPECT_EQ(
    platf::dual_display::evaluate_pair_frame(cache, now).state,
    frame_policy_e::stale_pane
  );
}

TEST(DualDisplayCompositorTest, KeepsCompositeTimestampWhenAStaticPaneIsStale) {
  const auto now = std::chrono::steady_clock::now();
  const std::array<pane_cache_t, platf::dual_display::display_pair_size> cache {{
    {true, now - platf::dual_display::maximum_pane_age - 1ms, now - platf::dual_display::maximum_pane_age - 1ms},
    {true, now, now},
  }};

  const auto result = platf::dual_display::evaluate_pair_frame(cache, now);

  EXPECT_EQ(result.state, frame_policy_e::stale_pane);
  ASSERT_TRUE(result.timestamp.has_value());
  EXPECT_EQ(*result.timestamp, now);
}

TEST(DualDisplayCompositorTest, RefreshesCompositeTimestampWhenBothStaticPanesAreStale) {
  const auto now = std::chrono::steady_clock::now();
  const std::array<pane_cache_t, platf::dual_display::display_pair_size> cache {{
    {true, now - platf::dual_display::maximum_pane_age - 2ms, now - platf::dual_display::maximum_pane_age - 2ms},
    {true, now - platf::dual_display::maximum_pane_age - 1ms, now - platf::dual_display::maximum_pane_age - 1ms},
  }};

  const auto result = platf::dual_display::evaluate_pair_frame(cache, now);

  EXPECT_EQ(result.state, frame_policy_e::stale_pane);
  ASSERT_TRUE(result.timestamp.has_value());
  EXPECT_EQ(*result.timestamp, now);
}

TEST(DualDisplayCompositorTest, KeepsFreshPaneSkewAdvisory) {
  const auto now = std::chrono::steady_clock::now();
  const std::array<pane_cache_t, platf::dual_display::display_pair_size> cache {{
    {true, now - platf::dual_display::maximum_pane_skew - 1ms, now - platf::dual_display::maximum_pane_skew - 1ms},
    {true, now, now},
  }};

  const auto result = platf::dual_display::evaluate_pair_frame(cache, now);

  EXPECT_EQ(result.state, frame_policy_e::skew_exceeded);
  ASSERT_TRUE(result.timestamp.has_value());
  EXPECT_EQ(*result.timestamp, now);
}

TEST(DualDisplayCompositorTest, BoundsSequentialPaneCaptureWaitForOneRefresh) {
  EXPECT_EQ(
    platf::dual_display::maximum_pane_capture_wait,
    8ms
  );
}

TEST(DualDisplayCompositorTest, SharesOneWaitDeadlineAcrossBothPanes) {
  const auto start = std::chrono::steady_clock::time_point {};
  const auto deadline = start + platf::dual_display::maximum_pane_capture_wait;

  EXPECT_EQ(platf::dual_display::pane_capture_wait_remaining(deadline, start), 8ms);
  EXPECT_EQ(platf::dual_display::pane_capture_wait_remaining(deadline, start + 5ms), 3ms);
  EXPECT_EQ(platf::dual_display::pane_capture_wait_remaining(deadline, start + 8ms), 0ms);
  EXPECT_EQ(platf::dual_display::pane_capture_wait_remaining(deadline, start + 9ms), 0ms);
}

TEST(DualDisplayCompositorTest, AcquiresAThenBAndReleasesBThenA) {
  std::vector<std::size_t> acquire_order;
  std::vector<std::size_t> release_order;

  platf::dual_display::for_each_pane_acquire([&](const std::size_t pane) {
    acquire_order.push_back(pane);
  });
  platf::dual_display::for_each_pane_release([&](const std::size_t pane) {
    release_order.push_back(pane);
  });

  EXPECT_EQ(acquire_order, (std::vector<std::size_t> {0, 1}));
  EXPECT_EQ(release_order, (std::vector<std::size_t> {1, 0}));
}

TEST(DualDisplayCompositorTest, LeavesOrdinarySingleDisplaySelectionUnchanged) {
  EXPECT_EQ(
    platf::dual_display::select_capture_route(false, true, true),
    capture_route_e::ordinary
  );
  EXPECT_EQ(
    platf::dual_display::select_capture_route(false, false, false),
    capture_route_e::ordinary
  );
  EXPECT_EQ(
    platf::dual_display::select_capture_route(true, true, true),
    capture_route_e::paired_d3d11
  );
  EXPECT_EQ(
    platf::dual_display::select_capture_route(true, false, true),
    capture_route_e::rejected
  );
  EXPECT_EQ(
    platf::dual_display::select_capture_route(true, true, false),
    capture_route_e::rejected
  );
}

TEST(DualDisplayCompositorTest, RejectsSoftwareProfileBeforeVirtualPairPreparation) {
  EXPECT_EQ(
    platf::dual_display::select_prepared_pair_profile(false),
    platf::dual_display::prepared_pair_profile_e::rejected_before_allocation
  );
  EXPECT_EQ(
    platf::dual_display::select_prepared_pair_profile(true),
    platf::dual_display::prepared_pair_profile_e::d3d11_ready
  );
}

#endif
