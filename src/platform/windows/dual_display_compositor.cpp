/**
 * @file src/platform/windows/dual_display_compositor.cpp
 * @brief Definitions for the bounded two-pane D3D11 compositor policy.
 */

// local includes
#include "dual_display_compositor.h"

namespace platf::dual_display {

  namespace {

    bool is_v1_mode(const pane_contract_t &pane) {
      return pane.width == pane_width && pane.height == pane_height &&
             pane.refresh_rate == pane_refresh_rate && pane.sdr && pane.bgra8 && pane.landscape;
    }

    bool is_v1_mode(const pane_observation_t &pane) {
      return pane.width == pane_width && pane.height == pane_height &&
             pane.refresh_rate == pane_refresh_rate && pane.sdr && pane.bgra8 && pane.landscape;
    }

    bool modes_match(const pane_contract_t &contract, const pane_observation_t &observation) {
      return contract.width == observation.width && contract.height == observation.height &&
             contract.refresh_rate == observation.refresh_rate && contract.sdr == observation.sdr &&
             contract.bgra8 == observation.bgra8 && contract.landscape == observation.landscape;
    }

    std::chrono::steady_clock::duration age_at(
      const std::chrono::steady_clock::time_point now,
      const std::chrono::steady_clock::time_point timestamp
    ) {
      return now >= timestamp ? now - timestamp : std::chrono::steady_clock::duration::zero();
    }

    std::chrono::steady_clock::duration absolute_difference(
      const std::chrono::steady_clock::time_point first,
      const std::chrono::steady_clock::time_point second
    ) {
      return first >= second ? first - second : second - first;
    }

  }  // namespace

  bool pair_frame_publish_ready(
    const std::optional<std::chrono::steady_clock::time_point> last_publication,
    const std::chrono::steady_clock::time_point now,
    const std::chrono::steady_clock::duration minimum_interval
  ) {
    return minimum_interval > std::chrono::steady_clock::duration::zero() &&
           (!last_publication ||
            (now > *last_publication && now - *last_publication >= minimum_interval));
  }

  bool make_horizontal_geometry(
    const int first_width,
    const int first_height,
    const int second_width,
    const int second_height,
    horizontal_geometry_t &geometry
  ) {
    if (first_width != pane_width || first_height != pane_height ||
        second_width != pane_width || second_height != pane_height) {
      return false;
    }

    geometry = horizontal_geometry_t {
      composite_width,
      composite_height,
      {
        rect_t {0, 0, pane_width, pane_height},
        rect_t {pane_width, 0, pane_width, pane_height}
      }
    };
    return true;
  }

  pair_validation_result_t validate_pair_capture(
    const std::array<pane_contract_t, display_pair_size> &contract,
    const std::array<pane_observation_t, display_pair_size> &observation
  ) {
    pair_validation_result_t result;
    if (contract[0].provider_display_name.empty() || contract[1].provider_display_name.empty()) {
      result.state = pair_validation_e::missing_provider_name;
      return result;
    }
    if (contract[0].provider_display_name == contract[1].provider_display_name) {
      result.state = pair_validation_e::duplicate_provider_name;
      return result;
    }
    if (!is_v1_mode(contract[0]) || !is_v1_mode(contract[1])) {
      result.state = pair_validation_e::unsupported_manifest_mode;
      return result;
    }

    if (observation[0].output_display_name != contract[0].provider_display_name ||
        observation[1].output_display_name != contract[1].provider_display_name) {
      result.state = pair_validation_e::output_name_mismatch;
      return result;
    }
    if (observation[0].output_display_name == observation[1].output_display_name) {
      result.state = pair_validation_e::duplicate_output;
      return result;
    }
    if (observation[0].adapter_identity != observation[1].adapter_identity) {
      result.state = pair_validation_e::cross_adapter;
      return result;
    }
    if (!observation[0].attached_to_desktop || !observation[1].attached_to_desktop) {
      result.state = pair_validation_e::output_detached;
      return result;
    }
    if (!observation[0].sdr || !observation[1].sdr ||
        !observation[0].bgra8 || !observation[1].bgra8 ||
        !observation[0].landscape || !observation[1].landscape) {
      result.state = pair_validation_e::unsupported_output_mode;
      return result;
    }
    if (!modes_match(contract[0], observation[0]) || !modes_match(contract[1], observation[1])) {
      result.state = pair_validation_e::manifest_output_mode_mismatch;
      return result;
    }
    if (!is_v1_mode(observation[0]) || !is_v1_mode(observation[1])) {
      result.state = pair_validation_e::unsupported_output_mode;
      return result;
    }
    if (!make_horizontal_geometry(
          observation[0].width,
          observation[0].height,
          observation[1].width,
          observation[1].height,
          result.geometry
        )) {
      result.state = pair_validation_e::invalid_geometry;
      return result;
    }

    result.state = pair_validation_e::accepted;
    return result;
  }

