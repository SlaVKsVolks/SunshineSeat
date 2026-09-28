/**
 * @file src/platform/windows/display_reliability.h
 * @brief Pure policy helpers for Windows capture reliability diagnostics.
 */
#pragma once

#include <chrono>
#include <string_view>

namespace platf::display_reliability {
  enum class physical_panel_power_action_e {
    none
  };

  /**
   * SudoVDA streaming never owns physical panel power. Physical paths remain
   * active throughout the lease; teardown only reasserts display topology.
   */
  constexpr physical_panel_power_action_e physical_panel_power_action(
    bool,
    bool,
    bool
  ) {
    return physical_panel_power_action_e::none;
  }

  /**
   * A reconnect allocation must wait for an in-flight intentional release to
   * finish. The release barrier is the lifecycle boundary; callers should not
   * inspect the cached lease or display topology while it is held elsewhere.
   */
  constexpr bool should_defer_sudovda_allocation(bool release_transition_in_progress) {
    return release_transition_in_progress;
  }

  /**
   * libdisplaydevice re-reads the current topology inside setTopology(). Do
   * not enter that call when the preceding probe returned an empty topology;
   * retry after Windows has finished re-enumerating the displays instead.
   */
  constexpr bool should_attempt_sudovda_topology_set(bool current_topology_populated) {
    return current_topology_populated;
  }

  constexpr bool should_report_capture_topology_instability(
    bool sidecar_mode,
    std::string_view display_provider,
    bool has_stream_identity,
    unsigned int width,
    unsigned int height,
    double refresh_hz
  ) {
    const bool sudovda_startup_encoder_validation = display_provider == "sudovda" && !has_stream_identity;
    const bool topology_outside_stream_profile = width != 1920 || height != 1080 || refresh_hz > 61.0;
    return sidecar_mode && !sudovda_startup_encoder_validation && topology_outside_stream_profile;
  }

  /**
   * The encoder has already been validated during sidecar startup. A first
   * stream must not synchronously repeat that expensive physical-desktop
   * validation before SudoVDA has allocated the stream display. Later DXGI
   * invalidation still triggers a normal re-enumeration.
   */
  constexpr bool should_defer_initial_sudovda_encoder_reenumeration(
    bool sidecar_mode,
    std::string_view display_provider,
    bool first_dxgi_observation,
    bool factory_created
  ) {
    return sidecar_mode &&
           display_provider == "sudovda" &&
           first_dxgi_observation &&
           factory_created;
  }

  /**
   * A reconnect cache is an optimization, never a reason to hold up the new
   * stream's control-plane deadline. A freshly created display still receives
   * the normal stabilization budget; a cached display gets a bounded probe and
   * is recreated immediately if it is no longer capturable.
   */
  constexpr std::chrono::milliseconds sudovda_cached_display_probe_timeout() {
    return std::chrono::milliseconds {750};
  }

  /**
   * Windows can transiently expose a newly-created SudoVDA device before DXGI
   * can duplicate it. Retry that complete create transaction once, but never
   * continue after cancellation or after a second failed stabilization.
   */
  constexpr bool should_retry_sudovda_allocation(
    bool stabilized,
    bool aborted,
    unsigned int completed_attempts
  ) {
    return !stabilized && !aborted && completed_attempts < 2;
  }

  /**
   * A newly-created dual pair can be visible to the display inventory in an
   * intermediate shape. Re-read that inventory for the same bounded budget
   * used by topology application, but never retry malformed requested IDs.
   */
  constexpr unsigned int sudovda_topology_build_attempts() {
    return 5;
  }

  constexpr bool should_retry_sudovda_topology_build(
    bool topology_built,
    bool requested_ids_invalid,
    unsigned int completed_attempts
  ) {
    return !topology_built && !requested_ids_invalid &&
           completed_attempts < sudovda_topology_build_attempts();
  }

  /**
   * If the final dual-display topology cannot be applied, pane A may already
   * be backed by a stale Windows path. Restart the bounded pair transaction so
   * both panes are released and recreated together; do not retry only pane B.
   */
  constexpr unsigned int sudovda_pair_transaction_attempts() {
    return 2;
  }

  constexpr bool should_retry_sudovda_pair_transaction(
    bool pair_acquired,
    bool topology_apply_failed,
    unsigned int completed_attempts
  ) {
    return !pair_acquired && topology_apply_failed &&
           completed_attempts < sudovda_pair_transaction_attempts();
  }

  /**
   * Teardown can race Windows display re-enumeration in the same way as
   * creation. Retry one failed release, including deferred baseline restore,
   * but never loop indefinitely from a destructor.
   */
  constexpr bool should_retry_sudovda_release(
    bool released,
    unsigned int completed_attempts
  ) {
    return !released && completed_attempts < 2;
  }

  /**
   * A cached SudoVDA lease should only be recreated when the display-device
   * inventory was successfully populated and no longer contains its device
   * identity. An empty device inventory is also how the Windows API reports a
   * transient enumeration failure, so that case must keep the existing lease
   * for a later capture retry. The capturable-output probe is intentionally not
   * required: a missing cached device can be non-capturable precisely because
   * its output has already disappeared from the desktop topology.
   */
  constexpr bool should_recreate_missing_sudovda_allocation(
    bool has_cached_allocation,
    bool has_cached_device_id,
    bool device_inventory_available,
    bool,
    bool cached_device_present
  ) {
    return has_cached_allocation &&
           has_cached_device_id &&
           device_inventory_available &&
           !cached_device_present;
  }

  /**
   * Legacy display ownership can use a reconnect grace. A stream-managed
   * sidecar SudoVDA allocation must release at the last-session boundary so
   * an abruptly terminated client cannot strand the virtual output.
   */
  constexpr std::chrono::seconds sudovda_reconnect_grace() {
    return std::chrono::seconds {300};
  }

  constexpr bool should_retain_sudovda_display_for_reconnect(
    bool sidecar_mode,
    std::string_view display_provider
  ) {
    return !sidecar_mode || display_provider != "sudovda";
  }
}  // namespace platf::display_reliability
