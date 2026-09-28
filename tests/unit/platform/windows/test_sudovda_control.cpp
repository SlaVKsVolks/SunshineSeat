/**
 * @file tests/unit/platform/windows/test_sudovda_control.cpp
 * @brief Test src/platform/windows/sudovda_control.cpp request helpers.
 */
#include "../../../tests_common.h"

#ifdef _WIN32
  #include <src/platform/windows/sudovda_control.h>
  #include <src/platform/windows/display_reliability.h>
#endif

#ifdef _WIN32

namespace platf::sudovda {
  [[nodiscard]] std::optional<std::vector<enumerated_display_t>> remap_sudovda_stream_inventory(
    const std::vector<enumerated_display_t> &requested,
    const std::vector<enumerated_display_t> &current
  );
}

TEST(SudoVdaControlTest, MakePersistentRequestUsesStableIdentityDerivedSerial) {
  const auto request = platf::sudovda::make_persistent_request(1920, 1080, 60, "3E280853-16CD-D70B-6C18-C58BCEB2D608");

  EXPECT_EQ(request.width, 1920U);
  EXPECT_EQ(request.height, 1080U);
  EXPECT_EQ(request.refresh_rate, 60U);
  EXPECT_EQ(request.device_name, "SUNSEAT");
  EXPECT_EQ(request.serial_number, "SSD44A611DF4");
}

TEST(SudoVdaControlTest, UnexpectedDisconnectKeepsLeaseDuringReconnectGrace) {
  EXPECT_FALSE(platf::sudovda::should_release_virtual_display_lease(false, 0s, 30s));
  EXPECT_FALSE(platf::sudovda::should_release_virtual_display_lease(false, 29s, 30s));
  EXPECT_TRUE(platf::sudovda::should_release_virtual_display_lease(false, 30s, 30s));
  EXPECT_TRUE(platf::sudovda::should_release_virtual_display_lease(true, 0s, 30s));
}

TEST(SudoVdaControlTest, RepeatedIdlePollDoesNotRestartReconnectGrace) {
  EXPECT_TRUE(platf::sudovda::should_schedule_virtual_display_release(true, false));
  EXPECT_FALSE(platf::sudovda::should_schedule_virtual_display_release(true, true));
  EXPECT_FALSE(platf::sudovda::should_schedule_virtual_display_release(false, false));
}

TEST(SudoVdaControlTest, PhysicalPanelPowerRemainsUntouchedByVirtualDisplayLease) {
  using action_e = platf::display_reliability::physical_panel_power_action_e;

  EXPECT_EQ(
    platf::display_reliability::physical_panel_power_action(true, true, false),
    action_e::none
  );
  EXPECT_EQ(
    platf::display_reliability::physical_panel_power_action(true, true, true),
    action_e::none
  );
  EXPECT_EQ(
    platf::display_reliability::physical_panel_power_action(false, true, true),
    action_e::none
  );
  EXPECT_EQ(
    platf::display_reliability::physical_panel_power_action(false, true, false),
    action_e::none
  );
  EXPECT_EQ(
    platf::display_reliability::physical_panel_power_action(true, false, false),
    action_e::none
  );
}

TEST(SudoVdaControlTest, MakePersistentRequestKeepsDifferentIdentitiesDistinct) {
  const auto live_request = platf::sudovda::make_persistent_request(1920, 1080, 60, "3E280853-16CD-D70B-6C18-C58BCEB2D608");
  const auto synthetic_request = platf::sudovda::make_persistent_request(1920, 1080, 60, "sunshineseat-sudovda-provider-test");

  EXPECT_EQ(synthetic_request.serial_number, "SSAC0D80B0BD");
  EXPECT_NE(live_request.serial_number, synthetic_request.serial_number);
  EXPECT_FALSE(IsEqualGUID(live_request.monitor_guid, synthetic_request.monitor_guid));
}

TEST(SudoVdaControlTest, CreateFailureRollbackTreatsDisplayAlreadyAbsentAsSuccess) {
  EXPECT_TRUE(platf::sudovda::is_successful_remove_result(false, ERROR_NOT_FOUND));
  EXPECT_TRUE(platf::sudovda::is_successful_remove_result(true, ERROR_SUCCESS));
  EXPECT_FALSE(platf::sudovda::is_successful_remove_result(false, ERROR_GEN_FAILURE));
}

