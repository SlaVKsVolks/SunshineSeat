#include "src/platform/windows/dual_display_compositor.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>

namespace {

  using namespace std::chrono_literals;

  [[noreturn]] void fail(const char *message) {
    std::cerr << "dual-display-compositor-contract: FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }

}  // namespace

int main() {
  using namespace platf::dual_display;

  const auto now = std::chrono::steady_clock::now();
  if (!pair_frame_publish_ready(std::nullopt, now)) {
    fail("first paired frame was throttled");
  }
  if (pair_frame_publish_ready(now, now + minimum_pair_publish_interval - 1us)) {
    fail("paired capture published faster than its negotiated refresh rate");
  }
  if (!pair_frame_publish_ready(now, now + minimum_pair_publish_interval)) {
    fail("paired capture remained throttled at the next refresh boundary");
  }
  if (pair_frame_publish_ready(now + 1ms, now)) {
    fail("a future publication timestamp bypassed the paired capture throttle");
  }
  if (select_prepared_pair_profile(false) != prepared_pair_profile_e::rejected_before_allocation) {
    fail("a software profile was allowed to prepare an exclusive dual virtual pair");
  }
  if (select_prepared_pair_profile(true) != prepared_pair_profile_e::d3d11_ready) {
    fail("a D3D11 profile was rejected before paired capture preparation");
  }

  std::cout << "dual-display-compositor-contract: PASS\n";
  return EXIT_SUCCESS;
}
