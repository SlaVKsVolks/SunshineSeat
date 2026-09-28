/**
 * @file src/input_timing.cpp
 * @brief Deterministic primitives for the optional input-timing-v1 protocol.
 */
#include "input_timing.h"

#include <algorithm>
#include <limits>

namespace input_timing {

  namespace {

    bool is_supported_family(event_family_e family) {
      return family == event_family_e::mouse_motion || family == event_family_e::mouse_button || family == event_family_e::keyboard;
    }

    bool marker_matches_input(event_family_e marker_family, event_family_e input_family) {
      if (marker_family == event_family_e::keyboard) {
        return input_family == event_family_e::keyboard;
      }
      return input_family == event_family_e::mouse_motion || input_family == event_family_e::mouse_button;
    }

  }  // namespace

  bool validate_marker(const marker_t &marker) {
    return marker.schema_version == current_schema_version && is_supported_family(marker.family) && marker.sequence != 0 && marker.client_capture_us > 0 && marker.client_send_us >= marker.client_capture_us;
  }

  bool marker_contains_input_content(const marker_t &) {
    return false;
  }

  std::optional<marker_t> decode_wire_marker(
    const std::uint8_t schema_version,
    const std::uint8_t family,
    const std::uint16_t reserved,
    const std::uint64_t sequence,
    const std::uint64_t client_capture_us,
    const std::uint64_t client_send_us
  ) {
    if (reserved != 0 || client_capture_us > std::numeric_limits<std::int64_t>::max() || client_send_us > std::numeric_limits<std::int64_t>::max()) {
      return std::nullopt;
    }

    std::optional<event_family_e> decoded_family;
    if (family == 1) {
      decoded_family = event_family_e::mouse_motion;
    } else if (family == 2) {
      decoded_family = event_family_e::keyboard;
    } else {
      return std::nullopt;
    }

    const marker_t marker {
      schema_version,
      *decoded_family,
      sequence,
      static_cast<std::int64_t>(client_capture_us),
      static_cast<std::int64_t>(client_send_us),
    };
    return validate_marker(marker) ? std::optional<marker_t> {marker} : std::nullopt;
  }

  bool sampler_t::should_sample(event_family_e family, std::int64_t now_us) {
    if (!is_supported_family(family) || now_us < 0) {
      return false;
    }
    if (family != event_family_e::mouse_motion) {
      return true;
    }
    if (!last_mouse_motion_us_.has_value() || now_us - *last_mouse_motion_us_ >= mouse_motion_sample_interval_us) {
      last_mouse_motion_us_ = now_us;
      return true;
    }
    return false;
  }

  std::optional<clock_estimate_t> estimate_clock(const std::vector<clock_exchange_t> &exchanges) {
    std::optional<clock_estimate_t> best;
    for (const auto &exchange: exchanges) {
      if (exchange.client_receive_us < exchange.host_send_us || exchange.client_send_us < exchange.client_receive_us || exchange.host_receive_us < exchange.client_send_us) {
        continue;
      }
      const auto round_trip_us = (exchange.host_receive_us - exchange.host_send_us) - (exchange.client_send_us - exchange.client_receive_us);
      if (round_trip_us < 0) {
        continue;
      }
      const auto offset_us = ((exchange.client_receive_us - exchange.host_send_us) + (exchange.client_send_us - exchange.host_receive_us)) / 2;
      const clock_estimate_t candidate {offset_us, round_trip_us / 2};
      if (!best.has_value() || candidate.uncertainty_us < best->uncertainty_us) {
        best = candidate;
      }
    }
    return best;
  }

  std::optional<std::int64_t> one_way_delay_us(std::int64_t client_send_us, std::int64_t host_receive_us, const clock_estimate_t &clock) {
    if (client_send_us <= 0 || host_receive_us < client_send_us || clock.uncertainty_us < 0 || clock.uncertainty_us > maximum_one_way_uncertainty_us) {
      return std::nullopt;
    }
    const auto delay_us = host_receive_us - client_send_us - clock.offset_us;
    return delay_us >= 0 ? std::optional<std::int64_t> {delay_us} : std::nullopt;
  }

  void tracker_t::accept_marker(const marker_t &marker, std::int64_t host_received_us) {
    pending_markers_.erase(std::remove_if(pending_markers_.begin(), pending_markers_.end(), [host_received_us](const pending_marker_t &candidate) {
      return host_received_us >= candidate.host_received_us && host_received_us - candidate.host_received_us > 2'000'000;
    }), pending_markers_.end());

    if (!validate_marker(marker) || host_received_us <= 0 || pending_markers_.size() >= maximum_pending_markers) {
      return;
    }
    pending_markers_.push_back(pending_marker_t {marker, host_received_us});
  }

  bool tracker_t::has_pending_marker_for(const event_family_e family) const {
    return std::any_of(pending_markers_.begin(), pending_markers_.end(), [family](const pending_marker_t &candidate) {
      return marker_matches_input(candidate.marker.family, family);
    });
  }

  std::optional<associated_input_t> tracker_t::attach_next_input(event_family_e family, std::int64_t host_attached_us) {
    pending_markers_.erase(std::remove_if(pending_markers_.begin(), pending_markers_.end(), [host_attached_us](const pending_marker_t &candidate) {
      return host_attached_us >= candidate.host_received_us && host_attached_us - candidate.host_received_us > 2'000'000;
    }), pending_markers_.end());

    const auto pending = std::find_if(pending_markers_.begin(), pending_markers_.end(), [family](const pending_marker_t &candidate) {
      return marker_matches_input(candidate.marker.family, family);
    });
    if (pending == pending_markers_.end() || host_attached_us < pending->host_received_us) {
      return std::nullopt;
    }
    associated_input_t associated {
      pending->marker.sequence,
      pending->marker,
      pending->host_received_us,
      host_attached_us
    };
    pending_markers_.erase(pending);
    if (associated_inputs_.size() >= maximum_pending_markers) {
      return std::nullopt;
    }
    associated_inputs_.insert_or_assign(associated.sequence, associated);
    return associated;
  }

  std::optional<completed_input_t> tracker_t::complete_injection(std::uint64_t sequence, bool injected, std::int64_t host_injected_us) {
    const auto it = associated_inputs_.find(sequence);
    if (it == associated_inputs_.end() || !injected || host_injected_us < it->second.host_attached_us) {
      return std::nullopt;
    }
    completed_input_t completed {it->second, host_injected_us};
    associated_inputs_.erase(it);
    return completed;
  }

  void tracker_t::discard_injection(const std::uint64_t sequence) {
    associated_inputs_.erase(sequence);
  }

}  // namespace input_timing