TEST(SudoVdaControlTest, WgcCaptureDoesNotRequireDxgiDuplicationProbe) {
  EXPECT_FALSE(platf::sudovda::capture_requires_dxgi_duplication_probe("wgc"));
  EXPECT_TRUE(platf::sudovda::capture_requires_dxgi_duplication_probe("ddx"));
}

TEST(SudoVdaControlTest, DualPairCreationDefersProviderCapturabilityUntilCommittedTopology) {
  EXPECT_TRUE(platf::sudovda::requires_provider_capturability_blocker(false, true));
  EXPECT_FALSE(platf::sudovda::requires_provider_capturability_blocker(false, false));
  EXPECT_FALSE(platf::sudovda::requires_provider_capturability_blocker(true, true));
}

TEST(SudoVdaControlTest, InitialSidecarSudoVdaEncoderProbeUsesStartupValidation) {
  EXPECT_TRUE(platf::display_reliability::should_defer_initial_sudovda_encoder_reenumeration(
    true,
    "sudovda",
    true,
    true
  ));
  EXPECT_FALSE(platf::display_reliability::should_defer_initial_sudovda_encoder_reenumeration(
    true,
    "sudovda",
    false,
    true
  ));
  EXPECT_FALSE(platf::display_reliability::should_defer_initial_sudovda_encoder_reenumeration(
    true,
    "desktop",
    true,
    true
  ));
  EXPECT_FALSE(platf::display_reliability::should_defer_initial_sudovda_encoder_reenumeration(
    true,
    "sudovda",
    true,
    false
  ));
}

TEST(SudoVdaControlTest, CachedVirtualDisplayProbeIsBoundedBelowControlPingTimeout) {
  EXPECT_EQ(platf::display_reliability::sudovda_cached_display_probe_timeout(), 750ms);
  EXPECT_LT(platf::display_reliability::sudovda_cached_display_probe_timeout(), 5s);
}

TEST(SudoVdaControlTest, NewAllocationRetriesOneTransientStabilizationFailure) {
  EXPECT_TRUE(platf::display_reliability::should_retry_sudovda_allocation(false, false, 1));
  EXPECT_FALSE(platf::display_reliability::should_retry_sudovda_allocation(false, false, 2));
  EXPECT_FALSE(platf::display_reliability::should_retry_sudovda_allocation(false, true, 1));
  EXPECT_FALSE(platf::display_reliability::should_retry_sudovda_allocation(true, false, 1));
}

TEST(SudoVdaControlTest, ReleaseRetriesOneTransientTeardownFailure) {
  EXPECT_TRUE(platf::display_reliability::should_retry_sudovda_release(false, 1));
  EXPECT_FALSE(platf::display_reliability::should_retry_sudovda_release(false, 2));
  EXPECT_FALSE(platf::display_reliability::should_retry_sudovda_release(true, 1));
}

TEST(SudoVdaControlTest, ReconnectDoesNotReadTopologyDuringIntentionalReleaseWindow) {
  EXPECT_TRUE(platf::display_reliability::should_defer_sudovda_allocation(true));
  EXPECT_FALSE(platf::display_reliability::should_defer_sudovda_allocation(false));
  EXPECT_FALSE(platf::display_reliability::should_attempt_sudovda_topology_set(false));
  EXPECT_TRUE(platf::display_reliability::should_attempt_sudovda_topology_set(true));
}

TEST(SudoVdaControlTest, WatchdogKeepaliveLeavesMultiplePingsBeforeDriverExpiry) {
  EXPECT_EQ(platf::sudovda::watchdog_keepalive_interval(3), 1000ms);
  EXPECT_EQ(platf::sudovda::watchdog_keepalive_interval(1), 333ms);
  EXPECT_EQ(platf::sudovda::watchdog_keepalive_interval(0), 0ms);
}

TEST(SudoVdaControlTest, MissingCachedDeviceIsRecreatedWhenDeviceInventoryIsReliable) {
  EXPECT_TRUE(platf::display_reliability::should_recreate_missing_sudovda_allocation(true, true, true, true, false));
  EXPECT_TRUE(platf::display_reliability::should_recreate_missing_sudovda_allocation(true, true, true, false, false));
  EXPECT_FALSE(platf::display_reliability::should_recreate_missing_sudovda_allocation(true, true, false, false, false));
  EXPECT_FALSE(platf::display_reliability::should_recreate_missing_sudovda_allocation(true, false, true, true, false));
  EXPECT_FALSE(platf::display_reliability::should_recreate_missing_sudovda_allocation(true, true, true, true, true));
}

