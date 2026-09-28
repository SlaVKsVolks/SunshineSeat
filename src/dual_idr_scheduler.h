/**
 * @file src/dual_idr_scheduler.h
 * @brief Non-blocking recovery-IDR scheduling for independent video tracks.
 */
#pragma once

// standard includes
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace dual_idr {

  inline constexpr auto recovery_stagger = std::chrono::milliseconds {40};
  // A completed paired recovery already refreshes both decoder references. Keep
  // delayed duplicate requests from converting a single recovery incident into
  // repeated paired keyframe bursts.
  inline constexpr auto recovery_quiet_period = std::chrono::seconds {6};

  class scheduler_t {
  public:
    using clock_t = std::chrono::steady_clock;
    using time_point = clock_t::time_point;
    using duration_t = clock_t::duration;

    struct claim_t {
      std::uint64_t generation {0};
      std::size_t track_index {0};
      std::uint64_t token {0};
      time_point due_at {};
      time_point deadline_at {};
    };

    static constexpr std::size_t track_count = 2;

    scheduler_t(duration_t stagger, duration_t frame_interval);

    [[nodiscard]] static std::optional<duration_t> frame_interval_for_framerate_x100(int framerate_x100);

    [[nodiscard]] std::uint64_t request_recovery(time_point now);
    [[nodiscard]] std::optional<claim_t> claim_due_idr(std::size_t track_index, time_point now);
    [[nodiscard]] bool emission_is_within_deadline(const claim_t &claim, time_point emitted_at) const;
    void complete_idr(const claim_t &claim, bool key_packet_emitted, time_point emitted_at);
    void cancel_generation(std::uint64_t generation);
    void cancel_all();
    [[nodiscard]] bool has_pending_recovery() const;

  private:
    struct track_state_t {
      std::optional<time_point> due_at;
      std::optional<time_point> deadline_at;
      std::uint64_t token {0};
      bool completed {false};
      bool in_flight {false};
    };

    void clear_active_generation_locked();

    mutable std::mutex mutex_;
    std::array<track_state_t, track_count> tracks_;
    duration_t stagger_;
    duration_t frame_interval_;
    std::uint64_t active_generation_ {0};
    std::uint64_t next_generation_ {0};
    std::uint64_t last_completed_generation_ {0};
    std::optional<time_point> recovery_quiet_until_;
    std::size_t lead_track_ {0};
    std::size_t next_lead_track_ {0};
  };

}  // namespace dual_idr
