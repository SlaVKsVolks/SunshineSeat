/**
 * @file src/network_test_provider.cpp
 * @brief Provider-neutral bounded public speed-test policy.
 */

// standard includes
#include <cmath>
#include <limits>
#include <string>
#include <utility>

// local includes
#include "network_test_provider.h"

namespace network_test {

  namespace {

    struct direction_measurement_t {
      std::optional<double> mbps;
      std::optional<std::uint64_t> bytes;
      std::optional<std::uint64_t> duration_ms;
      std::optional<error_t> error;
      bool cancelled {false};
    };

    bool valid_public_stage(const std::string_view stage) {
      return stage == "client_public" || stage == "host_public";
    }

    error_t provider_error(const public_speed_outcome_e outcome, const std::string_view stage) {
      switch (outcome) {
        case public_speed_outcome_e::cancelled:
          return {"provider_cancelled", std::string {stage}, "Public speed provider was cancelled."};
        case public_speed_outcome_e::timeout:
          return {"provider_timeout", std::string {stage}, "Public speed provider exceeded its deadline."};
        case public_speed_outcome_e::transport_failure:
          return {"provider_transport_failure", std::string {stage}, "Public speed provider transport failed."};
        case public_speed_outcome_e::invalid_response:
          return {"provider_invalid_response", std::string {stage}, "Public speed provider response was invalid."};
        case public_speed_outcome_e::succeeded:
          return {"provider_invalid_response", std::string {stage}, "Public speed provider response was invalid."};
      }
      return {"provider_invalid_response", std::string {stage}, "Public speed provider response was invalid."};
    }

    direction_measurement_t run_direction(
      public_speed_provider_t &provider,
      const public_speed_direction_e direction,
      const std::string_view stage,
      const cancel_observer_t &cancel_requested
    ) {
      direction_measurement_t measurement;
      std::uint64_t total_bytes = 0;
      for (const auto bytes : default_limits().transfer_ramp_bytes) {
        if (cancel_requested && cancel_requested()) {
          measurement.cancelled = true;
          measurement.error = provider_error(public_speed_outcome_e::cancelled, stage);
          return measurement;
        }
        if (total_bytes > default_limits().direction_byte_cap - bytes) {
          measurement.error = {"provider_byte_cap", std::string {stage}, "Public speed provider exceeded the byte cap."};
          return measurement;
        }

        const public_speed_request_t request {
          direction,
          bytes,
          default_limits().latency_timeout,
          default_limits().direction_timeout,
        };
        const auto sample = provider.measure(request, cancel_requested);
        if (sample.outcome != public_speed_outcome_e::succeeded) {
          measurement.cancelled = sample.outcome == public_speed_outcome_e::cancelled;
          measurement.error = provider_error(sample.outcome, stage);
          return measurement;
        }
        if (sample.transferred_bytes != bytes || sample.elapsed <= std::chrono::milliseconds::zero() || sample.elapsed > std::chrono::duration_cast<std::chrono::milliseconds>(default_limits().direction_timeout)) {
          measurement.error = provider_error(public_speed_outcome_e::invalid_response, stage);
          return measurement;
        }

        total_bytes += sample.transferred_bytes;
        const auto seconds = static_cast<double>(sample.elapsed.count()) / 1000.0;
        const auto megabits = static_cast<double>(sample.transferred_bytes) * 8.0 / 1'000'000.0;
        const auto mbps = megabits / seconds;
        if (!std::isfinite(mbps) || mbps < 0.0) {
          measurement.error = provider_error(public_speed_outcome_e::invalid_response, stage);
          return measurement;
        }
        measurement.mbps = mbps;
        measurement.bytes = sample.transferred_bytes;
        measurement.duration_ms = static_cast<std::uint64_t>(sample.elapsed.count());
        if (sample.elapsed >= std::chrono::seconds {2}) {
          return measurement;
        }
      }
      return measurement;
    }

  }  // namespace

  public_speed_profile_t run_public_speed_profile(
    public_speed_provider_t &provider,
    const std::string_view stage,
    const cancel_observer_t &cancel_requested
  ) {
    if (!valid_public_stage(stage)) {
      return {
        status_e::failed,
        std::nullopt,
        error_t {"invalid_public_stage", "host_public", "Public speed stage is invalid."},
      };
    }
    if (cancel_requested && cancel_requested()) {
      return {
        status_e::cancelled,
        std::nullopt,
        provider_error(public_speed_outcome_e::cancelled, stage),
      };
    }

    const auto download = run_direction(provider, public_speed_direction_e::download, stage, cancel_requested);
    if (download.cancelled) {
      return {
        status_e::cancelled,
        std::nullopt,
        download.error,
      };
    }
    const auto upload = run_direction(provider, public_speed_direction_e::upload, stage, cancel_requested);
    if (upload.cancelled) {
      return {
        status_e::cancelled,
        throughput_t {download.mbps, std::nullopt, download.bytes, std::nullopt, download.duration_ms, std::nullopt},
        upload.error,
      };
    }

    if (!download.mbps && !upload.mbps) {
      return {
        status_e::failed,
        std::nullopt,
        download.error ? download.error : upload.error,
      };
    }

    throughput_t throughput {
      download.mbps,
      upload.mbps,
      download.bytes,
      upload.bytes,
      download.duration_ms,
      upload.duration_ms,
    };
    if (download.error || upload.error) {
      return {
        status_e::partial,
        throughput,
        download.error ? download.error : upload.error,
      };
    }
    return {status_e::succeeded, throughput, std::nullopt};
  }

}  // namespace network_test
