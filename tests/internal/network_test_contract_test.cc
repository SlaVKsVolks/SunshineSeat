#include "src/network_test.h"

#include <cstdlib>
#include <iostream>
#include <nlohmann/json.hpp>

namespace {

  using namespace network_test;

  [[noreturn]] void fail(const char *message) {
    std::cerr << "network-test-contract: FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }

  result_t route_result() {
    result_t result;
    result.schema_version = current_schema_version;
    result.test_id = "7180c1e7-64be-4df0-9fa5-058c4e5e42f8";
    result.kind = result_kind_e::route;
    result.actor = actor_e::host;
    result.endpoint_label = "paired_host";
    result.started_at_utc = "2026-08-24T16:00:00Z";
    result.completed_at_utc = "2026-08-24T16:00:08Z";
    result.status = status_e::succeeded;
    result.latency = latency_t {8, 8.5, 13.0, 1.25};
    result.throughput = throughput_t {
      120.0,
      std::nullopt,
      33ULL * 1024ULL * 1024ULL,
      std::nullopt,
      2'200,
      std::nullopt
    };
    result.recommendation = recommendation_t {84, 0.70, "route_download"};
    return result;
  }

}  // namespace

int main() {
  using namespace network_test;

  const auto &limits = default_limits();
  if (limits.latency_request_count != 8 || limits.minimum_latency_samples != 5 || limits.latency_timeout != std::chrono::seconds {2} || limits.direction_byte_cap != 64ULL * 1024ULL * 1024ULL || limits.direction_timeout != std::chrono::seconds {10} || limits.workflow_timeout != std::chrono::seconds {45} || limits.transfer_ramp_bytes != std::array<std::uint64_t, 5> {256ULL * 1024ULL, 1ULL * 1024ULL * 1024ULL, 4ULL * 1024ULL * 1024ULL, 16ULL * 1024ULL * 1024ULL, 32ULL * 1024ULL * 1024ULL}) {
    fail("the bounded v1 measurement profile does not match the approved limits");
  }

  const recommendation_input_t local_input {
    false,
    120.0,
    40.0,
    50.0
  };
  const auto local_recommendation = make_bitrate_recommendation(local_input);
  if (!local_recommendation.has_value() || local_recommendation->safe_aggregate_video_mbps != 84 || local_recommendation->limiting_stage != "route_download") {
    fail("a local route recommendation used public diagnostics or omitted route headroom");
  }

  const recommendation_input_t remote_input {
    true,
    120.0,
    80.0,
    90.0
  };
  const auto remote_recommendation = make_bitrate_recommendation(remote_input);
  if (!remote_recommendation.has_value() || remote_recommendation->safe_aggregate_video_mbps != 56 || remote_recommendation->limiting_stage != "host_public_upload") {
    fail("a remote route recommendation did not use the limiting public leg");
  }
  if (make_bitrate_recommendation({true, std::nullopt, 80.0, 90.0}).has_value()) {
    fail("a recommendation was created without a successful route download");
  }

  const auto expected = route_result();
  if (validate_result(expected).has_value()) {
    fail("a valid route result was rejected");
  }

  auto remote_result = expected;
  remote_result.recommendation = remote_recommendation;
  if (validate_result(remote_result).has_value()) {
    fail("a valid remote recommendation was rejected by the result contract");
  }

  auto invalid_kind = expected;
  invalid_kind.kind = static_cast<result_kind_e>(99);
  if (validate_result(invalid_kind) != std::optional<std::string> {"invalid_kind"}) {
    fail("an invalid result kind was not rejected before serialization");
  }

  const auto encoded = serialize_result(expected);
  const auto encoded_json = nlohmann::json::parse(encoded);
  if (encoded_json.contains("passive_stream") || encoded_json.contains("error") || encoded_json.at("throughput").contains("upload_mbps") || encoded_json.at("throughput").at("download_bytes") != 33ULL * 1024ULL * 1024ULL) {
    fail("serialization invented absent measurements or did not preserve completed route data");
  }

  std::string failure;
  const auto decoded = parse_result(encoded, failure);
  if (!decoded.has_value() || decoded->kind != result_kind_e::route || !decoded->throughput.has_value() || decoded->throughput->download_mbps != std::optional<double> {120.0} || decoded->throughput->upload_mbps.has_value()) {
    fail("a valid result did not round-trip through the v1 parser");
  }

  auto malformed = encoded_json;
  malformed["raw_ip"] = "198.51.100.10";
  if (parse_result(malformed.dump(), failure).has_value() || failure != "unknown_result_field") {
    fail("the persisted/wire contract accepted an unapproved privacy-sensitive field");
  }
  malformed = encoded_json;
  malformed["schema_version"] = current_schema_version + 1;
  if (parse_result(malformed.dump(), failure).has_value() || failure != "unsupported_schema_version") {
    fail("the parser accepted an unsupported result schema");
  }
  malformed = encoded_json;
  malformed["throughput"]["download_bytes"] = limits.direction_byte_cap + 1;
  if (parse_result(malformed.dump(), failure).has_value() || failure != "download_bytes_exceeds_cap") {
    fail("the parser accepted an over-cap transfer record");
  }

  std::cout << "network-test-contract: PASS\n";
  return EXIT_SUCCESS;
}
