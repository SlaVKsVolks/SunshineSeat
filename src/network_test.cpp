/**
 * @file src/network_test.cpp
 * @brief Versioned, privacy-safe network diagnostics data and policy contract.
 */

// standard includes
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <utility>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "network_test.h"

namespace network_test {

  namespace {

    using json = nlohmann::json;

    constexpr auto mib = std::uint64_t {1024} * 1024;
    constexpr auto milliseconds_per_second = std::uint64_t {1000};

    bool finite_nonnegative(const double value) {
      return std::isfinite(value) && value >= 0.0;
    }

    bool safe_identifier(const std::string_view value) {
      if (value.empty() || value.size() > 128) {
        return false;
      }

      return std::all_of(value.begin(), value.end(), [](const char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-' || character == '_';
      });
    }

    bool safe_message(const std::string_view value) {
      if (value.empty() || value.size() > 256) {
        return false;
      }

      return std::all_of(value.begin(), value.end(), [](const char character) {
        return character >= 0x20 && character != 0x7f;
      });
    }

    bool valid_timestamp(const std::string_view value) {
      return value.size() >= 20 && value.size() <= 64 && value.find('T') != std::string_view::npos &&
             value.ends_with('Z') && safe_message(value);
    }

    bool has_only_keys(const json &value, const std::initializer_list<std::string_view> permitted) {
      if (!value.is_object()) {
        return false;
      }

      for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
        if (std::find(permitted.begin(), permitted.end(), iterator.key()) == permitted.end()) {
          return false;
        }
      }

      return true;
    }

    std::optional<result_kind_e> parse_kind(const std::string_view value) {
      if (value == "route") {
        return result_kind_e::route;
      }
      if (value == "client_public") {
        return result_kind_e::client_public;
      }
      if (value == "host_public") {
        return result_kind_e::host_public;
      }
      if (value == "passive_stream") {
        return result_kind_e::passive_stream;
      }
      return std::nullopt;
    }

    std::string_view serialize_kind(const result_kind_e value) {
      switch (value) {
        case result_kind_e::route:
          return "route";
        case result_kind_e::client_public:
          return "client_public";
        case result_kind_e::host_public:
          return "host_public";
        case result_kind_e::passive_stream:
          return "passive_stream";
      }
      return "";
    }

    std::optional<actor_e> parse_actor(const std::string_view value) {
      if (value == "client") {
        return actor_e::client;
      }
      if (value == "host") {
        return actor_e::host;
      }
      return std::nullopt;
    }

    std::string_view serialize_actor(const actor_e value) {
      switch (value) {
        case actor_e::client:
          return "client";
        case actor_e::host:
          return "host";
      }
      return "";
    }

    std::optional<status_e> parse_status(const std::string_view value) {
      if (value == "succeeded") {
        return status_e::succeeded;
      }
      if (value == "partial") {
        return status_e::partial;
      }
      if (value == "cancelled") {
        return status_e::cancelled;
      }
      if (value == "failed") {
        return status_e::failed;
      }
      if (value == "blocked_stream_active") {
        return status_e::blocked_stream_active;
      }
      return std::nullopt;
    }

    std::string_view serialize_status(const status_e value) {
      switch (value) {
        case status_e::succeeded:
          return "succeeded";
        case status_e::partial:
          return "partial";
        case status_e::cancelled:
          return "cancelled";
        case status_e::failed:
          return "failed";
        case status_e::blocked_stream_active:
          return "blocked_stream_active";
      }
      return "";
    }

    bool valid_endpoint_label(const std::string_view value) {
      return value == "paired_host" || value == "cloudflare_edge" || value == "stream_telemetry";
    }

    bool valid_stage(const std::string_view value) {
      return value == "capabilities" || value == "ports" || value == "route_latency" ||
             value == "route_download" || value == "route_upload" || value == "client_public" ||
             value == "host_public" || value == "passive_stream";
    }

    bool valid_limiting_stage(const std::string_view value) {
      return value == "route_download" || value == "host_public_upload" || value == "client_public_download";
    }

    std::optional<std::string> validate_latency(const latency_t &latency) {
      const auto &limits = default_limits();
      if (latency.sample_count < limits.minimum_latency_samples || latency.sample_count > limits.latency_request_count) {
        return "invalid_latency_sample_count";
      }
      if (!finite_nonnegative(latency.median_ms) || !finite_nonnegative(latency.p95_ms) || !finite_nonnegative(latency.jitter_ms) || latency.p95_ms < latency.median_ms) {
        return "invalid_latency_measurement";
      }
      return std::nullopt;
    }

