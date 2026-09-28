/**
 * @file src/dual_encoder_initialization_gate.h
 * @brief Serialize hardware encoder initialization for dual-display streams.
 */
#pragma once

#include <mutex>

namespace dual_encoder {

  class initialization_gate_t {
  public:
    using lock_t = std::unique_lock<std::mutex>;

    [[nodiscard]] lock_t acquire() {
      return lock_t {mutex_};
    }

  private:
    std::mutex mutex_;
  };

}  // namespace dual_encoder
