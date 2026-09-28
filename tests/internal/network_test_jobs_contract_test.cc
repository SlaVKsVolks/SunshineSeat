#include "src/network_test_jobs.h"
#include "src/network_test_provider.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace {

  using namespace network_test;

  [[noreturn]] void fail(const char *message) {
    std::cerr << "network-test-jobs-contract: FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }

  class fake_clock_t final: public diagnostic_clock_t {
  public:
    std::chrono::steady_clock::time_point steady_now() const override {
      return steady;
    }

    std::chrono::system_clock::time_point system_now() const override {
      return system;
    }

    void advance(const std::chrono::seconds duration) {
      steady += duration;
      system += duration;
    }

    std::chrono::steady_clock::time_point steady {};
    std::chrono::system_clock::time_point system {};
  };

  class successful_public_provider_t final: public public_speed_provider_t {
  public:
    public_speed_sample_t measure(const public_speed_request_t &request, const cancel_observer_t &) override {
      return {
        .outcome = public_speed_outcome_e::succeeded,
        .transferred_bytes = request.bytes,
        .elapsed = std::chrono::seconds {2},
      };
    }
  };

  result_t client_result() {
    result_t result;
    result.schema_version = current_schema_version;
    result.test_id = "9f7be7f1-59e0-4900-a643-1aefac106dd0";
    result.kind = result_kind_e::client_public;
    result.actor = actor_e::client;
    result.endpoint_label = "cloudflare_edge";
    result.started_at_utc = "2026-08-24T17:00:00Z";
    result.completed_at_utc = "2026-08-24T17:00:08Z";
    result.status = status_e::succeeded;
    result.throughput = throughput_t {
      120.0,
      std::nullopt,
      32ULL * 1024ULL * 1024ULL,
      std::nullopt,
      2'000,
      std::nullopt,
    };
    return result;
  }

}  // namespace