TEST(SudoVdaControlTest, StreamManagedSidecarDoesNotRetainVirtualDisplayAfterDisconnect) {
  EXPECT_FALSE(platf::display_reliability::should_retain_sudovda_display_for_reconnect(true, "sudovda"));
  EXPECT_TRUE(platf::display_reliability::should_retain_sudovda_display_for_reconnect(false, "sudovda"));
  EXPECT_TRUE(platf::display_reliability::should_retain_sudovda_display_for_reconnect(true, "desktop"));
}

TEST(SudoVdaControlTest, FindNewDisplayDeviceIdPrefersNewInactiveRequestedDisplay) {
  const std::vector<platf::sudovda::enumerated_display_t> before {
    {"device-physical", R"(\\.\DISPLAY17)", "Primary Monitor", true}
  };
  const std::vector<platf::sudovda::enumerated_display_t> after {
    {"device-physical", R"(\\.\DISPLAY17)", "Primary Monitor", true},
    {"device-sunseat", "", "Generic Monitor (SUNSEAT)", false}
  };

  EXPECT_EQ(
    platf::sudovda::find_new_display_device_id(before, after, "SUNSEAT"),
    std::optional<std::string> {"device-sunseat"}
  );
}

TEST(SudoVdaControlTest, FindNewDisplayDeviceIdFallsBackToRequestedPersistentDisplayWhenDeviceIdIsReused) {
  const std::vector<platf::sudovda::enumerated_display_t> before {
    {"device-physical", R"(\\.\DISPLAY17)", "Primary Monitor", true},
    {"device-sunseat", "", "Generic Monitor (SUNSEAT)", false}
  };
  const std::vector<platf::sudovda::enumerated_display_t> after {
    {"device-physical", R"(\\.\DISPLAY17)", "Primary Monitor", true},
    {"device-sunseat", "", "Generic Monitor (SUNSEAT)", false}
  };

  EXPECT_EQ(
    platf::sudovda::find_new_display_device_id(before, after, "SUNSEAT"),
    std::optional<std::string> {"device-sunseat"}
  );
}

TEST(SudoVdaControlTest, ResolveDeviceIdAfterDisplayNameResolutionFillsMissingDeviceId) {
  const std::vector<platf::sudovda::enumerated_display_t> before {
    {"device-physical", R"(\\.\DISPLAY17)", "Primary Monitor", true}
  };
  const std::vector<platf::sudovda::enumerated_display_t> after {
    {"device-physical", R"(\\.\DISPLAY17)", "Primary Monitor", true},
    {"device-sunseat", R"(\\.\DISPLAY6)", "Generic Monitor (SUNSEAT)", true}
  };

  EXPECT_EQ(
    platf::sudovda::resolve_device_id_after_display_name_resolution(
      std::nullopt,
      before,
      after,
      "SUNSEAT"
    ),
    std::optional<std::string> {"device-sunseat"}
  );
}

TEST(SudoVdaControlTest, RestoreTargetResolutionRemapsStaleDeviceIdByStableIdentity) {
  const std::vector<platf::sudovda::enumerated_display_t> baseline {
    {"stale-device-id", R"(\\.\DISPLAY1)", "Physical One", true, "edid:ACM-100-1", {1920, 1080, 60}}
  };
  const std::vector<platf::sudovda::enumerated_display_t> current {
    {"current-device-id", R"(\\.\DISPLAY7)", "Physical One", true, "edid:ACM-100-1", {1920, 1080, 60}}
  };

  const auto record = platf::sudovda::snapshot_display_transaction(baseline);
  const auto resolution = platf::sudovda::resolve_current_display_targets_for_restore(record, current);

  ASSERT_TRUE(resolution.healthy);
  ASSERT_EQ(resolution.targets.size(), 1U);
  EXPECT_EQ(resolution.targets.front().current_device_id, "current-device-id");
}

