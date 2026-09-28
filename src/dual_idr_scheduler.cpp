/**
 * @file src/dual_idr_scheduler.cpp
 * @brief Non-blocking recovery-IDR scheduling for independent video tracks.
 */

// local includes
#include "dual_idr_scheduler.h"

namespace dual_idr {

  scheduler_t::scheduler_t(
    const duration_t stagger,
    const duration_t frame_interval
  ):
      stagger_ {stagger},
      frame_interval_ {frame_interval} {
  }

  std::optional<scheduler_t::duration_t> scheduler_t::frame_interval_for_framerate_x100(
    const int framerate_x100
  ) {
    if (framerate_x100 <= 0) {
      return std::nullopt;
    }

    constexpr auto nanoseconds_per_second_x100 = std::int64_t {100'000'000'000};
    const auto rate = static_cast<std::int64_t>(framerate_x100);
    const auto frame_interval_nanoseconds = (nanoseconds_per_second_x100 + rate - 1) / rate;
    return std::chrono::duration_cast<duration_t>(std::chrono::nanoseconds {frame_interval_nanoseconds});
  }

  std::uint64_t scheduler_t::request_recovery(const time_point now) {
    std::lock_guard lock {mutex_};
    if (active_generation_ != 0) {
      return active_generation_;
    }
    if (recovery_quiet_until_.has_value() && now < *recovery_quiet_until_) {
      return last_completed_generation_;
    }

    ++next_generation_;
    if (next_generation_ == 0) {
      ++next_generation_;
    }

    active_generation_ = next_generation_;
    lead_track_ = next_lead_track_;
    next_lead_track_ = (next_lead_track_ + 1) % track_count;
    for (auto &track : tracks_) {
      track = {};
    }
    tracks_[lead_track_].due_at = now;
    tracks_[lead_track_].deadline_at = now + frame_interval_;
    return active_generation_;
  }

  std::optional<scheduler_t::claim_t> scheduler_t::claim_due_idr(
    const std::size_t track_index,
    const time_point now
  ) {
    std::lock_guard lock {mutex_};
    if (active_generation_ == 0 || track_index >= track_count) {
      return std::nullopt;
    }

    auto &track = tracks_[track_index];
    if (track.completed || track.in_flight || !track.due_at.has_value() || now < *track.due_at) {
      return std::nullopt;
    }

    ++track.token;
    if (track.token == 0) {
      ++track.token;
    }
    track.in_flight = true;
    return claim_t {active_generation_, track_index, track.token, *track.due_at, *track.deadline_at};
  }

  bool scheduler_t::emission_is_within_deadline(const claim_t &claim, const time_point emitted_at) const {
    return emitted_at >= claim.due_at && emitted_at <= claim.deadline_at;
  }

  void scheduler_t::complete_idr(
    const claim_t &claim,
    const bool key_packet_emitted,
    const time_point emitted_at
  ) {
    std::lock_guard lock {mutex_};
    if (claim.generation == 0 || claim.generation != active_generation_ || claim.track_index >= track_count) {
      return;
    }

    auto &track = tracks_[claim.track_index];
    if (!track.in_flight || track.token != claim.token) {
      return;
    }
    track.in_flight = false;
    if (!key_packet_emitted) {
      return;
    }

    track.completed = true;
    if (claim.track_index == lead_track_) {
      auto &follower = tracks_[(lead_track_ + 1) % track_count];
      follower.due_at = emitted_at + stagger_;
      follower.deadline_at = *follower.due_at + frame_interval_;
    }

    if (tracks_[0].completed && tracks_[1].completed) {
      last_completed_generation_ = active_generation_;
      recovery_quiet_until_ = emitted_at + recovery_quiet_period;
      clear_active_generation_locked();
    }
  }

  void scheduler_t::cancel_generation(const std::uint64_t generation) {
    std::lock_guard lock {mutex_};
    if (generation != 0 && generation == active_generation_) {
      clear_active_generation_locked();
      last_completed_generation_ = 0;
      recovery_quiet_until_.reset();
    }
  }

  void scheduler_t::cancel_all() {
    std::lock_guard lock {mutex_};
    clear_active_generation_locked();
    last_completed_generation_ = 0;
    recovery_quiet_until_.reset();
  }

  bool scheduler_t::has_pending_recovery() const {
    std::lock_guard lock {mutex_};
    return active_generation_ != 0;
  }

  void scheduler_t::clear_active_generation_locked() {
    active_generation_ = 0;
    for (auto &track : tracks_) {
      track = {};
    }
  }

}  // namespace dual_idr
