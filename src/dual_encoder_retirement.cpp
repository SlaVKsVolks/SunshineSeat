/**
 * @file src/dual_encoder_retirement.cpp
 * @brief Definitions for bounded dual encoder generation retirement.
 */

// local includes
#include "dual_encoder_retirement.h"

namespace dual_encoder {

  retirement_t::retirement_t(const duration_t stall_timeout, const duration_t recovery_grace):
      stall_timeout_ {stall_timeout},
      recovery_grace_ {recovery_grace} {}

  std::optional<retirement_t::claim_t> retirement_t::begin_send(const std::size_t track_index, const time_point now) {
    std::lock_guard lock {mutex_};
    if (track_index >= track_count || control_state_ != control_state_e::healthy || tracks_[track_index].in_flight) {
      return std::nullopt;
    }

    auto &track = tracks_[track_index];
    ++track.generation;
    if (track.generation == 0) {
      ++track.generation;
    }
    track.started_at = now;
    track.in_flight = true;
    return claim_t {track.generation, track_index};
  }

  void retirement_t::finish_send(const claim_t &claim) {
    std::lock_guard lock {mutex_};
    if (claim.track_index >= track_count) {
      return;
    }

    auto &track = tracks_[claim.track_index];
    if (track.in_flight && track.generation == claim.generation) {
      track.in_flight = false;
    }
  }

  retirement_t::decision_t retirement_t::poll(const time_point now) {
    std::lock_guard lock {mutex_};

    if (control_state_ == control_state_e::process_recovery_required) {
      return {};
    }

    const auto in_flight_track_count = [this]() {
      std::size_t count = 0;
      for (const auto &track : tracks_) {
        count += track.in_flight ? 1 : 0;
      }
      return count;
    };

    if (control_state_ == control_state_e::recovery_requested) {
      if (in_flight_track_count() == 0 || now < retirement_started_at_ || now - retirement_started_at_ < recovery_grace_) {
        return {};
      }

      control_state_ = control_state_e::process_recovery_required;
      return {action_e::force_process_recovery, in_flight_track_count()};
    }

    std::size_t stalled_track_count = 0;
    for (const auto &track : tracks_) {
      if (track.in_flight && now >= track.started_at && now - track.started_at >= stall_timeout_) {
        ++stalled_track_count;
      }
    }
    if (stalled_track_count == 0) {
      return {};
    }

    control_state_ = control_state_e::recovery_requested;
    retirement_started_at_ = now;
    return {action_e::retire_generation, stalled_track_count};
  }

  bool retirement_t::is_retired() const {
    std::lock_guard lock {mutex_};
    return control_state_ != control_state_e::healthy;
  }

  control_state_e retirement_t::control_state() const {
    std::lock_guard lock {mutex_};
    return control_state_;
  }

  bool retirement_t::accepts_control_ping() const {
    std::lock_guard lock {mutex_};
    return control_state_ != control_state_e::process_recovery_required;
  }

}  // namespace dual_encoder
