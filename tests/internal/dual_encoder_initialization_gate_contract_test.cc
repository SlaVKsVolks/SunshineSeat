#include "src/dual_encoder_initialization_gate.h"

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>

namespace {

  using namespace std::chrono_literals;

  [[noreturn]] void fail(const char *message) {
    std::cerr << "dual-encoder-initialization-gate-contract: FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }

}  // namespace

int main() {
  dual_encoder::initialization_gate_t gate;
  auto first_lock = gate.acquire();
  if (!first_lock.owns_lock()) {
    fail("first initializer did not acquire the gate");
  }

  std::promise<void> second_attempting;
  std::promise<void> second_acquired;
  auto second_attempting_future = second_attempting.get_future();
  auto second_acquired_future = second_acquired.get_future();
  std::thread second_initializer {[&] {
    second_attempting.set_value();
    auto lock = gate.acquire();
    second_acquired.set_value();
  }};

  second_attempting_future.wait();
  const bool blocked_until_first_finishes = second_acquired_future.wait_for(100ms) == std::future_status::timeout;
  first_lock.unlock();
  second_initializer.join();

  if (!blocked_until_first_finishes) {
    fail("second encoder initialized while the first still owned the gate");
  }
  if (second_acquired_future.wait_for(0ms) != std::future_status::ready) {
    fail("second encoder did not proceed after the first released the gate");
  }

  std::cout << "dual-encoder-initialization-gate-contract: PASS\n";
  return EXIT_SUCCESS;
}
