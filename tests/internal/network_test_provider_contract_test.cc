#include "src/network_test_provider.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

  using namespace network_test;

  [[noreturn]] void fail(const char *message) {
    std::cerr << "network-test-provider-contract: FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }

  class fake_provider_t final: public public_speed_provider_t {
  public:
    public_speed_sample_t measure(const public_speed_request_t &request, const cancel_observer_t &) override {
      requests.push_back(request);
      if (responses.empty()) {
        return {
          .outcome = public_speed_outcome_e::transport_failure,
          .transferred_bytes = 0,
          .elapsed = std::chrono::milliseconds {0},
        };
      }
      auto response = responses.front();
      responses.erase(responses.begin());
      if (response.transferred_bytes == 0 && response.outcome == public_speed_outcome_e::succeeded) {
        response.transferred_bytes = request.bytes;
      }
      return response;
    }

    std::vector<public_speed_request_t> requests;
    std::vector<public_speed_sample_t> responses;
  };

}  // namespace

int main() {
  using namespace network_test;

  fake_provider_t success_provider;
  success_provider.responses = {
    {.outcome = public_speed_outcome_e::succeeded, .elapsed = std::chrono::milliseconds {2'500}},
    {.outcome = public_speed_outcome_e::succeeded, .elapsed = std::chrono::milliseconds {2'000}},
  };
  const auto success = run_public_speed_profile(success_provider, "host_public", [] {
    return false;
  });
  if (success.status != status_e::succeeded || !success.throughput.has_value() || !success.throughput->download_mbps.has_value() || !success.throughput->upload_mbps.has_value() || success.error.has_value() || success_provider.requests.size() != 2 || success_provider.requests[0].direction != public_speed_direction_e::download || success_provider.requests[1].direction != public_speed_direction_e::upload || success_provider.requests[0].bytes != 256ULL * 1024ULL || success_provider.requests[0].connect_timeout != std::chrono::seconds {2} || success_provider.requests[0].direction_timeout != std::chrono::seconds {10}) {
    fail("the provider profile did not use the approved bounded first samples");
  }
  if (std::abs(*success.throughput->download_mbps - 0.8388608) > 0.0001 || std::abs(*success.throughput->upload_mbps - 1.048576) > 0.0001) {
    fail("throughput did not use measured payload bytes over transfer duration");
  }

  fake_provider_t partial_provider;
  partial_provider.responses = {
    {.outcome = public_speed_outcome_e::timeout, .elapsed = std::chrono::seconds {10}},
    {.outcome = public_speed_outcome_e::succeeded, .elapsed = std::chrono::milliseconds {2'000}},
  };
  const auto partial = run_public_speed_profile(partial_provider, "client_public", [] {
    return false;
  });
  if (partial.status != status_e::partial || !partial.throughput.has_value() || partial.throughput->download_mbps.has_value() || !partial.throughput->upload_mbps.has_value() || !partial.error.has_value() || partial.error->code != "provider_timeout" || partial.error->stage != "client_public") {
    fail("a provider timeout discarded a successful opposite direction or lost its stable error code");
  }

  fake_provider_t cancellation_provider;
  const auto cancelled = run_public_speed_profile(cancellation_provider, "host_public", [] {
    return true;
  });
  if (cancelled.status != status_e::cancelled || !cancelled.error.has_value() || cancelled.error->code != "provider_cancelled" || !cancellation_provider.requests.empty()) {
    fail("provider cancellation was not checked before allocating public traffic");
  }

  fake_provider_t invalid_provider;
  invalid_provider.responses = {
    {.outcome = public_speed_outcome_e::succeeded, .transferred_bytes = 1, .elapsed = std::chrono::milliseconds {2'000}},
    {.outcome = public_speed_outcome_e::transport_failure, .elapsed = std::chrono::milliseconds {1}},
  };
  const auto invalid = run_public_speed_profile(invalid_provider, "host_public", [] {
    return false;
  });
  if (invalid.status != status_e::failed || !invalid.error.has_value() || invalid.error->code != "provider_invalid_response") {
    fail("a byte-accounting mismatch was accepted as a valid provider measurement");
  }

  std::cout << "network-test-provider-contract: PASS\n";
  return EXIT_SUCCESS;
}
