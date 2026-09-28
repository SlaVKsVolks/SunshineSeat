#include "src/platform/windows/wgc_frame_handoff.h"

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

  [[noreturn]] void fail(const char *message) {
    std::cerr << "wgc-frame-handoff-contract: FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }

}  // namespace

int main() {
  std::unique_ptr<int> produced {new int {1}};
  auto replaced = platf::dxgi::detail::replace_produced_frame(produced, std::unique_ptr<int> {new int {2}});

  if (!replaced || *replaced != 1 || !produced || *produced != 2) {
    fail("frame replacement did not preserve the old frame and publish the new frame");
  }

  replaced.reset();
  if (!produced || *produced != 2) {
    fail("the old frame could not be released after the producer slot was published");
  }

  std::cout << "wgc-frame-handoff-contract: PASS\n";
  return EXIT_SUCCESS;
}