    std::optional<std::string> validate_direction(
      const std::optional<double> &mbps,
      const std::optional<std::uint64_t> &bytes,
      const std::optional<std::uint64_t> &duration_ms,
      const std::string_view name
    ) {
      const bool any_present = mbps.has_value() || bytes.has_value() || duration_ms.has_value();
      if (!any_present) {
        return std::nullopt;
      }
      if (!mbps.has_value() || !bytes.has_value() || !duration_ms.has_value() || !finite_nonnegative(*mbps)) {
        return std::string {name} + "_measurement_incomplete";
      }
      if (*bytes > default_limits().direction_byte_cap) {
        return std::string {name} + "_bytes_exceeds_cap";
      }
      if (*duration_ms > static_cast<std::uint64_t>(default_limits().direction_timeout.count()) * milliseconds_per_second) {
        return std::string {name} + "_duration_exceeds_cap";
      }
      return std::nullopt;
    }

    std::optional<std::string> validate_throughput(const throughput_t &throughput) {
      if (const auto failure = validate_direction(throughput.download_mbps, throughput.download_bytes, throughput.download_duration_ms, "download")) {
        return failure;
      }
      if (const auto failure = validate_direction(throughput.upload_mbps, throughput.upload_bytes, throughput.upload_duration_ms, "upload")) {
        return failure;
      }
      if (!throughput.download_mbps.has_value() && !throughput.upload_mbps.has_value()) {
        return "missing_throughput_measurement";
      }
      return std::nullopt;
    }

    std::optional<std::string> validate_passive_stream(const passive_stream_t &passive) {
      if (!finite_nonnegative(passive.network_dropped_percent) || passive.network_dropped_percent > 100.0 || !finite_nonnegative(passive.rtt_ms) || !finite_nonnegative(passive.rendered_fps) || !finite_nonnegative(passive.host_processing_avg_ms)) {
        return "invalid_passive_stream_measurement";
      }
      return std::nullopt;
    }

    std::optional<std::string> validate_recommendation(const recommendation_t &recommendation) {
      if (recommendation.safe_aggregate_video_mbps < 0 || std::abs(recommendation.headroom_ratio - 0.70) > std::numeric_limits<double>::epsilon() || !valid_limiting_stage(recommendation.limiting_stage)) {
        return "invalid_recommendation";
      }
      return std::nullopt;
    }

    std::optional<std::string> validate_error(const error_t &error) {
      if (!safe_identifier(error.code) || !valid_stage(error.stage) || !safe_message(error.message)) {
        return "invalid_error";
      }
      return std::nullopt;
    }

    template<typename T>
    std::optional<T> optional_field(const json &value, const std::string_view key) {
      if (!value.contains(key)) {
        return std::nullopt;
      }
      return value.at(key).get<T>();
    }

