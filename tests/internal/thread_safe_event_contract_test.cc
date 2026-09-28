#include "../../src/thread_safe.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

  [[noreturn]] void fail(const std::string_view message) {
    std::cerr << "thread-safe-event-contract: FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }

}  // namespace

int main() {
  using namespace std::chrono_literals;

  safe::event_t<bool> signal;
  if (signal.pop(0ms)) {
    fail("an unset signal was consumed");
  }

  signal.raise(true);
  if (!signal.pop(0ms)) {
    fail("a raised signal was not consumed");
  }
  if (signal.pop(0ms)) {
    fail("a signal was consumed more than once");
  }

  std::cout << "thread-safe-event-contract: PASS\n";
  return EXIT_SUCCESS;
}