  std::string_view pair_validation_name(const pair_validation_e state) {
    switch (state) {
      case pair_validation_e::accepted: return "accepted";
      case pair_validation_e::missing_provider_name: return "missing_provider_name";
      case pair_validation_e::duplicate_provider_name: return "duplicate_provider_name";
      case pair_validation_e::output_name_mismatch: return "output_name_mismatch";
      case pair_validation_e::duplicate_output: return "duplicate_output";
      case pair_validation_e::cross_adapter: return "cross_adapter";
      case pair_validation_e::output_detached: return "output_detached";
      case pair_validation_e::unsupported_manifest_mode: return "unsupported_manifest_mode";
      case pair_validation_e::unsupported_output_mode: return "unsupported_output_mode";
      case pair_validation_e::manifest_output_mode_mismatch: return "manifest_output_mode_mismatch";
      case pair_validation_e::invalid_geometry: return "invalid_geometry";
    }
    return "unknown";
  }

  frame_policy_result_t evaluate_pair_frame(
    const std::array<pane_cache_t, display_pair_size> &cache,
    const std::chrono::steady_clock::time_point now,
    const std::chrono::steady_clock::duration maximum_age,
    const std::chrono::steady_clock::duration maximum_skew
  ) {
    frame_policy_result_t result;
    if (maximum_age <= std::chrono::steady_clock::duration::zero() ||
        maximum_skew <= std::chrono::steady_clock::duration::zero()) {
      result.state = frame_policy_e::invalid_policy;
      return result;
    }
    if (!cache[0].has_texture || !cache[1].has_texture) {
      result.state = frame_policy_e::first_frame_pending;
      return result;
    }
    if (!cache[0].captured_at || !cache[1].captured_at ||
        !cache[0].timestamp || !cache[1].timestamp) {
      result.state = frame_policy_e::missing_timestamp;
      return result;
    }

    result.ages[0] = age_at(now, *cache[0].captured_at);
    result.ages[1] = age_at(now, *cache[1].captured_at);
    result.skew = absolute_difference(*cache[0].timestamp, *cache[1].timestamp);
    result.timestamp = *cache[0].timestamp >= *cache[1].timestamp
      ? *cache[0].timestamp
      : *cache[1].timestamp;
    if (result.ages[0] > maximum_age || result.ages[1] > maximum_age) {
      // The composite texture is assembled now even when DXGI reuses cached
      // pixels for static desktops.  Tagging it with an old pane-present time
      // makes the generic video watchdog mistake valid static content for a
      // stalled capture and repeatedly recreate capture and the encoder.
      result.timestamp = now;
      result.state = frame_policy_e::stale_pane;
      return result;
    }

    if (result.skew > maximum_skew) {
      // A static pane can legitimately reuse a recent cached frame while the
      // other pane continues presenting.  Keep the pair alive as long as
      // both panes satisfy maximum_age; the capture layer logs this state as
      // advisory instead of reinitializing the encoder.
      result.state = frame_policy_e::skew_exceeded;
      return result;
    }

    result.state = frame_policy_e::ready;
    return result;
  }

  std::string_view frame_policy_name(const frame_policy_e state) {
    switch (state) {
      case frame_policy_e::ready: return "ready";
      case frame_policy_e::first_frame_pending: return "first_frame_pending";
      case frame_policy_e::missing_timestamp: return "missing_timestamp";
      case frame_policy_e::stale_pane: return "stale_pane";
      case frame_policy_e::skew_exceeded: return "skew_exceeded";
      case frame_policy_e::invalid_policy: return "invalid_policy";
    }
    return "unknown";
  }

  prepared_pair_profile_e select_prepared_pair_profile(const bool d3d11_memory) {
    return d3d11_memory
      ? prepared_pair_profile_e::d3d11_ready
      : prepared_pair_profile_e::rejected_before_allocation;
  }

  capture_route_e select_capture_route(
    const bool has_prepared_pair,
    const bool d3d11_memory,
    const bool prepared_pair_valid
  ) {
    if (!has_prepared_pair) {
      return capture_route_e::ordinary;
    }
    return d3d11_memory && prepared_pair_valid
      ? capture_route_e::paired_d3d11
      : capture_route_e::rejected;
  }

}  // namespace platf::dual_display