TEST(SudoVdaControlTest, RestoreTargetResolutionPrefersStableIdentityOverReusedStaleDeviceId) {
  const auto record = platf::sudovda::snapshot_display_transaction({
    {"stale-device-id", R"(\\.\DISPLAY1)", "Physical One", true, "edid:ACM-100-1", {1920, 1080, 60}}
  });
  const auto resolution = platf::sudovda::resolve_current_display_targets_for_restore(record, {
    {"stale-device-id", R"(\\.\DISPLAY7)", "Wrong Physical", true, "edid:OTHER-9", {1920, 1080, 60}},
    {"current-device-id", R"(\\.\DISPLAY8)", "Physical One", true, "edid:ACM-100-1", {1920, 1080, 60}}
  });

  ASSERT_TRUE(resolution.healthy);
  ASSERT_EQ(resolution.targets.size(), 1U);
  EXPECT_EQ(resolution.targets.front().current_device_id, "current-device-id");
}

TEST(SudoVdaControlTest, RestoreTargetResolutionFailsClosedForAmbiguousOrMissingTargets) {
  const auto record = platf::sudovda::snapshot_display_transaction({
    {"stale-device-id", R"(\\.\DISPLAY1)", "Physical One", true, "edid:ACM-100-1", {1920, 1080, 60}}
  });

  const auto ambiguous = platf::sudovda::resolve_current_display_targets_for_restore(record, {
    {"current-device-a", R"(\\.\DISPLAY7)", "Physical One", true, "edid:ACM-100-1", {1920, 1080, 60}},
    {"current-device-b", R"(\\.\DISPLAY8)", "Physical One", true, "edid:ACM-100-1", {1920, 1080, 60}}
  });
  EXPECT_FALSE(ambiguous.healthy);
  EXPECT_TRUE(ambiguous.rollback_required);
  EXPECT_NE(ambiguous.issues.end(), std::find(ambiguous.issues.begin(), ambiguous.issues.end(), "ambiguous_restore_target"));

  const auto missing = platf::sudovda::resolve_current_display_targets_for_restore(record, {});
  EXPECT_FALSE(missing.healthy);
  EXPECT_TRUE(missing.rollback_required);
  EXPECT_NE(missing.issues.end(), std::find(missing.issues.begin(), missing.issues.end(), "missing_restore_target"));
}

TEST(SudoVdaControlTest, RestoreVerificationRejectsWrongRefreshAndVirtualDisplayRemaining) {
  auto record = platf::sudovda::snapshot_display_transaction({
    {"physical-device", R"(\\.\DISPLAY1)", "Physical One", true, "edid:ACM-100-1", {1920, 1080, 60}}
  });
  record.virtual_display = {"virtual-device", R"(\\.\DISPLAY9)", "SUNSEAT", true, "sudovda:seat", {1920, 1080, 60}};

  const auto wrong_refresh = platf::sudovda::verify_display_transaction_restored(record, {
    {"physical-device-new", R"(\\.\DISPLAY7)", "Physical One", true, "edid:ACM-100-1", {1920, 1080, 59}}
  });
  EXPECT_FALSE(wrong_refresh.healthy);
  EXPECT_FALSE(wrong_refresh.refresh_restore_healthy);
  EXPECT_TRUE(wrong_refresh.rollback_required);

  const auto virtual_remaining = platf::sudovda::verify_display_transaction_restored(record, {
    {"physical-device-new", R"(\\.\DISPLAY7)", "Physical One", true, "edid:ACM-100-1", {1920, 1080, 60}},
    *record.virtual_display
  });
  EXPECT_FALSE(virtual_remaining.healthy);
  EXPECT_TRUE(virtual_remaining.rollback_required);
  EXPECT_NE(virtual_remaining.issues.end(), std::find(virtual_remaining.issues.begin(), virtual_remaining.issues.end(), "virtual_display_remaining"));
}

