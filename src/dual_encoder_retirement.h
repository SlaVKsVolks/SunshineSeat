/**
 * @file src/dual_encoder_retirement.h
 * @brief Bounded retirement policy for a dual AVCodec encoder generation.
 */
#pragma once

// standard includes
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace dual_encoder {

  enum class action_e {
    none,
    retire_generation,
    force_process_recovery,
  };

  enum class control_state_e {
    healthy,
    recovery_requested,
    process_recovery_required,
  };

  /**
   * Tracks in-flight calls into a dual hardware encoder without attempting to
   * cancel its driver call. Once a call is overdue, no replacement call is
   * admitted; a separate supervisor can then recover the process before a
   * stuck generation blocks session shutdown indefinitely.
   */
  class retirement_t {
  public:
    using clock_t = std::chrono::steady_clock;
    using time_point = clock_t::time_point;
    using duration_t = clock_t::duration;

    static constexpr std::size_t track_count = 2;

    struct claim_t {
      std::uint64_t generation {0};
      std::size_t track_index {0};
    };

    struct decision_t {
      action_e action {action_e::none};
      std::size_t stalled_track_count {0};
    };

    retirement_t(duration_t stall_timeout, duration_t recovery_grace);

    [[nodiscard]] std::optional<claim_t> begin_send(std::size_t track_index, time_point now);
    void finish_send(const claim_t &claim);
    [[nodiscard]] decision_t poll(time_point now);

    [[nodiscard]] bool is_retired() const;
    [[nodiscard]] control_state_e control_state() const;
    [[nodiscard]] bool accepts_control_ping() const;

  private:
    struct track_state_t {
      std::uint64_t generation {0};
      time_point started_at {};
      bool in_flight {false};
    };

    mutable std::mutex mutex_;
    std::array<track_state_t, track_count> tracks_;
    duration_t stall_timeout_;
    duration_t recovery_grace_;
    time_point retirement_started_at_ {};
    control_state_e control_state_ {control_state_e::healthy};
  };

}  // namespace dual_encoder