int main() {
  using namespace network_test;

  const auto state_file = std::filesystem::temp_directory_path() / "sunshine-network-test-jobs-contract.json";
  std::error_code remove_error;
  std::filesystem::remove(state_file, remove_error);

  auto clock = std::make_shared<fake_clock_t>();
  stream_state_e stream_state = stream_state_e::unknown;
  diagnostic_jobs_t jobs {
    state_file,
    clock,
    [&] {
      return stream_state;
    },
  };

  auto created = jobs.create_route_job("paired-client-a");
  if (created.http_status != 409 || created.error_code != "stream_active") {
    fail("unknown stream state did not fail closed");
  }

  stream_state = stream_state_e::idle;
  created = jobs.create_route_job("paired-client-a");
  if (created.http_status != 201 || !created.job.has_value() || created.job->job_id.empty() || created.job->nonce.empty()) {
    fail("an idle paired client could not create a bounded route job");
  }
  const auto route_job = *created.job;

  if (jobs.create_route_job("paired-client-a").http_status != 409) {
    fail("a second active route job for the same paired client was accepted");
  }
  if (jobs.read_download("paired-client-b", route_job.job_id, route_job.nonce, 256ULL * 1024ULL).http_status != 403) {
    fail("a route job crossed paired-client ownership");
  }
  if (jobs.read_download("paired-client-a", route_job.job_id, route_job.nonce, default_limits().direction_byte_cap + 1).http_status != 413) {
    fail("an over-cap download was accepted");
  }

  const auto first_download = jobs.read_download("paired-client-a", route_job.job_id, route_job.nonce, 256ULL * 1024ULL);
  const auto second_download = jobs.read_download("paired-client-a", route_job.job_id, route_job.nonce, 256ULL * 1024ULL);
  if (first_download.http_status != 200 || !first_download.payload.has_value() || first_download.payload->size() != 256ULL * 1024ULL || first_download.payload != second_download.payload) {
    fail("route downloads were not deterministic bounded payloads");
  }
  if (jobs.ping_route_job("paired-client-a", route_job.job_id, route_job.nonce).http_status != 204) {
    fail("a zero-byte latency probe was not authorized by the route nonce");
  }
  if (jobs.consume_upload("paired-client-a", route_job.job_id, route_job.nonce, std::vector<std::uint8_t>(256ULL * 1024ULL)).http_status != 204) {
    fail("a bounded route upload was not consumed without persistence");
  }
  if (jobs.cancel_route_job("paired-client-a", route_job.job_id, route_job.nonce).http_status != 204 || jobs.read_download("paired-client-a", route_job.job_id, route_job.nonce, 1).http_status != 410) {
    fail("route cancellation did not invalidate the nonce and release the job");
  }

  for (std::size_t attempt = 0; attempt < 6; ++attempt) {
    const auto rate_limited_job = jobs.create_route_job("paired-client-rate-limited");
    if (rate_limited_job.http_status != 201 || !rate_limited_job.job.has_value() || jobs.cancel_route_job("paired-client-rate-limited", rate_limited_job.job->job_id, rate_limited_job.job->nonce).http_status != 204) {
      fail("a bounded route-job attempt could not be started and cancelled");
    }
  }
  if (jobs.create_route_job("paired-client-rate-limited").http_status != 429) {
    fail("excessive route-job attempts were not rate limited");
  }

  const auto interrupted_job = jobs.create_route_job("paired-client-interrupted");
  if (interrupted_job.http_status != 201 || !interrupted_job.job.has_value()) {
    fail("a route job could not be established before the stream-start interruption");
  }
  stream_state = stream_state_e::active;
  if (jobs.read_download("paired-client-interrupted", interrupted_job.job->job_id, interrupted_job.job->nonce, 1).http_status != 409) {
    fail("a stream start did not cancel in-flight route throughput immediately");
  }
  stream_state = stream_state_e::idle;
  const auto released_job = jobs.create_route_job("paired-client-interrupted");
  if (released_job.http_status != 201 || !released_job.job.has_value() || jobs.cancel_route_job("paired-client-interrupted", released_job.job->job_id, released_job.job->nonce).http_status != 204) {
    fail("a stream-start cancellation did not release the paired-client job slot");
  }

  const auto host_job = jobs.create_host_public_job();
  if (host_job.http_status != 202 || !host_job.job.has_value() || jobs.create_host_public_job().http_status != 409) {
    fail("host-public jobs were not globally exclusive");
  }
  if (jobs.cancel_host_public_job(host_job.job->job_id).http_status != 204) {
    fail("host-public cancellation did not release the global job slot");
  }

  const auto measured_host_job = jobs.create_host_public_job();
  successful_public_provider_t public_provider;
  if (measured_host_job.http_status != 202 || !measured_host_job.job.has_value() || jobs.run_host_public_job(measured_host_job.job->job_id, public_provider).http_status != 204 || !jobs.host_public_job_status(measured_host_job.job->job_id).result.has_value() || !jobs.latest_results("paired-client-a").host_public_result.has_value() || jobs.create_host_public_job().http_status != 202) {
    fail("a completed host-public provider job was not safely persisted and released");
  }

  const auto result = client_result();
  if (jobs.submit_client_result("paired-client-a", result).http_status != 204) {
    fail("a valid paired-client result was not persisted");
  }
  diagnostic_jobs_t reloaded {
    state_file,
    clock,
    [&] {
      return stream_state;
    },
  };
  if (!reloaded.latest_results("paired-client-a").client_result.has_value()) {
    fail("latest paired-client results did not survive an atomic reload");
  }

  clock->advance(std::chrono::hours {24 * 31});
  reloaded.prune_expired_results();
  if (reloaded.latest_results("paired-client-a").client_result.has_value()) {
    fail("expired diagnostic records were retained beyond thirty days");
  }

  std::filesystem::remove(state_file, remove_error);
  std::cout << "network-test-jobs-contract: PASS\n";
  return EXIT_SUCCESS;
}