TEST(SudoVdaControlTest, TransactionFailureMovesThroughRollbackPhases) {
  auto record = platf::sudovda::snapshot_display_transaction({
    {"physical-device", R"(\\.\DISPLAY1)", "Physical One", true, "edid:ACM-100-1", {1920, 1080, 60}}
  });
  EXPECT_EQ(record.phase, platf::sudovda::display_transaction_phase_e::snapshot);

  platf::sudovda::transition_display_transaction(record, platf::sudovda::display_transaction_phase_e::creating);
  EXPECT_EQ(record.phase, platf::sudovda::display_transaction_phase_e::creating);

  const auto create_failure = platf::sudovda::require_display_transaction_rollback(record, "create_failed");
  EXPECT_EQ(create_failure.phase, platf::sudovda::display_transaction_phase_e::rollback_required);
  EXPECT_TRUE(create_failure.rollback_required);

  const auto rolled_back = platf::sudovda::complete_display_transaction_rollback(record, true);
  EXPECT_EQ(rolled_back.phase, platf::sudovda::display_transaction_phase_e::rolled_back);
  EXPECT_TRUE(rolled_back.healthy);
}

TEST(SudoVdaControlTest, CreateRollbackIsRequiredWhenNewDisplayIsNotCapturable) {
  EXPECT_TRUE(platf::sudovda::requires_create_rollback(true, false));
  EXPECT_FALSE(platf::sudovda::requires_create_rollback(true, true));
  EXPECT_FALSE(platf::sudovda::requires_create_rollback(false, false));
}

TEST(SudoVdaControlTest, StreamTopologyAddsVirtualPairWithoutRemovingExistingTopology) {
  const auto first = platf::sudovda::make_sudovda_stream_topology({}, "virtual-left");
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, (std::vector<std::vector<std::string>> {{"virtual-left"}}));

  const auto pair = platf::sudovda::make_sudovda_stream_topology(*first, "virtual-right");
  ASSERT_TRUE(pair.has_value());
  EXPECT_EQ(*pair, (std::vector<std::vector<std::string>> {
    {"virtual-left"},
    {"virtual-right"}
  }));
}

TEST(SudoVdaControlTest, StreamTopologyExcludesPhysicalAndUnrequestedVirtualDisplays) {
  const auto pair = platf::sudovda::make_sudovda_stream_topology(
    {
      {"physical-primary"},
      {"physical-secondary"},
      {"physical-tertiary"}
    },
    {{"virtual-existing"}},
    std::vector<std::string> {"virtual-left", "virtual-right"}
  );

  ASSERT_TRUE(pair.has_value());
  EXPECT_EQ(*pair, (std::vector<std::vector<std::string>> {
    {"virtual-left"},
    {"virtual-right"}
  }));
}

TEST(SudoVdaControlTest, PairStreamTopologyAddsBothVirtualDevicesInOneTransaction) {
  const auto pair = platf::sudovda::make_sudovda_stream_topology(
    {{"virtual-existing"}},
    std::vector<std::string> {"virtual-left", "virtual-right"}
  );

  ASSERT_TRUE(pair.has_value());
  EXPECT_EQ(*pair, (std::vector<std::vector<std::string>> {
    {"virtual-existing"},
    {"virtual-left"},
    {"virtual-right"}
  }));
}

TEST(SudoVdaControlTest, PairStreamTopologyRejectsDuplicateVirtualDevices) {
  EXPECT_FALSE(platf::sudovda::make_sudovda_stream_topology(
    {},
    std::vector<std::string> {"virtual-left", "virtual-left"}
  ).has_value());
}

TEST(SudoVdaControlTest, StreamTopologyRejectsMalformedVirtualDeviceIdsAndIsIdempotent) {
  const std::vector<std::vector<std::string>> active_virtual_topology {{"virtual-left"}};

  EXPECT_FALSE(platf::sudovda::make_sudovda_stream_topology(active_virtual_topology, "").has_value());
  EXPECT_FALSE(platf::sudovda::make_sudovda_stream_topology({{""}}, "virtual-right").has_value());
  EXPECT_FALSE(platf::sudovda::make_sudovda_stream_topology({{"virtual-left", "virtual-left"}}, "virtual-right").has_value());
  EXPECT_EQ(
    platf::sudovda::make_sudovda_stream_topology(active_virtual_topology, "virtual-left"),
    std::optional<std::vector<std::vector<std::string>>> {active_virtual_topology}
  );
}

TEST(SudoVdaControlTest, TopologyBuildRetriesTransientInventoryFailure) {
  EXPECT_TRUE(platf::display_reliability::should_retry_sudovda_topology_build(false, false, 1));
  EXPECT_FALSE(platf::display_reliability::should_retry_sudovda_topology_build(false, true, 1));
  EXPECT_FALSE(platf::display_reliability::should_retry_sudovda_topology_build(false, false, 5));
  EXPECT_FALSE(platf::display_reliability::should_retry_sudovda_topology_build(true, false, 1));
}

