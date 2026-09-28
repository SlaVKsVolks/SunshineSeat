/**
 * @file src/video_frame_deadline.h
 * @brief Minimum video cadence without adding encode time to each interval.
 */
#pragma once

#include <chrono>

namespace video {
  inline std::chrono::steady_clock::time_point advance_minimum_frame_deadline(
    std::chrono::steady_clock::time_point deadline,
    std::chrono::steady_clock::time_point frame_started,
    std::chrono::steady_clock::duration interval,
    bool fresh_frame
  ) {
    // Early capture/IDR work starts a new cadence. Late fresh frames keep
    // the existing phase so convert time and wakeup jitter cannot accumulate.
    // A long pause rebases instead of sending a burst of old duplicate frames.
    if ((fresh_frame && frame_started < deadline) || frame_started >= deadline + interval) {
      return frame_started + interval;
    }
    // Preserve the phase across ordinary wakeup jitter and encode work.
    return deadline + interval;
  }
}  // namespace video
