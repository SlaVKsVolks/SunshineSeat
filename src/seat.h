/**
 * @file src/seat.h
 * @brief Runtime seat profile declarations for SunshineSeat.
 */
#pragma once

// standard includes
#include <string>

namespace seat {
  struct profile_t {
    std::string id;
    std::string target_user;
    int target_session_id;
    std::string role;
    std::string display_provider;
    std::string input_policy;
    std::string audio_policy;
  };

  profile_t active_profile();
  std::string describe(const profile_t &profile);
  void log_startup_profile();
}  // namespace seat
