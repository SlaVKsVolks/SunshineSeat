#include "src/dual_encoder_retirement.h"

#include <chrono>
#include <cstdlib>
#include <iostream>

namespace {

  using namespace std::chrono_literals;
  using dual_encoder::retirement_t;

  [[noreturn]] void fail(const char *message) {
    std::cerr << "dual-encoder-retirement-contract: FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }

}  // namespace

int main() {
  using clock_t = retirement_t::clock_t;

  constexpr auto stall_timeout = 500ms;
  constexpr auto recovery_grace = 1500ms;
  constexpr auto emergency_shutdown_budget = 45s;
  const auto start = clock_t::time_point {};

  retirement_t simultaneous_stalls {stall_timeout, recovery_grace};
  const auto left_claim = simultaneous_stalls.begin_send(0, start);
  const auto right_claim = simultaneous_stalls.begin_send(1, start);
  if (!left_claim || !right_claim) {
    fail("both dual encoder tracks were not admitted into the same generation");
  }

  if (simultaneous_stalls.control_state() != dual_encoder::control_state_e::healthy ||
      !simultaneous_stalls.accepts_control_ping() ||
      simultaneous_stalls.poll(start + stall_timeout - 1ms).action != dual_encoder::action_e::none) {
    fail("an in-flight dual encoder generation did not preserve control ping liveness before its deadline");
  }

  const auto retire = simultaneous_stalls.poll(start + stall_timeout);
  if (retire.action != dual_encoder::action_e::retire_generation ||
      retire.stalled_track_count != 2 ||
      simultaneous_stalls.control_state() != dual_encoder::control_state_e::recovery_requested ||
      !simultaneous_stalls.accepts_control_ping() ||
      simultaneous_stalls.begin_send(0, start + stall_timeout).has_value()) {
    fail("simultaneous encoder stalls did not retire the generation without admitting a replacement call");
  }

  if (simultaneous_stalls.poll(start + stall_timeout + recovery_grace - 1ms).action != dual_encoder::action_e::none) {
    fail("the supervisor escalated before the bounded control recovery grace elapsed");
  }

  const auto force_recovery = simultaneous_stalls.poll(start + stall_timeout + recovery_grace);
  if (force_recovery.action != dual_encoder::action_e::force_process_recovery ||
      simultaneous_stalls.control_state() != dual_encoder::control_state_e::process_recovery_required ||
      simultaneous_stalls.accepts_control_ping() ||
      stall_timeout + recovery_grace >= emergency_shutdown_budget) {
    fail("a wedged dual encoder generation did not force recovery inside the emergency shutdown budget");
  }

  retirement_t returned_calls {stall_timeout, recovery_grace};
  const auto first = returned_calls.begin_send(0, start);
  const auto second = returned_calls.begin_send(1, start);
  if (!first || !second ||
      returned_calls.poll(start + stall_timeout).action != dual_encoder::action_e::retire_generation) {
    fail("the retiring-generation setup was not deterministic");
  }
  returned_calls.finish_send(*first);
  returned_calls.finish_send(*second);
  if (returned_calls.poll(start + stall_timeout + recovery_grace).action != dual_encoder::action_e::none ||
      returned_calls.control_state() != dual_encoder::control_state_e::recovery_requested) {
    fail("returned encoder calls incorrectly forced process recovery after the generation was retired");
  }

  std::cout << "dual-encoder-retirement-contract: PASS\n";
  return EXIT_SUCCESS;
}
