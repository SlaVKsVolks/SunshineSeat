#include "src/stream_protocol.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

  [[noreturn]] void fail(const char *message) {
    std::cerr << "clipboard-image-envelope-contract: FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }

}  // namespace

int main() {
  constexpr auto max_encrypted_length = std::numeric_limits<std::uint16_t>::max();
  constexpr auto control_header_size = sizeof(std::uint16_t) * 2;
  constexpr auto encrypted_body_overhead = sizeof(std::uint32_t) + 16 + control_header_size;
  constexpr auto max_control_payload = max_encrypted_length - encrypted_body_overhead;

  if (stream::CLIPBOARD_IMAGE_MAX_CHUNK_BYTES != max_control_payload - stream::CLIPBOARD_IMAGE_MESSAGE_HEADER_SIZE) {
    fail("image chunk limit does not reserve the encrypted control envelope");
  }
  if (stream::CLIPBOARD_MAX_TEXT_BYTES != max_control_payload - 24) {
    fail("text limit does not reserve the encrypted control envelope");
  }
  if (stream::CLIPBOARD_IMAGE_MAX_CHUNK_BYTES + stream::CLIPBOARD_IMAGE_MESSAGE_HEADER_SIZE + encrypted_body_overhead > max_encrypted_length) {
    fail("maximum image chunk still exceeds the uint16 encrypted length field");
  }
  if (!stream::control_stream_plaintext_fits(stream::CONTROL_STREAM_MAX_PLAINTEXT_BYTES) || stream::control_stream_plaintext_fits(stream::CONTROL_STREAM_MAX_PLAINTEXT_BYTES + 1)) {
    fail("encrypted plaintext boundary helper is incorrect");
  }

  std::cout << "clipboard-image-envelope-contract: PASS\n";
  return EXIT_SUCCESS;
}
