#include "third-party/moonlight-common-c/src/Input.h"
#include "third-party/moonlight-common-c/src/Limelight.h"

#include <cstddef>
#include <cstdlib>
#include <iostream>

namespace {

  [[noreturn]] void fail(const char *message) {
    std::cerr << "input-timing-wire-contract: FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }

}  // namespace

int main() {
  if (SS_INPUT_TIMING_MAGIC != 0x55000008 || SS_INPUT_TIMING_VERSION != 1) {
    fail("the marker must use the version-one Sunshine extension identifier");
  }
  if (SS_INPUT_TIMING_FAMILY_MOUSE != 1 || SS_INPUT_TIMING_FAMILY_KEYBOARD != 2) {
    fail("the marker must expose only broad mouse and keyboard families");
  }
  if (LI_FF_INPUT_TIMING_V1 != 0x04) {
    fail("the marker capability must occupy the negotiated third feature bit");
  }
  if (sizeof(SS_INPUT_TIMING_PACKET) != 36 ||
      offsetof(SS_INPUT_TIMING_PACKET, version) != 8 ||
      offsetof(SS_INPUT_TIMING_PACKET, family) != 9 ||
      offsetof(SS_INPUT_TIMING_PACKET, sequence) != 12 ||
      offsetof(SS_INPUT_TIMING_PACKET, capture_time_us) != 20 ||
      offsetof(SS_INPUT_TIMING_PACKET, send_time_us) != 28) {
    fail("the marker must be a packed header plus opaque timing metadata only");
  }

  if (LI_INPUT_TIMING_ACK_PTYPE != 0x3009 ||
      SS_INPUT_TIMING_ACK_VERSION != SS_INPUT_TIMING_VERSION ||
      SS_INPUT_TIMING_STATUS_APPLIED != 1 || SS_INPUT_TIMING_STATUS_TIMEOUT != 2) {
    fail("the acknowledgment must use its negotiated SunshineSeat extension and stable status values");
  }
  if (sizeof(SS_INPUT_TIMING_ACK_PACKET) != 60 ||
      offsetof(SS_INPUT_TIMING_ACK_PACKET, version) != 0 ||
      offsetof(SS_INPUT_TIMING_ACK_PACKET, status) != 1 ||
      offsetof(SS_INPUT_TIMING_ACK_PACKET, family) != 2 ||
      offsetof(SS_INPUT_TIMING_ACK_PACKET, sequence) != 4 ||
      offsetof(SS_INPUT_TIMING_ACK_PACKET, client_capture_time_us) != 12 ||
      offsetof(SS_INPUT_TIMING_ACK_PACKET, client_send_time_us) != 20 ||
      offsetof(SS_INPUT_TIMING_ACK_PACKET, host_received_time_us) != 28 ||
      offsetof(SS_INPUT_TIMING_ACK_PACKET, host_attached_time_us) != 36 ||
      offsetof(SS_INPUT_TIMING_ACK_PACKET, host_injected_time_us) != 44 ||
      offsetof(SS_INPUT_TIMING_ACK_PACKET, host_send_time_us) != 52) {
    fail("the acknowledgment must carry only bounded correlation and client/host timing fields");
  }

  std::cout << "input-timing-wire-contract: PASS\n";
  return EXIT_SUCCESS;
}
