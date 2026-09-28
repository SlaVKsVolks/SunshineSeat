#include <cstdint>
#include <cstdlib>
#include <iostream>

extern "C" int ff_amf_receive_poll_expired(std::int64_t started_at_us, std::int64_t now_us);

namespace {

  [[noreturn]] void fail(const char *message) {
    std::cerr << "amf-receive-poll-contract: FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }

}  // namespace

int main() {
  constexpr std::int64_t start = 10'000'000;

  if (ff_amf_receive_poll_expired(start, start + 499'999)) {
    fail("AMF receive polling expired before the 500 ms watchdog boundary");
  }
  if (!ff_amf_receive_poll_expired(start, start + 500'000)) {
    fail("AMF receive polling did not expire at the 500 ms watchdog boundary");
  }
  if (!ff_amf_receive_poll_expired(start, start + 700'000)) {
    fail("AMF receive polling did not remain expired after the boundary");
  }

  std::cout << "amf-receive-poll-contract: PASS\n";
  return EXIT_SUCCESS;
}
