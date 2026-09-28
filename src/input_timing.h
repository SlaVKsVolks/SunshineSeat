/**
 * @file src/input_timing.h
 * @brief Privacy-bounded correlation for optional paired input diagnostics.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

namespace input_timing {

  constexpr std::uint8_t current_schema_version = 1;
  constexpr std::int64_t mouse_motion_sample_interval_us = 100'000;
  constexpr std::int64_t maximum_one_way_uncertainty_us = 1'000;
  constexpr std::size_t maximum_pending_markers = 128;

  enum class event_family_e: std::uint8_t {
    mouse_motion = 1,
    mouse_button = 2,
    keyboard = 3,
  };

  struct marker_t {
    std::uint8_t schema_version;
    event_family_e family;
    std::uint64_t sequence;
    std::int64_t client_capture_us;
    std::int64_t client_send_us;
  };

  bool validate_marker(const marker_t &marker);
  bool marker_contains_input_content(const marker_t &marker);
  std::optional<marker_t> decode_wire_marker(
    std::uint8_t schema_version,
    std::uint8_t family,
    std::uint16_t reserved,
    std::uint64_t sequence,
    std::uint64_t client_capture_us,
    std::uint64_t client_send_us
  );

  class sampler_t {
  public:
    bool should_sample(event_family_e family, std::int64_t now_us);

  private:
    std::optional<std::int64_t> last_mouse_motion_us_;
  };

  struct clock_exchange_t {
    std::int64_t host_send_us;
    std::int64_t client_receive_us;
    std::int64_t client_send_us;
    std::int64_t host_receive_us;
  };

  struct clock_estimate_t {
    std::int64_t offset_us;
    std::int64_t uncertainty_us;
  };

  std::optional<clock_estimate_t> estimate_clock(const std::vector<clock_exchange_t> &exchanges);
  std::optional<std::int64_t> one_way_delay_us(std::int64_t client_send_us, std::int64_t host_receive_us, const clock_estimate_t &clock);

  struct associated_input_t {
    std::uint64_t sequence;
    marker_t marker;
    std::int64_t host_received_us;
    std::int64_t host_attached_us;
  };

  struct completed_input_t {
    associated_input_t associated;
    std::int64_t host_injected_us;
  };

  class tracker_t {
  public:
    void accept_marker(const marker_t &marker, std::int64_t host_received_us);
    bool has_pending_marker_for(event_family_e family) const;
    std::optional<associated_input_t> attach_next_input(event_family_e family, std::int64_t host_attached_us);
    std::optional<completed_input_t> complete_injection(std::uint64_t sequence, bool injected, std::int64_t host_injected_us);
    void discard_injection(std::uint64_t sequence);

  private:
    struct pending_marker_t {
      marker_t marker;
      std::int64_t host_received_us;
    };

    std::deque<pending_marker_t> pending_markers_;
    std::unordered_map<std::uint64_t, associated_input_t> associated_inputs_;
  };

}  // namespace input_timing
