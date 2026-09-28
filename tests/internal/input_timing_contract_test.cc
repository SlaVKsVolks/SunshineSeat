#include "src/input_timing.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

  [[noreturn]] void fail(const char *message) {
    std::cerr << "input-timing-contract: FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }

}  // namespace

int main() {
  using namespace input_timing;

  const marker_t keyboard_marker {
    current_schema_version,
    event_family_e::keyboard,
    41,
    2'000'000,
    2'000'040
  };

  if (!validate_marker(keyboard_marker)) {
    fail("a version-one keyboard marker was rejected");
  }
  if (marker_contains_input_content(keyboard_marker)) {
    fail("the marker contract retained input content");
  }

  const auto wire_mouse_marker = decode_wire_marker(1, 1, 0, 40, 2'100'000, 2'100'040);
  if (!wire_mouse_marker.has_value() || wire_mouse_marker->family != event_family_e::mouse_motion ||
      wire_mouse_marker->sequence != 40) {
    fail("a valid metadata-only mouse wire marker was rejected or remapped");
  }
  if (decode_wire_marker(1, 3, 0, 40, 2'100'000, 2'100'040).has_value() ||
      decode_wire_marker(1, 1, 1, 40, 2'100'000, 2'100'040).has_value()) {
    fail("a marker with an unsupported family or nonzero reserved field was accepted");
  }

  sampler_t sampler;
  if (!sampler.should_sample(event_family_e::mouse_motion, 1'000'000)) {
    fail("the first mouse-motion sample was suppressed");
  }
  if (sampler.should_sample(event_family_e::mouse_motion, 1'099'999)) {
    fail("mouse motion bypassed the 100 ms rate limit");
  }
  if (!sampler.should_sample(event_family_e::keyboard, 1'099'999)) {
    fail("a keyboard transition was rate limited as mouse motion");
  }

  const std::vector<clock_exchange_t> exchanges {
    {1'000'000, 1'000'500, 1'000'550, 1'001'050},
    {2'000'000, 2'000'500, 2'000'550, 2'001'050},
  };
  const auto clock = estimate_clock(exchanges);
  if (!clock.has_value() || clock->offset_us != 0 || clock->uncertainty_us != 500) {
    fail("symmetric exchanges did not produce the expected clock estimate");
  }
  if (one_way_delay_us(2'000'040, 2'001'000, clock_estimate_t {0, 1'001}).has_value()) {
    fail("one-way timing was reported above the one-millisecond uncertainty gate");
  }

  tracker_t tracker;
  tracker.accept_marker(keyboard_marker, 2'001'000);
  const auto associated = tracker.attach_next_input(event_family_e::keyboard, 2'001'020);
  if (!associated.has_value() || associated->sequence != 41) {
    fail("the next keyboard input did not retain its FIFO timing marker");
  }
  if (tracker.complete_injection(41, false, 2'001'030).has_value()) {
    fail("a failed Windows injection produced a completed timing record");
  }
  if (!tracker.complete_injection(41, true, 2'001'040).has_value()) {
    fail("a successful Windows injection did not complete the timing record");
  }

  tracker_t interleaved_tracker;
  const marker_t mouse_marker {
    current_schema_version,
    event_family_e::mouse_button,
    42,
    3'000'000,
    3'000'040,
  };
  const marker_t second_keyboard_marker {
    current_schema_version,
    event_family_e::keyboard,
    43,
    3'000'010,
    3'000'050,
  };
  interleaved_tracker.accept_marker(mouse_marker, 3'001'000);
  interleaved_tracker.accept_marker(second_keyboard_marker, 3'001'010);
  const auto keyboard_association = interleaved_tracker.attach_next_input(event_family_e::keyboard, 3'001'020);
  const auto mouse_association = interleaved_tracker.attach_next_input(event_family_e::mouse_button, 3'001'030);
  if (!keyboard_association.has_value() || keyboard_association->sequence != 43 ||
      !mouse_association.has_value() || mouse_association->sequence != 42) {
    fail("interleaved markers did not retain FIFO association within their eligible families");
  }

  tracker_t bounded_tracker;
  for (std::uint64_t sequence = 1; sequence <= maximum_pending_markers + 1; ++sequence) {
    bounded_tracker.accept_marker(
      marker_t {current_schema_version, event_family_e::keyboard, sequence, 4'000'000, 4'000'040},
      4'001'000 + static_cast<std::int64_t>(sequence)
    );
  }
  for (std::uint64_t sequence = 1; sequence <= maximum_pending_markers; ++sequence) {
    const auto retained = bounded_tracker.attach_next_input(event_family_e::keyboard, 4'002'000 + static_cast<std::int64_t>(sequence));
    if (!retained.has_value() || retained->sequence != sequence) {
      fail("a full marker queue did not retain its oldest FIFO associations");
    }
  }
  if (bounded_tracker.attach_next_input(event_family_e::keyboard, 4'003'000).has_value()) {
    fail("a marker above the pending-queue bound was retained");
  }
  bounded_tracker.accept_marker(
    marker_t {current_schema_version, event_family_e::keyboard, maximum_pending_markers + 1, 4'000'000, 4'000'040},
    4'004'000
  );
  if (bounded_tracker.attach_next_input(event_family_e::keyboard, 4'004'020).has_value()) {
    fail("associated input timing state exceeded its configured bound");
  }

  tracker_t expired_marker_tracker;
  expired_marker_tracker.accept_marker(
    marker_t {current_schema_version, event_family_e::keyboard, 99, 4'000'000, 4'000'040},
    4'001'000
  );
  expired_marker_tracker.accept_marker(
    marker_t {current_schema_version, event_family_e::keyboard, 100, 6'100'000, 6'100'040},
    6'100'000
  );
  const auto fresh_after_expired = expired_marker_tracker.attach_next_input(event_family_e::keyboard, 6'100'020);
  if (!fresh_after_expired.has_value() || fresh_after_expired->sequence != 100) {
    fail("an expired marker was allowed to steal the next input association");
  }
  expired_marker_tracker.accept_marker(
    marker_t {current_schema_version, event_family_e::keyboard, 101, 7'000'000, 7'000'040},
    7'001'000
  );
  if (expired_marker_tracker.attach_next_input(event_family_e::keyboard, 9'001'001).has_value()) {
    fail("an expired marker was associated with a later input");
  }

  tracker_t rejected_input_tracker;
  rejected_input_tracker.accept_marker(keyboard_marker, 5'001'000);
  const auto rejected_association = rejected_input_tracker.attach_next_input(event_family_e::keyboard, 5'001'020);
  if (!rejected_association.has_value()) {
    fail("a valid marker could not be associated before a rejected input");
  }
  rejected_input_tracker.discard_injection(rejected_association->sequence);
  if (rejected_input_tracker.complete_injection(rejected_association->sequence, true, 5'001'040).has_value()) {
    fail("a rejected input left timing metadata eligible for later completion");
  }

  tracker_t batching_tracker;
  batching_tracker.accept_marker(mouse_marker, 6'001'000);
  if (!batching_tracker.has_pending_marker_for(event_family_e::mouse_motion) ||
      batching_tracker.has_pending_marker_for(event_family_e::keyboard)) {
    fail("the batching barrier did not report only the marker's eligible family");
  }
  if (!batching_tracker.attach_next_input(event_family_e::mouse_motion, 6'001'020).has_value() ||
      batching_tracker.has_pending_marker_for(event_family_e::mouse_motion)) {
    fail("a consumed marker remained a batching barrier");
  }

  std::cout << "input-timing-contract: PASS\n";
  return EXIT_SUCCESS;
}
