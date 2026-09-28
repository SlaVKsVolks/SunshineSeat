#include "src/dual_idr_scheduler.h"

#include <array>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {

  using namespace std::chrono_literals;
  using dual_idr::scheduler_t;

  [[noreturn]] void fail(const char *message) {
    std::cerr << "dual-idr-scheduler-contract: FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }

}  // namespace

int main() {
  using clock_t = scheduler_t::clock_t;

  const auto sixty_hz = scheduler_t::frame_interval_for_framerate_x100(6000);
  const auto ntsc_sixty_hz = scheduler_t::frame_interval_for_framerate_x100(5994);
  if (!sixty_hz.has_value() || !ntsc_sixty_hz.has_value() || std::chrono::duration_cast<std::chrono::nanoseconds>(*sixty_hz) != 16'666'667ns || std::chrono::duration_cast<std::chrono::nanoseconds>(*ntsc_sixty_hz) != 16'683'351ns || scheduler_t::frame_interval_for_framerate_x100(0).has_value()) {
    fail("negotiated frame intervals were not rounded up from the requested frame rate");
  }

  const auto start = clock_t::time_point {};
  scheduler_t scheduler {40ms, 16ms};

  scheduler_t concurrent_scheduler {40ms, 16ms};
  std::array<std::uint64_t, 2> concurrent_generations {};
  std::barrier concurrent_start {3};
  std::jthread left_recovery_request {[&]() {
    concurrent_start.arrive_and_wait();
    concurrent_generations[0] = concurrent_scheduler.request_recovery(start);
  }};
  std::jthread right_recovery_request {[&]() {
    concurrent_start.arrive_and_wait();
    concurrent_generations[1] = concurrent_scheduler.request_recovery(start);
  }};
  concurrent_start.arrive_and_wait();
  left_recovery_request.join();
  right_recovery_request.join();

  if (concurrent_generations[0] == 0 || concurrent_generations[0] != concurrent_generations[1]) {
    fail("simultaneous track recovery requests created different generations");
  }
  const auto concurrent_leader = concurrent_scheduler.claim_due_idr(0, start);
  if (!concurrent_leader.has_value() || concurrent_scheduler.claim_due_idr(0, start).has_value() || concurrent_scheduler.claim_due_idr(1, start).has_value()) {
    fail("simultaneous recovery requests admitted duplicate per-track IDR claims");
  }

  const auto first_generation = scheduler.request_recovery(start);
  if (first_generation == 0) {
    fail("the first recovery generation was not created");
  }
  if (scheduler.request_recovery(start + 1ms) != first_generation) {
    fail("duplicate recovery did not coalesce into the active generation");
  }

  const auto first_leader = scheduler.claim_due_idr(0, start);
  if (!first_leader.has_value()) {
    fail("track zero did not lead the first recovery generation");
  }
  if (first_leader->due_at != start || first_leader->deadline_at != start + 16ms || !scheduler.emission_is_within_deadline(*first_leader, start + 16ms) || scheduler.emission_is_within_deadline(*first_leader, start + 17ms)) {
    fail("the leader claim did not preserve one negotiated frame interval as its latest emission time");
  }
  if (scheduler.claim_due_idr(1, start).has_value()) {
    fail("the follower was due before the leader emitted a key packet");
  }

  scheduler.complete_idr(*first_leader, false, start + 1ms);
  const auto retried_leader = scheduler.claim_due_idr(0, start + 1ms);
  if (!retried_leader.has_value()) {
    fail("an EAGAIN-style encode result cleared the leader request before a key packet");
  }
  scheduler.complete_idr(*retried_leader, true, start + 2ms);

  if (scheduler.claim_due_idr(1, start + 41ms).has_value()) {
    fail("the follower was due before the required 40 millisecond stagger elapsed");
  }
  const auto first_follower = scheduler.claim_due_idr(1, start + 42ms);
  if (!first_follower.has_value()) {
    fail("the follower was not due after the required stagger elapsed");
  }
  if (first_follower->due_at != start + 42ms || first_follower->deadline_at != start + 58ms || !scheduler.emission_is_within_deadline(*first_follower, start + 58ms) || scheduler.emission_is_within_deadline(*first_follower, start + 59ms)) {
    fail("the follower claim did not bound staggered emission to 40 milliseconds plus one frame interval");
  }
  scheduler.complete_idr(*first_follower, true, start + 42ms);
  const auto next_recovery_at = start + 42ms + dual_idr::recovery_quiet_period;

  // A simultaneous or delayed duplicate from the peer track must not create
  // another paired keyframe burst after the first generation completed. The
  // scheduler remains non-blocking: it simply has no due claim during the
  // quiet period, so the broadcaster can keep packetizing ordinary frames.
  for (const auto duplicate_at : {start + 60ms, start + 100ms, start + 2s, next_recovery_at - 1ms}) {
    if (scheduler.request_recovery(duplicate_at) != first_generation ||
        scheduler.has_pending_recovery() ||
        scheduler.claim_due_idr(0, duplicate_at).has_value() ||
        scheduler.claim_due_idr(1, duplicate_at).has_value()) {
      fail("a delayed duplicate recovery request created a second paired IDR burst");
    }
  }

  const auto second_generation = scheduler.request_recovery(next_recovery_at);
  if (second_generation == first_generation) {
    fail("a new recovery request was not admitted after the quiet period");
  }
  if (scheduler.claim_due_idr(0, next_recovery_at).has_value()) {
    fail("later recovery generations did not alternate the leading track");
  }
  const auto second_leader = scheduler.claim_due_idr(1, next_recovery_at);
  if (!second_leader.has_value()) {
    fail("track one did not lead the second recovery generation");
  }
  scheduler.complete_idr(*second_leader, true, next_recovery_at);
  const auto second_follower = scheduler.claim_due_idr(0, next_recovery_at + 40ms);
  if (!second_follower.has_value()) {
    fail("the alternating generation did not release its follower after the stagger");
  }
  scheduler.complete_idr(*second_follower, true, next_recovery_at + 40ms);

  const auto cancelled_at = next_recovery_at + dual_idr::recovery_quiet_period * 2;
  const auto cancelled_generation = scheduler.request_recovery(cancelled_at);
  const auto stale_claim = scheduler.claim_due_idr(0, cancelled_at);
  if (!stale_claim.has_value()) {
    fail("the third recovery generation did not return to track-zero leadership");
  }
  scheduler.cancel_generation(cancelled_generation);
  scheduler.complete_idr(*stale_claim, true, cancelled_at);
  if (scheduler.claim_due_idr(1, cancelled_at + 500ms).has_value() || scheduler.has_pending_recovery()) {
    fail("a cancelled stale claim revived a pending follower request");
  }

  const auto teardown_at = cancelled_at + dual_idr::recovery_quiet_period;
  const auto teardown_generation = scheduler.request_recovery(teardown_at);
  if (teardown_generation == 0) {
    fail("the scheduler did not accept a recovery after cancellation");
  }
  scheduler.cancel_all();
  if (scheduler.has_pending_recovery() || scheduler.claim_due_idr(0, teardown_at).has_value() || scheduler.claim_due_idr(1, teardown_at).has_value()) {
    fail("teardown cancellation left a due IDR request behind");
  }

  std::cout << "dual-idr-scheduler-contract: PASS\n";
  return EXIT_SUCCESS;
}
