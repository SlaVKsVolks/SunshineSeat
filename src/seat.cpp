/**
 * @file src/seat.cpp
 * @brief Runtime seat profile definitions for SunshineSeat.
 */

// standard includes
#include <sstream>

// local includes
#include "config.h"
#include "logging.h"
#include "seat.h"

namespace seat {
  profile_t active_profile() {
    return {
      config::seat.id,
      config::seat.target_user,
      config::seat.target_session_id,
      config::seat.role,
      config::seat.display_provider,
      config::seat.input_policy,
      config::seat.audio_policy,
    };
  }

  std::string describe(const profile_t &profile) {
    std::stringstream stream;
    stream << "id=" << profile.id
           << ", role=" << profile.role
           << ", target_user=" << (profile.target_user.empty() ? "<current>" : profile.target_user)
           << ", target_session_id=" << (profile.target_session_id < 0 ? "<auto>" : std::to_string(profile.target_session_id))
           << ", display_provider=" << profile.display_provider
           << ", input_policy=" << profile.input_policy
           << ", audio_policy=" << profile.audio_policy;

    return stream.str();
  }

  void log_startup_profile() {
    BOOST_LOG(info) << "SunshineSeat profile: " << describe(active_profile());
  }
}  // namespace seat
