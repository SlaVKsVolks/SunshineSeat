/**
 * @file src/stream_protocol.h
 * @brief Wire-size constants for the encrypted control stream.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace stream {

  constexpr std::size_t CONTROL_STREAM_MAX_ENCRYPTED_LENGTH = std::numeric_limits<std::uint16_t>::max();
  constexpr std::size_t CONTROL_STREAM_ENCRYPTED_SEQUENCE_SIZE = sizeof(std::uint32_t);
  constexpr std::size_t CONTROL_STREAM_ENCRYPTED_TAG_SIZE = 16;
  constexpr std::size_t CONTROL_STREAM_CONTROL_HEADER_SIZE = sizeof(std::uint16_t) * 2;
  constexpr std::size_t CONTROL_STREAM_MAX_PLAINTEXT_BYTES =
    CONTROL_STREAM_MAX_ENCRYPTED_LENGTH -
    CONTROL_STREAM_ENCRYPTED_SEQUENCE_SIZE -
    CONTROL_STREAM_ENCRYPTED_TAG_SIZE;
  constexpr std::size_t CONTROL_STREAM_MAX_PAYLOAD_BYTES =
    CONTROL_STREAM_MAX_PLAINTEXT_BYTES - CONTROL_STREAM_CONTROL_HEADER_SIZE;

  constexpr std::size_t CLIPBOARD_MESSAGE_HEADER_SIZE = 24;
  constexpr std::size_t CLIPBOARD_MAX_TEXT_BYTES =
    CONTROL_STREAM_MAX_PAYLOAD_BYTES - CLIPBOARD_MESSAGE_HEADER_SIZE;
  constexpr std::size_t CLIPBOARD_MAX_IMAGE_BYTES = 16 * 1024 * 1024;
  constexpr std::size_t CLIPBOARD_IMAGE_MESSAGE_HEADER_SIZE = 40;
  constexpr std::size_t CLIPBOARD_IMAGE_MAX_CHUNK_BYTES =
    CONTROL_STREAM_MAX_PAYLOAD_BYTES - CLIPBOARD_IMAGE_MESSAGE_HEADER_SIZE;

  constexpr bool control_stream_plaintext_fits(std::size_t plaintext_size) {
    return plaintext_size <= CONTROL_STREAM_MAX_PLAINTEXT_BYTES;
  }

}  // namespace stream
