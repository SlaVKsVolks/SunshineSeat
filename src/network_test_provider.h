/**
 * @file src/network_test_provider.h
 * @brief Provider-neutral public speed-test policy and transport seam.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

// local includes
#include "network_test.h"

namespace network_test {

  enum class public_speed_direction_e {
    download,
    upload,
  };

  enum class public_speed_outcome_e {
    succeeded,
    cancelled,
    timeout,
    transport_failure,
    invalid_response,
  };

  using cancel_observer_t = std::function<bool()>;

  struct public_speed_request_t {
    public_speed_direction_e direction;
    std::uint64_t bytes;
    std::chrono::seconds connect_timeout;
    std::chrono::seconds direction_timeout;
  };

  struct public_speed_sample_t {
    public_speed_outcome_e outcome;
    std::uint64_t transferred_bytes {0};
    std::chrono::milliseconds elapsed {0};
  };

  class public_speed_provider_t {
  public:
    virtual ~public_speed_provider_t() = default;

    virtual public_speed_sample_t measure(
      const public_speed_request_t &request,
      const cancel_observer_t &cancel_requested
    ) = 0;
  };

  /**
   * The production adapter is deliberately endpoint-fixed: callers choose a
   * direction and bounded byte count, never an arbitrary URL.
   */
  class curl_public_speed_provider_t final: public public_speed_provider_t {
  public:
    public_speed_sample_t measure(
      const public_speed_request_t &request,
      const cancel_observer_t &cancel_requested
    ) override;
  };

  struct public_speed_profile_t {
    status_e status {status_e::failed};
    std::optional<throughput_t> throughput;
    std::optional<error_t> error;
  };

  /**
   * Runs the approved bounded ramp without choosing a provider endpoint. The
   * supplied provider must account only payload bytes and transfer time.
   */
  [[nodiscard]] public_speed_profile_t run_public_speed_profile(
    public_speed_provider_t &provider,
    std::string_view stage,
    const cancel_observer_t &cancel_requested
  );

}  // namespace network_test
