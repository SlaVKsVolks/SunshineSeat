/**
 * @file src/platform/windows/dual_display_compositor.h
 * @brief Pure policy for a bounded two-pane D3D11 compositor.
 */
#pragma once

// standard includes
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

namespace platf::dual_display {

  constexpr std::size_t display_pair_size = 2;
  constexpr int pane_width = 1920;
  constexpr int pane_height = 1080;
  constexpr int pane_refresh_rate = 60;
  constexpr int composite_width = pane_width * static_cast<int>(display_pair_size);
  constexpr int composite_height = pane_height;
  constexpr auto minimum_pair_publish_interval = std::chrono::microseconds {
    (1'000'000 + pane_refresh_rate - 1) / pane_refresh_rate
  };
  // The two DXGI duplications are acquired sequentially while the D3D11
  // device lock is held.  Keep the combined wait below one 60 Hz frame so a
  // static pane cannot starve the encoder while the other pane is presenting.
  constexpr auto maximum_pane_capture_wait = std::chrono::milliseconds {8};

  [[nodiscard]] inline std::chrono::milliseconds pane_capture_wait_remaining(
    const std::chrono::steady_clock::time_point deadline,
    const std::chrono::steady_clock::time_point now
  ) {
    if (now >= deadline) {
      return std::chrono::milliseconds::zero();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  }
  // A pane may be reused when its desktop is static.  Age is a diagnostic
  // threshold for the cached frame; topology/access validation remains the
  // hard validity boundary.  Skew is advisory and must not tear down an
  // otherwise valid paired capture.
  constexpr auto maximum_pane_age = std::chrono::milliseconds {250};
  constexpr auto maximum_pane_skew = std::chrono::milliseconds {50};

  [[nodiscard]] bool pair_frame_publish_ready(
    std::optional<std::chrono::steady_clock::time_point> last_publication,
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::duration minimum_interval = minimum_pair_publish_interval
  );

  // DXGI commonly reports a requested 60 Hz mode as 59.94 Hz (for example,
  // 59940/1000).  Compare the same rounded rate used during pair setup rather
  // than requiring one particular rational representation.
  [[nodiscard]] inline bool matches_pane_refresh_rate(
    const std::uint32_t numerator,
    const std::uint32_t denominator
  ) {
    return denominator != 0 &&
      std::lround(
        static_cast<double>(numerator) / static_cast<double>(denominator)
      ) == pane_refresh_rate;
  }

  struct rect_t {
    int x {0};
    int y {0};
    int width {0};
    int height {0};

    friend bool operator==(const rect_t &, const rect_t &) = default;
  };

  struct multi_capture_surface_plan_t {
    rect_t encoder;
    rect_t composite;
  };

  [[nodiscard]] constexpr multi_capture_surface_plan_t multi_capture_surface_plan() {
    return {
      {0, 0, pane_width, pane_height},
      {0, 0, composite_width, composite_height},
    };
  }

  struct horizontal_geometry_t {
    int width {0};
    int height {0};
    std::array<rect_t, display_pair_size> panes;
  };

  struct pane_contract_t {
    std::string_view provider_display_name;
    int width {0};
    int height {0};
    int refresh_rate {0};
    bool sdr {false};
    bool bgra8 {false};
    bool landscape {false};
  };

  struct pane_observation_t {
    // The DXGI name is converted from a temporary wide string during capture
    // initialization. Own it here so validation cannot retain a dangling view.
    std::string output_display_name;
    std::uint64_t adapter_identity {0};
    bool attached_to_desktop {false};
    int width {0};
    int height {0};
    int refresh_rate {0};
    bool sdr {false};
    bool bgra8 {false};
    bool landscape {false};
  };

  enum class pair_validation_e {
    accepted,
    missing_provider_name,
    duplicate_provider_name,
    output_name_mismatch,
    duplicate_output,
    cross_adapter,
    output_detached,
    unsupported_manifest_mode,
    unsupported_output_mode,
    manifest_output_mode_mismatch,
    invalid_geometry,
  };

  struct pair_validation_result_t {
    pair_validation_e state {pair_validation_e::invalid_geometry};
    horizontal_geometry_t geometry;
  };

  [[nodiscard]] bool make_horizontal_geometry(
    int first_width,
    int first_height,
    int second_width,
    int second_height,
    horizontal_geometry_t &geometry
  );

  [[nodiscard]] pair_validation_result_t validate_pair_capture(
    const std::array<pane_contract_t, display_pair_size> &contract,
    const std::array<pane_observation_t, display_pair_size> &observation
  );

  [[nodiscard]] std::string_view pair_validation_name(pair_validation_e state);

  struct pane_cache_t {
    bool has_texture {false};
    // Time at which the cached texture was copied into the compositor cache.
    // This is distinct from the source desktop's present timestamp: DXGI may
    // batch or reuse that timestamp for a frame that was copied successfully.
    std::optional<std::chrono::steady_clock::time_point> captured_at;
    std::optional<std::chrono::steady_clock::time_point> timestamp;
  };

  enum class frame_policy_e {
    ready,
    first_frame_pending,
    missing_timestamp,
    stale_pane,
    skew_exceeded,
    invalid_policy,
  };

  struct frame_policy_result_t {
    frame_policy_e state {frame_policy_e::first_frame_pending};
    std::optional<std::chrono::steady_clock::time_point> timestamp;
    std::array<std::chrono::steady_clock::duration, display_pair_size> ages {};
    std::chrono::steady_clock::duration skew {};
  };

  [[nodiscard]] frame_policy_result_t evaluate_pair_frame(
    const std::array<pane_cache_t, display_pair_size> &cache,
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::duration maximum_age = maximum_pane_age,
    std::chrono::steady_clock::duration maximum_skew = maximum_pane_skew
  );

  [[nodiscard]] std::string_view frame_policy_name(frame_policy_e state);

  template<class Operation>
  void for_each_pane_acquire(Operation &&operation) {
    operation(0);
    operation(1);
  }

  template<class Operation>
  void for_each_pane_release(Operation &&operation) {
    operation(1);
    operation(0);
  }

  enum class capture_route_e {
    ordinary,
    paired_d3d11,
    rejected,
  };

  // A paired virtual-display request owns two exclusive SudoVDA outputs. It
  // must be refused before allocation unless the selected encoder can consume
  // the pair through the D3D11 compositor. Software/DDX remains valid for an
  // ordinary stream, never as a dual-pair fallback.
  enum class prepared_pair_profile_e {
    d3d11_ready,
    rejected_before_allocation,
  };

  [[nodiscard]] prepared_pair_profile_e select_prepared_pair_profile(bool d3d11_memory);

  [[nodiscard]] capture_route_e select_capture_route(
    bool has_prepared_pair,
    bool d3d11_memory,
    bool prepared_pair_valid
  );

}  // namespace platf::dual_display