    json serialize_throughput(const throughput_t &throughput) {
      json encoded = json::object();
      if (throughput.download_mbps) {
        encoded["download_mbps"] = *throughput.download_mbps;
      }
      if (throughput.upload_mbps) {
        encoded["upload_mbps"] = *throughput.upload_mbps;
      }
      if (throughput.download_bytes) {
        encoded["download_bytes"] = *throughput.download_bytes;
      }
      if (throughput.upload_bytes) {
        encoded["upload_bytes"] = *throughput.upload_bytes;
      }
      if (throughput.download_duration_ms) {
        encoded["download_duration_ms"] = *throughput.download_duration_ms;
      }
      if (throughput.upload_duration_ms) {
        encoded["upload_duration_ms"] = *throughput.upload_duration_ms;
      }
      return encoded;
    }

  }  // namespace

  const limits_t &default_limits() {
    static constexpr limits_t limits {
      8,
      5,
      std::chrono::seconds {2},
      {256ULL * 1024ULL, mib, 4ULL * mib, 16ULL * mib, 32ULL * mib},
      64ULL * mib,
      std::chrono::seconds {10},
      std::chrono::seconds {45},
      std::chrono::hours {24 * 30},
    };
    return limits;
  }

  std::optional<recommendation_t> make_bitrate_recommendation(const recommendation_input_t &input) {
    if (!input.route_download_mbps.has_value() || !finite_nonnegative(*input.route_download_mbps) || *input.route_download_mbps <= 0.0) {
      return std::nullopt;
    }

    std::vector<std::pair<double, std::string_view>> candidates;
    candidates.emplace_back(*input.route_download_mbps, "route_download");
    if (input.route_is_remote) {
      if (input.host_public_upload_mbps.has_value() && finite_nonnegative(*input.host_public_upload_mbps) && *input.host_public_upload_mbps > 0.0) {
        candidates.emplace_back(*input.host_public_upload_mbps, "host_public_upload");
      }
      if (input.client_public_download_mbps.has_value() && finite_nonnegative(*input.client_public_download_mbps) && *input.client_public_download_mbps > 0.0) {
        candidates.emplace_back(*input.client_public_download_mbps, "client_public_download");
      }
    }

    const auto limiting = std::min_element(candidates.begin(), candidates.end(), [](const auto &left, const auto &right) {
      return left.first < right.first;
    });
    return recommendation_t {
      static_cast<int>(std::floor(limiting->first * 0.70)),
      0.70,
      std::string {limiting->second}
    };
  }

  std::optional<std::string> validate_result(const result_t &result) {
    if (result.schema_version != current_schema_version) {
      return "unsupported_schema_version";
    }
    if (serialize_kind(result.kind).empty()) {
      return "invalid_kind";
    }
    if (serialize_actor(result.actor).empty()) {
      return "invalid_actor";
    }
    if (serialize_status(result.status).empty()) {
      return "invalid_status";
    }
    if (!safe_identifier(result.test_id)) {
      return "invalid_test_id";
    }
    if (!valid_endpoint_label(result.endpoint_label)) {
      return "invalid_endpoint_label";
    }
    if (!valid_timestamp(result.started_at_utc) || !valid_timestamp(result.completed_at_utc)) {
      return "invalid_timestamp";
    }
    if (result.latency) {
      if (const auto failure = validate_latency(*result.latency)) {
        return failure;
      }
    }
    if (result.throughput) {
      if (const auto failure = validate_throughput(*result.throughput)) {
        return failure;
      }
    }
    if (result.passive_stream) {
      if (const auto failure = validate_passive_stream(*result.passive_stream)) {
        return failure;
      }
    }
    if (result.recommendation) {
      if (const auto failure = validate_recommendation(*result.recommendation)) {
        return failure;
      }
    }
    if (result.error) {
      if (const auto failure = validate_error(*result.error)) {
        return failure;
      }
    }
    return std::nullopt;
  }

  std::string serialize_result(const result_t &result) {
    json encoded {
      {"schema_version", result.schema_version},
      {"test_id", result.test_id},
      {"kind", serialize_kind(result.kind)},
      {"actor", serialize_actor(result.actor)},
      {"endpoint_label", result.endpoint_label},
      {"started_at_utc", result.started_at_utc},
      {"completed_at_utc", result.completed_at_utc},
      {"status", serialize_status(result.status)},
    };
    if (result.latency) {
      encoded["latency"] = {
        {"sample_count", result.latency->sample_count},
        {"median_ms", result.latency->median_ms},
        {"p95_ms", result.latency->p95_ms},
        {"jitter_ms", result.latency->jitter_ms},
      };
    }
    if (result.throughput) {
      encoded["throughput"] = serialize_throughput(*result.throughput);
    }
    if (result.passive_stream) {
      encoded["passive_stream"] = {
        {"sample_age_ms", result.passive_stream->sample_age_ms},
        {"network_dropped_percent", result.passive_stream->network_dropped_percent},
        {"rtt_ms", result.passive_stream->rtt_ms},
        {"rendered_fps", result.passive_stream->rendered_fps},
        {"decoder_errors", result.passive_stream->decoder_errors},
        {"host_processing_avg_ms", result.passive_stream->host_processing_avg_ms},
        {"sender_slow_events", result.passive_stream->sender_slow_events},
        {"recovery_idr_events", result.passive_stream->recovery_idr_events},
      };
    }
    if (result.recommendation) {
      encoded["recommendation"] = {
        {"safe_aggregate_video_mbps", result.recommendation->safe_aggregate_video_mbps},
        {"headroom_ratio", result.recommendation->headroom_ratio},
        {"limiting_stage", result.recommendation->limiting_stage},
      };
    }
    if (result.error) {
      encoded["error"] = {
        {"code", result.error->code},
        {"stage", result.error->stage},
        {"message", result.error->message},
      };
    }
    return encoded.dump();
  }

  std::optional<result_t> parse_result(const std::string_view encoded, std::string &failure) {
    failure.clear();
    try {
      const auto value = json::parse(encoded);
      if (!has_only_keys(
            value,
            {
              "schema_version",
              "test_id",
              "kind",
              "actor",
              "endpoint_label",
              "started_at_utc",
              "completed_at_utc",
              "status",
              "latency",
              "throughput",
              "passive_stream",
              "recommendation",
              "error",
            }
          )) {
        failure = "unknown_result_field";
        return std::nullopt;
      }

      result_t result;
      result.schema_version = value.at("schema_version").get<int>();
      if (result.schema_version != current_schema_version) {
        failure = "unsupported_schema_version";
        return std::nullopt;
      }
      result.test_id = value.at("test_id").get<std::string>();
      const auto kind = parse_kind(value.at("kind").get<std::string>());
      const auto actor = parse_actor(value.at("actor").get<std::string>());
      const auto status = parse_status(value.at("status").get<std::string>());
      if (!kind || !actor || !status) {
        failure = "invalid_result";
        return std::nullopt;
      }
      result.kind = *kind;
      result.actor = *actor;
      result.endpoint_label = value.at("endpoint_label").get<std::string>();
      result.started_at_utc = value.at("started_at_utc").get<std::string>();
      result.completed_at_utc = value.at("completed_at_utc").get<std::string>();
      result.status = *status;

      if (value.contains("latency")) {
        const auto &latency = value.at("latency");
        if (!has_only_keys(latency, {"sample_count", "median_ms", "p95_ms", "jitter_ms"})) {
          failure = "unknown_latency_field";
          return std::nullopt;
        }
        result.latency = latency_t {
          latency.at("sample_count").get<std::uint32_t>(),
          latency.at("median_ms").get<double>(),
          latency.at("p95_ms").get<double>(),
          latency.at("jitter_ms").get<double>(),
        };
      }

      if (value.contains("throughput")) {
        const auto &throughput = value.at("throughput");
        if (!has_only_keys(
              throughput,
              {
                "download_mbps",
                "upload_mbps",
                "download_bytes",
                "upload_bytes",
                "download_duration_ms",
                "upload_duration_ms",
              }
            )) {
          failure = "unknown_throughput_field";
          return std::nullopt;
        }
        result.throughput = throughput_t {
          optional_field<double>(throughput, "download_mbps"),
          optional_field<double>(throughput, "upload_mbps"),
          optional_field<std::uint64_t>(throughput, "download_bytes"),
          optional_field<std::uint64_t>(throughput, "upload_bytes"),
          optional_field<std::uint64_t>(throughput, "download_duration_ms"),
          optional_field<std::uint64_t>(throughput, "upload_duration_ms"),
        };
      }

      if (value.contains("passive_stream")) {
        const auto &passive = value.at("passive_stream");
        if (!has_only_keys(
              passive,
              {
                "sample_age_ms",
                "network_dropped_percent",
                "rtt_ms",
                "rendered_fps",
                "decoder_errors",
                "host_processing_avg_ms",
                "sender_slow_events",
                "recovery_idr_events",
              }
            )) {
          failure = "unknown_passive_stream_field";
          return std::nullopt;
        }
        result.passive_stream = passive_stream_t {
          passive.at("sample_age_ms").get<std::uint64_t>(),
          passive.at("network_dropped_percent").get<double>(),
          passive.at("rtt_ms").get<double>(),
          passive.at("rendered_fps").get<double>(),
          passive.at("decoder_errors").get<std::uint64_t>(),
          passive.at("host_processing_avg_ms").get<double>(),
          passive.at("sender_slow_events").get<std::uint64_t>(),
          passive.at("recovery_idr_events").get<std::uint64_t>(),
        };
      }

      if (value.contains("recommendation")) {
        const auto &recommendation = value.at("recommendation");
        if (!has_only_keys(recommendation, {"safe_aggregate_video_mbps", "headroom_ratio", "limiting_stage"})) {
          failure = "unknown_recommendation_field";
          return std::nullopt;
        }
        result.recommendation = recommendation_t {
          recommendation.at("safe_aggregate_video_mbps").get<int>(),
          recommendation.at("headroom_ratio").get<double>(),
          recommendation.at("limiting_stage").get<std::string>(),
        };
      }

      if (value.contains("error")) {
        const auto &error = value.at("error");
        if (!has_only_keys(error, {"code", "stage", "message"})) {
          failure = "unknown_error_field";
          return std::nullopt;
        }
        result.error = error_t {
          error.at("code").get<std::string>(),
          error.at("stage").get<std::string>(),
          error.at("message").get<std::string>(),
        };
      }

      if (const auto validation_failure = validate_result(result)) {
        failure = *validation_failure;
        return std::nullopt;
      }
      return result;
    } catch (const std::exception &) {
      failure = "invalid_result";
      return std::nullopt;
    }
  }

}  // namespace network_test
