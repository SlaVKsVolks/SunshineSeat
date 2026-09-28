/**
 * @file src/network_test.h
 * @brief Versioned, privacy-safe network diagnostics data and policy contract.
 */
#pragma once

// standard includes
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace network_test {

  inline constexpr int current_schema_version = 1;

  enum class result_kind_e {
    route,
    client_public,
    host_public,
    passive_stream,
  };

  enum class actor_e {
    client,
    host,
  };

  enum class status_e {
    succeeded,
    partial,
    cancelled,
    failed,
    blocked_stream_active,
  };

  struct limits_t {
    std::uint32_t latency_request_count;
    std::uint32_t minimum_latency_samples;
    std::chrono::seconds latency_timeout;
    std::array<std::uint64_t, 5> transfer_ramp_bytes;
    std::uint64_t direction_byte_cap;
    std::chrono::seconds direction_timeout;
    std::chrono::seconds workflow_timeout;
    std::chrono::hours retention_period;
  };

  struct latency_t {
    std::uint32_t sample_count {0};
    double median_ms {0.0};
    double p95_ms {0.0};
    double jitter_ms {0.0};
  };

  struct throughput_t {
    std::optional<double> download_mbps;
    std::optional<double> upload_mbps;
    std::optional<std::uint64_t> download_bytes;
    std::optional<std::uint64_t> upload_bytes;
    std::optional<std::uint64_t> download_duration_ms;
    std::optional<std::uint64_t> upload_duration_ms;
  };

  struct passive_stream_t {
    std::uint64_t sample_age_ms {0};
    double network_dropped_percent {0.0};
    double rtt_ms {0.0};
    double rendered_fps {0.0};
    std::uint64_t decoder_errors {0};
    double host_processing_avg_ms {0.0};
    std::uint64_t sender_slow_events {0};
    std::uint64_t recovery_idr_events {0};
  };

  struct recommendation_t {
    int safe_aggregate_video_mbps {0};
    double headroom_ratio {0.70};
    std::string limiting_stage;
  };

  struct error_t {
    std::string code;
    std::string stage;
    std::string message;
  };

  struct result_t {
    int schema_version {current_schema_version};
    std::string test_id;
    result_kind_e kind {result_kind_e::route};
    actor_e actor {actor_e::host};
    std::string endpoint_label;
    std::string started_at_utc;
    std::string completed_at_utc;
    status_e status {status_e::failed};
    std::optional<latency_t> latency;
    std::optional<throughput_t> throughput;
    std::optional<passive_stream_t> passive_stream;
    std::optional<recommendation_t> recommendation;
    std::optional<error_t> error;
  };

  struct recommendation_input_t {
    bool route_is_remote {false};
    std::optional<double> route_download_mbps;
    std::optional<double> host_public_upload_mbps;
    std::optional<double> client_public_download_mbps;
  };

  [[nodiscard]] const limits_t &default_limits();
  [[nodiscard]] std::optional<recommendation_t> make_bitrate_recommendation(const recommendation_input_t &input);
  [[nodiscard]] std::optional<std::string> validate_result(const result_t &result);
  [[nodiscard]] std::string serialize_result(const result_t &result);
  [[nodiscard]] std::optional<result_t> parse_result(std::string_view encoded, std::string &failure);

}  // namespace network_test