TEST(SudoVdaControlTest, StreamTopologyInventoryRebuildRemapsChurnedDeviceIdsByDisplayName) {
  const std::vector<platf::sudovda::enumerated_display_t> requested {
    {"{c0e1346e-1321-53ff-9262-d6be2e5c863c}", R"(\\.\DISPLAY41)", "Generic Monitor (SUNSEAT)", true, "sudovda:left", {1920, 1080, 60}},
    {"{edf51a21-e4ca-5d01-a63b-3506e1fc9894}", R"(\\.\DISPLAY42)", "Generic Monitor (SUNSEAT)", true, "sudovda:right", {1920, 1080, 60}}
  };
  const std::vector<platf::sudovda::enumerated_display_t> current {
    {"{current-left}", R"(\\.\DISPLAY41)", "Generic Monitor (SUNSEAT)", true, "sudovda:left", {1920, 1080, 60}},
    {"{current-right}", R"(\\.\DISPLAY42)", "Generic Monitor (SUNSEAT)", true, "sudovda:right", {1920, 1080, 60}},
    {"physical-primary", R"(\\.\DISPLAY1)", "Physical Monitor", true, "edid:primary", {1920, 1080, 60}}
  };

  const auto remapped = platf::sudovda::remap_sudovda_stream_inventory(requested, current);

  ASSERT_TRUE(remapped.has_value());
  ASSERT_EQ(remapped->size(), 2U);
  EXPECT_EQ((*remapped)[0].device_id, "{current-left}");
  EXPECT_EQ((*remapped)[1].device_id, "{current-right}");
  EXPECT_NE((*remapped)[0].device_id, requested[0].device_id);
  EXPECT_NE((*remapped)[1].device_id, requested[1].device_id);
}

TEST(SudoVdaControlTest, StreamTopologyInventoryRebuildFailsClosedWhenOneChurnedPaneIsMissing) {
  const std::vector<platf::sudovda::enumerated_display_t> requested {
    {"stale-left", R"(\\.\DISPLAY41)", "Generic Monitor (SUNSEAT)", true, "sudovda:left", {1920, 1080, 60}},
    {"stale-right", R"(\\.\DISPLAY42)", "Generic Monitor (SUNSEAT)", true, "sudovda:right", {1920, 1080, 60}}
  };
  const std::vector<platf::sudovda::enumerated_display_t> current {
    {"current-left", R"(\\.\DISPLAY41)", "Generic Monitor (SUNSEAT)", true, "sudovda:left", {1920, 1080, 60}},
    {"physical-primary", R"(\\.\DISPLAY1)", "Physical Monitor", true, "edid:primary", {1920, 1080, 60}}
  };

  EXPECT_FALSE(platf::sudovda::remap_sudovda_stream_inventory(requested, current).has_value());
}

TEST(SudoVdaControlTest, PairTransactionRetriesOnlyAfterTopologyApplyFailure) {
  EXPECT_TRUE(platf::display_reliability::should_retry_sudovda_pair_transaction(false, true, 1));
  EXPECT_FALSE(platf::display_reliability::should_retry_sudovda_pair_transaction(false, true, 2));
  EXPECT_FALSE(platf::display_reliability::should_retry_sudovda_pair_transaction(false, false, 1));
  EXPECT_FALSE(platf::display_reliability::should_retry_sudovda_pair_transaction(true, true, 1));
}

TEST(SudoVdaControlTest, StreamTopologyVerificationRequiresEveryPhysicalDisplayAndVirtualDisplay) {
  const std::vector<std::vector<std::string>> required {
    {"physical-primary"},
    {"physical-secondary"},
    {"virtual-sunseat"}
  };

  EXPECT_TRUE(platf::sudovda::topology_contains_sudovda_devices(required, required));
  EXPECT_FALSE(platf::sudovda::topology_contains_sudovda_devices(
    {{"physical-primary"}, {"virtual-sunseat"}},
    required
  ));
  EXPECT_FALSE(platf::sudovda::topology_contains_sudovda_devices(
    {{"physical-primary"}, {"physical-secondary"}},
    required
  ));
}

#endif
