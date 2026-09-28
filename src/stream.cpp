/**
 * @file src/stream.cpp
 * @brief Definitions for the streaming protocols.
 */

// standard includes
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <queue>
#include <random>

#ifdef _WIN32
  #include <windows.h>
  #include <winsock2.h>
#endif

// lib includes
#include <boost/endian/arithmetic.hpp>
#include <openssl/err.h>

extern "C" {
  // clang-format off
#include <moonlight-common-c/src/Limelight-internal.h>
#include "rswrapper.h"
  // clang-format on
}

// local includes
#include "client_mic.h"
#include "config.h"
#include "display_device.h"
#include "globals.h"
#include "input.h"
#include "input_timing.h"
#include "logging.h"
#include "network.h"
#include "platform/common.h"
#include "process.h"
#include "stream.h"
#include "sync.h"
#include "system_tray.h"
#include "thread_safe.h"
#include "utility.h"

constexpr int IDX_START_A = 0;
constexpr int IDX_START_B = 1;
constexpr int IDX_INVALIDATE_REF_FRAMES = 2;
constexpr int IDX_LOSS_STATS = 3;
constexpr int IDX_INPUT_DATA = 5;
constexpr int IDX_RUMBLE_DATA = 6;
constexpr int IDX_TERMINATION = 7;
constexpr int IDX_PERIODIC_PING = 8;
constexpr int IDX_REQUEST_IDR_FRAME = 9;
constexpr int IDX_ENCRYPTED = 10;
constexpr int IDX_HDR_MODE = 11;
constexpr int IDX_RUMBLE_TRIGGER_DATA = 12;
constexpr int IDX_SET_MOTION_EVENT = 13;
constexpr int IDX_SET_RGB_LED = 14;
constexpr int IDX_EXEC_SERVER_CMD = 15;
constexpr int IDX_SET_CLIPBOARD = 16;
constexpr int IDX_FILE_TRANSFER_NONCE_REQUEST = 17;
constexpr int IDX_SET_ADAPTIVE_TRIGGERS = 18;
constexpr int IDX_CLIPBOARD_HOST_TO_CLIENT = 19;
constexpr int IDX_CLIPBOARD_CLIENT_TO_HOST = 20;
constexpr int IDX_MIC_CLIENT_TO_HOST = 21;
constexpr int IDX_ADAPTIVE_BITRATE = 22;
constexpr int IDX_CLIPBOARD_IMAGE_HOST_TO_CLIENT = 23;
constexpr int IDX_CLIPBOARD_IMAGE_CLIENT_TO_HOST = 24;
constexpr int IDX_INPUT_TIMING_ACK = 25;

static const short packetTypes[] = {
  0x0305,  // Start A
  0x0307,  // Start B
  0x0301,  // Invalidate reference frames
  0x0201,  // Loss Stats
  0x0204,  // Frame Stats (unused)
  0x0206,  // Input data
  0x010b,  // Rumble data
  0x0109,  // Termination
  0x0200,  // Periodic Ping
  0x0302,  // IDR frame
  0x0001,  // fully encrypted
  0x010e,  // HDR mode
  0x5500,  // Rumble triggers (Sunshine protocol extension)
  0x5501,  // Set motion event (Sunshine protocol extension)
  0x5502,  // Set RGB LED (Sunshine protocol extension)
  0x3000,  // Execute server command (Apollo protocol extension)
  0x3001,  // Set clipboard (Apollo protocol extension)
  0x3002,  // File transfer nonce request (Apollo protocol extension)
  0x5503,  // Set Adaptive triggers (Sunshine protocol extension)
  0x3003,  // Clipboard update host to client (SunshineSeat protocol extension)
  0x3004,  // Clipboard update client to host (SunshineSeat protocol extension)
  0x3005,  // Client microphone audio (SunshineSeat protocol extension)
  0x3006,  // Adaptive bitrate request (SunshineSeat protocol extension)
  0x3007,  // Clipboard image update host to client (SunshineSeat protocol extension)
  0x3008,  // Clipboard image update client to host (SunshineSeat protocol extension)
  0x3009,  // Input timing acknowledgment (SunshineSeat protocol extension)
};

namespace asio = boost::asio;
namespace sys = boost::system;

using asio::ip::tcp;
using asio::ip::udp;

using namespace std::literals;

namespace stream {

  namespace {
    constexpr std::uint8_t CLIPBOARD_MESSAGE_VERSION = 1;
    constexpr std::uint8_t CLIPBOARD_ORIGIN_HOST = 1;
    constexpr std::uint8_t CLIPBOARD_ORIGIN_CLIENT = 2;
    constexpr std::uint8_t CLIPBOARD_IMAGE_MESSAGE_VERSION = 1;
    constexpr std::uint8_t CLIPBOARD_IMAGE_FORMAT_BMP = 1;

    std::uint16_t clipboard_read_u16(const char *data) {
      return static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[0])) |
             static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[1])) << 8;
    }

    bool clipboard_valid_utf8(const std::string_view &text) {
      std::size_t index = 0;
      while (index < text.size()) {
        auto byte = static_cast<std::uint8_t>(text[index]);
        std::size_t continuation_count = 0;
        std::uint32_t codepoint = 0;

        if (byte <= 0x7f) {
          ++index;
          continue;
        } else if (byte >= 0xc2 && byte <= 0xdf) {
          continuation_count = 1;
          codepoint = byte & 0x1f;
        } else if (byte >= 0xe0 && byte <= 0xef) {
          continuation_count = 2;
          codepoint = byte & 0x0f;
        } else if (byte >= 0xf0 && byte <= 0xf4) {
          continuation_count = 3;
          codepoint = byte & 0x07;
        } else {
          return false;
        }

        if (index + continuation_count >= text.size()) {
          return false;
        }

        for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
          auto continuation = static_cast<std::uint8_t>(text[index + offset]);
          if ((continuation & 0xc0) != 0x80) {
            return false;
          }
          codepoint = (codepoint << 6) | (continuation & 0x3f);
        }

        if ((continuation_count == 2 && codepoint < 0x800) || (continuation_count == 3 && codepoint < 0x10000) || (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff) {
          return false;
        }

        index += continuation_count + 1;
      }

      return true;
    }

    std::uint32_t clipboard_read_u32(const char *data) {
      return static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[0])) |
             static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[1])) << 8 |
             static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[2])) << 16 |
             static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[3])) << 24;
    }

    std::uint64_t clipboard_read_u64(const char *data) {
      std::uint64_t value = 0;
      for (int shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[shift / 8])) << shift;
      }
      return value;
    }

    void clipboard_write_u32(char *data, std::uint32_t value) {
      for (int shift = 0; shift < 32; shift += 8) {
        data[shift / 8] = static_cast<char>((value >> shift) & 0xff);
      }
    }

    void clipboard_write_u64(char *data, std::uint64_t value) {
      for (int shift = 0; shift < 64; shift += 8) {
        data[shift / 8] = static_cast<char>((value >> shift) & 0xff);
      }
    }

    bool clipboard_valid_bmp(const std::string_view &image) {
      if (image.size() < 54 || image[0] != 'B' || image[1] != 'M') {
        return false;
      }

      auto file_size = clipboard_read_u32(image.data() + 2);
      auto pixel_offset = clipboard_read_u32(image.data() + 10);
      auto dib_size = clipboard_read_u32(image.data() + 14);
      auto width = static_cast<std::int32_t>(clipboard_read_u32(image.data() + 18));
      auto height = static_cast<std::int32_t>(clipboard_read_u32(image.data() + 22));
      auto planes = clipboard_read_u16(image.data() + 26);
      auto bits_per_pixel = clipboard_read_u16(image.data() + 28);
      auto compression = clipboard_read_u32(image.data() + 30);

      if (file_size != image.size() || dib_size < 40 || dib_size > image.size() - 14 || pixel_offset < 14 + dib_size || pixel_offset >= image.size() || width == 0 || height == 0 || planes != 1 || (bits_per_pixel != 1 && bits_per_pixel != 4 && bits_per_pixel != 8 && bits_per_pixel != 16 && bits_per_pixel != 24 && bits_per_pixel != 32) || (compression != 0 && compression != 3)) {
        return false;
      }

      const auto width_abs = static_cast<std::uint64_t>(width < 0 ? -static_cast<std::int64_t>(width) : width);
      const auto height_abs = static_cast<std::uint64_t>(height < 0 ? -static_cast<std::int64_t>(height) : height);
      const auto row_bits = width_abs * bits_per_pixel;
      const auto row_bytes = ((row_bits + 31) / 32) * 4;
      if (height_abs == 0 || row_bytes > std::numeric_limits<std::uint64_t>::max() / height_abs) {
        return false;
      }
      const auto pixel_bytes = row_bytes * height_abs;
      if (pixel_bytes > std::numeric_limits<std::uint64_t>::max() - pixel_offset) {
        return false;
      }
      const auto pixel_end = static_cast<std::uint64_t>(pixel_offset) + pixel_bytes;
      return pixel_end <= image.size();
    }
  }  // namespace

  std::uint64_t clipboard_text_hash(const std::string_view &text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (auto byte : text) {
      hash ^= static_cast<std::uint8_t>(byte);
      hash *= 1099511628211ULL;
    }
    return hash;
  }

  std::uint64_t clipboard_image_hash(const std::string_view &image) {
    return clipboard_text_hash(image);
  }

  bool decode_clipboard_message(const std::string_view &payload, clipboard_message_t &message) {
    if (payload.size() < CLIPBOARD_MESSAGE_HEADER_SIZE || static_cast<std::uint8_t>(payload[0]) != CLIPBOARD_MESSAGE_VERSION || (static_cast<std::uint8_t>(payload[1]) != CLIPBOARD_ORIGIN_HOST && static_cast<std::uint8_t>(payload[1]) != CLIPBOARD_ORIGIN_CLIENT)) {
      return false;
    }

    auto origin_id = clipboard_read_u64(payload.data() + 4);
    if (origin_id == 0) {
      return false;
    }

    auto length = clipboard_read_u32(payload.data() + 20);
    if (length > CLIPBOARD_MAX_TEXT_BYTES || payload.size() != CLIPBOARD_MESSAGE_HEADER_SIZE + length) {
      return false;
    }

    std::string_view text {payload.data() + CLIPBOARD_MESSAGE_HEADER_SIZE, length};
    auto content_hash = clipboard_read_u64(payload.data() + 12);
    if (!clipboard_valid_utf8(text) || clipboard_text_hash(text) != content_hash) {
      return false;
    }

    message.origin = static_cast<std::uint8_t>(payload[1]);
    message.origin_id = origin_id;
    message.content_hash = content_hash;
    message.text.assign(text);
    return true;
  }

  bool decode_clipboard_image_chunk(const std::string_view &payload, clipboard_image_chunk_t &chunk) {
    if (payload.size() < CLIPBOARD_IMAGE_MESSAGE_HEADER_SIZE || static_cast<std::uint8_t>(payload[0]) != CLIPBOARD_IMAGE_MESSAGE_VERSION || (static_cast<std::uint8_t>(payload[1]) != CLIPBOARD_ORIGIN_HOST && static_cast<std::uint8_t>(payload[1]) != CLIPBOARD_ORIGIN_CLIENT) || static_cast<std::uint8_t>(payload[2]) != CLIPBOARD_IMAGE_FORMAT_BMP || payload[3] != 0 || payload[38] != 0 || payload[39] != 0) {
      return false;
    }

    auto origin_id = clipboard_read_u64(payload.data() + 4);
    auto content_hash = clipboard_read_u64(payload.data() + 12);
    auto transfer_id = clipboard_read_u64(payload.data() + 20);
    auto total_length = clipboard_read_u32(payload.data() + 28);
    auto offset = clipboard_read_u32(payload.data() + 32);
    auto chunk_length = clipboard_read_u16(payload.data() + 36);

    if (origin_id == 0 || transfer_id == 0 || total_length < 54 || total_length > CLIPBOARD_MAX_IMAGE_BYTES || chunk_length == 0 || chunk_length > CLIPBOARD_IMAGE_MAX_CHUNK_BYTES || offset > total_length || chunk_length > total_length - offset || payload.size() != CLIPBOARD_IMAGE_MESSAGE_HEADER_SIZE + chunk_length) {
      return false;
    }

    chunk.origin = static_cast<std::uint8_t>(payload[1]);
    chunk.format = static_cast<std::uint8_t>(payload[2]);
    chunk.origin_id = origin_id;
    chunk.content_hash = content_hash;
    chunk.transfer_id = transfer_id;
    chunk.total_length = total_length;
    chunk.offset = offset;
    chunk.data.assign(payload.data() + CLIPBOARD_IMAGE_MESSAGE_HEADER_SIZE, chunk_length);
    if (offset == 0 && !clipboard_valid_bmp(std::string_view {chunk.data})) {
      // A complete one-chunk image must be recognizable immediately. For a
      // multi-chunk transfer the host validates the BMP after reassembly.
      if (total_length == chunk_length) {
        return false;
      }
    }
    return true;
  }

  bool should_apply_clipboard_update(std::uint64_t incoming_hash, std::uint64_t last_applied_hash) {
    return incoming_hash != 0 && incoming_hash != last_applied_hash;
  }

  std::optional<int> decode_adaptive_bitrate_request(const std::string_view &payload) {
    if (payload.size() != sizeof(std::uint32_t)) {
      return std::nullopt;
    }

    const auto value = static_cast<std::uint32_t>(static_cast<std::uint8_t>(payload[0])) |
                       (static_cast<std::uint32_t>(static_cast<std::uint8_t>(payload[1])) << 8) |
                       (static_cast<std::uint32_t>(static_cast<std::uint8_t>(payload[2])) << 16) |
                       (static_cast<std::uint32_t>(static_cast<std::uint8_t>(payload[3])) << 24);
    if (value < 3000 || value > 500000) {
      return std::nullopt;
    }

    return static_cast<int>(value);
  }

  adaptive_bitrate_action_e evaluate_adaptive_bitrate_request(
    const int current_kbps,
    const int target_kbps,
    const std::chrono::steady_clock::duration target_age,
    const std::chrono::steady_clock::duration time_since_last_apply
  ) {
    if (current_kbps <= 0 || target_kbps <= 0 || target_kbps == current_kbps) {
      return adaptive_bitrate_action_e::ignore;
    }

    const auto delta_kbps = std::llabs(static_cast<long long>(target_kbps) - current_kbps);
    const auto minimum_significant_delta_kbps = (static_cast<long long>(current_kbps) + 9) / 10;
    if (delta_kbps < minimum_significant_delta_kbps) {
      return adaptive_bitrate_action_e::ignore;
    }

    if (target_kbps < current_kbps) {
      return adaptive_bitrate_action_e::apply;
    }

    if (target_age < std::chrono::seconds {30} || time_since_last_apply < std::chrono::seconds {60}) {
      return adaptive_bitrate_action_e::defer;
    }

    return adaptive_bitrate_action_e::apply;
  }

  struct adaptive_bitrate_policy_state_t {
    std::optional<int> pending_target_kbps;
    std::optional<std::chrono::steady_clock::time_point> pending_target_since;
    std::optional<std::chrono::steady_clock::time_point> last_applied_at;
  };

  std::optional<int> select_adaptive_bitrate_target(
    adaptive_bitrate_policy_state_t &state,
    const int current_kbps,
    const int target_kbps,
    const std::chrono::steady_clock::time_point now
  ) {
    if (!state.pending_target_kbps || *state.pending_target_kbps != target_kbps) {
      state.pending_target_kbps = target_kbps;
      state.pending_target_since = now;
    }

    const auto target_age = now - *state.pending_target_since;
    const auto time_since_last_apply = state.last_applied_at ? now - *state.last_applied_at : std::chrono::seconds {60};
    const auto action = evaluate_adaptive_bitrate_request(current_kbps, target_kbps, target_age, time_since_last_apply);
    if (action == adaptive_bitrate_action_e::ignore) {
      state.pending_target_kbps.reset();
      state.pending_target_since.reset();
      return std::nullopt;
    }
    if (action == adaptive_bitrate_action_e::defer) {
      return std::nullopt;
    }

    state.pending_target_kbps.reset();
    state.pending_target_since.reset();
    state.last_applied_at = now;
    return target_kbps;
  }

  namespace {
    std::string encode_clipboard_message(std::uint8_t origin, std::uint64_t origin_id, const std::string_view &text) {
      if (text.size() > CLIPBOARD_MAX_TEXT_BYTES || !clipboard_valid_utf8(text)) {
        return {};
      }

      std::string payload(CLIPBOARD_MESSAGE_HEADER_SIZE + text.size(), '\0');
      payload[0] = static_cast<char>(CLIPBOARD_MESSAGE_VERSION);
      payload[1] = static_cast<char>(origin);
      clipboard_write_u64(payload.data() + 4, origin_id);
      clipboard_write_u64(payload.data() + 12, clipboard_text_hash(text));
      clipboard_write_u32(payload.data() + 20, static_cast<std::uint32_t>(text.size()));
      std::copy(text.begin(), text.end(), payload.begin() + CLIPBOARD_MESSAGE_HEADER_SIZE);
      return payload;
    }

    std::string encode_clipboard_image_chunk(std::uint8_t origin, std::uint64_t origin_id, std::uint64_t content_hash, std::uint64_t transfer_id, std::uint32_t total_length, std::uint32_t offset, const std::string_view &chunk) {
      if (origin_id == 0 || transfer_id == 0 || total_length < 54 || total_length > CLIPBOARD_MAX_IMAGE_BYTES || chunk.empty() || chunk.size() > CLIPBOARD_IMAGE_MAX_CHUNK_BYTES || offset > total_length || chunk.size() > total_length - offset) {
        return {};
      }

      std::string payload(CLIPBOARD_IMAGE_MESSAGE_HEADER_SIZE + chunk.size(), '\0');
      payload[0] = static_cast<char>(CLIPBOARD_IMAGE_MESSAGE_VERSION);
      payload[1] = static_cast<char>(origin);
      payload[2] = static_cast<char>(CLIPBOARD_IMAGE_FORMAT_BMP);
      clipboard_write_u64(payload.data() + 4, origin_id);
      clipboard_write_u64(payload.data() + 12, content_hash);
      clipboard_write_u64(payload.data() + 20, transfer_id);
      clipboard_write_u32(payload.data() + 28, total_length);
      clipboard_write_u32(payload.data() + 32, offset);
      clipboard_write_u32(payload.data() + 36, static_cast<std::uint32_t>(chunk.size()));
      std::copy(chunk.begin(), chunk.end(), payload.begin() + CLIPBOARD_IMAGE_MESSAGE_HEADER_SIZE);
      return payload;
    }

    std::uint64_t clipboard_origin_id() {
      static const std::uint64_t value = [] {
        std::random_device random;
        auto result = (static_cast<std::uint64_t>(random()) << 32) ^ random();
        return result == 0 ? 1ULL : result;
      }();
      return value;
    }

    std::uint64_t clipboard_random_id() {
      std::random_device random;
      auto result = (static_cast<std::uint64_t>(random()) << 32) ^ random();
      return result == 0 ? 1ULL : result;
    }
  }  // namespace

  enum class socket_e : int {
    video,  ///< Video
    audio  ///< Audio
  };

#pragma pack(push, 1)

  struct video_short_frame_header_t {
    uint8_t *payload() {
      return (uint8_t *) (this + 1);
    }

    std::uint8_t headerType;  // Always 0x01 for short headers

    // Sunshine extension
    // Frame processing latency, in 1/10 ms units
    //     zero when the frame is repeated or there is no backend implementation
    boost::endian::little_uint16_at frame_processing_latency;

    // Currently known values:
    // 1 = Normal P-frame
    // 2 = IDR-frame
    // 4 = P-frame with intra-refresh blocks
    // 5 = P-frame after reference frame invalidation
    std::uint8_t frameType;

    // Length of the final packet payload for codecs that cannot handle
    // zero padding, such as AV1 (Sunshine extension).
    boost::endian::little_uint16_at lastPayloadLen;

    std::uint8_t unknown[2];
  };

  static_assert(
    sizeof(video_short_frame_header_t) == 8,
    "Short frame header must be 8 bytes"
  );

  struct video_packet_raw_t {
    uint8_t *payload() {
      return (uint8_t *) (this + 1);
    }

    RTP_PACKET rtp;
    char reserved[4];

    NV_VIDEO_PACKET packet;
  };

  struct video_packet_enc_prefix_t {
    std::uint8_t iv[12];  // 12-byte IV is ideal for AES-GCM
    std::uint32_t frameNumber;
    std::uint8_t tag[16];
  };

  struct audio_packet_t {
    RTP_PACKET rtp;
  };

  struct control_header_v2 {
    std::uint16_t type;
    std::uint16_t payloadLength;

    uint8_t *payload() {
      return (uint8_t *) (this + 1);
    }
  };

  struct control_terminate_t {
    control_header_v2 header;

    std::uint32_t ec;
  };

  struct control_rumble_t {
    control_header_v2 header;

    std::uint32_t useless;

    std::uint16_t id;
    std::uint16_t lowfreq;
    std::uint16_t highfreq;
  };

  struct control_rumble_triggers_t {
    control_header_v2 header;

    std::uint16_t id;
    std::uint16_t left;
    std::uint16_t right;
  };

  struct control_set_motion_event_t {
    control_header_v2 header;

    std::uint16_t id;
    std::uint16_t reportrate;
    std::uint8_t type;
  };

  struct control_set_rgb_led_t {
    control_header_v2 header;

    std::uint16_t id;
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
  };

  struct control_adaptive_triggers_t {
    control_header_v2 header;

    std::uint16_t id;
    /**
     * 0x04 - Right trigger
     * 0x08 - Left trigger
     */
    std::uint8_t event_flags;
    std::uint8_t type_left;
    std::uint8_t type_right;
    std::uint8_t left[DS_EFFECT_PAYLOAD_SIZE];
    std::uint8_t right[DS_EFFECT_PAYLOAD_SIZE];
  };

  struct control_hdr_mode_t {
    control_header_v2 header;

    std::uint8_t enabled;

    // Sunshine protocol extension
    SS_HDR_METADATA metadata;
  };

  typedef struct control_encrypted_t {
    std::uint16_t encryptedHeaderType;  // Always LE 0x0001
    std::uint16_t length;  // sizeof(seq) + 16 byte tag + secondary header and data

    // seq is accepted as an arbitrary value in Moonlight
    std::uint32_t seq;  // Monotonically increasing sequence number (used as IV for AES-GCM)

    uint8_t *payload() {
      return (uint8_t *) (this + 1);
    }

    // encrypted control_header_v2 and payload data follow
  } *control_encrypted_p;

  struct audio_fec_packet_t {
    RTP_PACKET rtp;
    AUDIO_FEC_HEADER fecHeader;
  };

#pragma pack(pop)

  constexpr std::size_t round_to_pkcs7_padded(std::size_t size) {
    return ((size + 15) / 16) * 16;
  }

  constexpr std::size_t MAX_AUDIO_PACKET_SIZE = 1400;

  using audio_aes_t = std::array<char, round_to_pkcs7_padded(MAX_AUDIO_PACKET_SIZE)>;

  using av_session_id_t = std::variant<asio::ip::address, std::string>;  // IP address or SS-Ping-Payload from RTSP handshake
  using message_queue_t = std::shared_ptr<safe::queue_t<std::pair<udp::endpoint, std::string>>>;
  using message_queue_queue_t = std::shared_ptr<safe::queue_t<std::tuple<socket_e, av_session_id_t, message_queue_t>>>;

  // return bytes written on success
  // return -1 on error
  static inline int encode_audio(bool encrypted, const audio::buffer_t &plaintext, uint8_t *destination, crypto::aes_t &iv, crypto::cipher::cbc_t &cbc) {
    // If encryption isn't enabled
    if (!encrypted) {
      std::copy(std::begin(plaintext), std::end(plaintext), destination);
      return (int) plaintext.size();
    }

    return cbc.encrypt(std::string_view {(char *) std::begin(plaintext), plaintext.size()}, destination, &iv);
  }

  static inline void while_starting_do_nothing(std::atomic<session::state_e> &state) {
    while (state.load(std::memory_order_acquire) == session::state_e::STARTING) {
      std::this_thread::sleep_for(1ms);
    }
  }

  class control_server_t {
  public:
    int bind(net::af_e address_family, std::uint16_t port) {
      _host = net::host_create(address_family, _addr, port);

      return !(bool) _host;
    }

    // Get session associated with address.
    // If none are found, try to find a session not yet claimed. (It will be marked by a port of value 0
    // If none of those are found, return nullptr
    session_t *get_session(const net::peer_t peer, uint32_t connect_data);

    // Circular dependency:
    //   iterate refers to session
    //   session refers to broadcast_ctx_t
    //   broadcast_ctx_t refers to control_server_t
    // Therefore, iterate is implemented further down the source file
    void iterate(std::chrono::milliseconds timeout);

    /**
     * @brief Call the handler for a given control stream message.
     * @param type The message type.
     * @param session The session the message was received on.
     * @param payload The payload of the message.
     * @param reinjected `true` if this message is being reprocessed after decryption.
     */
    void call(std::uint16_t type, session_t *session, const std::string_view &payload, bool reinjected);

    void map(uint16_t type, std::function<void(session_t *, const std::string_view &)> cb) {
      _map_type_cb.emplace(type, std::move(cb));
    }

    int send(const std::string_view &payload, net::peer_t peer) {
      auto packet = enet_packet_create(payload.data(), payload.size(), ENET_PACKET_FLAG_RELIABLE);
      if (enet_peer_send(peer, 0, packet)) {
        enet_packet_destroy(packet);

        return -1;
      }

      return 0;
    }

    void flush() {
      enet_host_flush(_host.get());
    }

    // Callbacks
    std::unordered_map<std::uint16_t, std::function<void(session_t *, const std::string_view &)>> _map_type_cb;

    // Clipboard notifications are produced by the Windows message thread and
    // consumed by the control thread so ENet remains single-threaded.
    std::shared_ptr<safe::queue_t<clipboard_update_t>> clipboard_queue = std::make_shared<safe::queue_t<clipboard_update_t>>(32);

    // All active sessions (including those still waiting for a peer to connect)
    sync_util::sync_t<std::vector<session_t *>> _sessions;

    // ENet peer to session mapping for sessions with a peer connected
    sync_util::sync_t<std::map<net::peer_t, session_t *>> _peer_to_session;

    ENetAddress _addr;
    net::host_t _host;
  };

  struct broadcast_ctx_t {
    message_queue_queue_t message_queue_queue;

    std::thread recv_thread;
    std::thread video_thread;
    std::thread audio_thread;
    std::thread control_thread;

    asio::io_context io_context;

    udp::socket video_sock {io_context};
    udp::socket audio_sock {io_context};

    control_server_t control_server;
  };

  struct session_t {
    config_t config;

    // Shared with the encoder and video sender so a client can change the
    // target bitrate without tearing down the RTSP/session state.
    std::shared_ptr<std::atomic<int>> adaptive_bitrate_kbps;
    int adaptive_bitrate_ceiling_kbps = 0;
    std::mutex adaptive_bitrate_mutex;
    adaptive_bitrate_policy_state_t adaptive_bitrate_policy;

    safe::mail_t mail;

    std::shared_ptr<input::input_t> input;

    std::thread audioThread;
    std::thread videoThread;

    std::chrono::steady_clock::time_point pingTimeout;

    safe::shared_t<broadcast_ctx_t>::ptr_t broadcast_ref;

    boost::asio::ip::address localAddress;

    struct video_track_state_t {
      int lowseq = 0;
      std::optional<crypto::cipher::gcm_t> cipher;
      std::uint64_t gcm_iv_counter = 0;
      std::chrono::steady_clock::time_point fec_fallback_until = std::chrono::steady_clock::time_point::min();
    };

    struct {
      std::string ping_payload;
      udp::endpoint peer;
      std::array<video_track_state_t, dual_display_launch::display_count> tracks;

      safe::mail_raw_t::event_t<bool> idr_events;
      safe::mail_raw_t::event_t<std::pair<int64_t, int64_t>> invalidate_ref_frames_events;

      std::unique_ptr<platf::deinit_t> qos;
    } video;

    struct {
      crypto::cipher::cbc_t cipher;
      std::string ping_payload;

      std::uint16_t sequenceNumber;
      // avRiKeyId == util::endian::big(First (sizeof(avRiKeyId)) bytes of launch_session->iv)
      std::uint32_t avRiKeyId;
      std::uint32_t timestamp;
      udp::endpoint peer;

      util::buffer_t<char> shards;
      util::buffer_t<uint8_t *> shards_p;

      audio_fec_packet_t fec_packet;
      std::unique_ptr<platf::deinit_t> qos;
    } audio;

    struct {
      crypto::cipher::gcm_t cipher;
      crypto::aes_t legacy_input_enc_iv;  // Only used when the client doesn't support full control stream encryption
      crypto::aes_t incoming_iv;
      crypto::aes_t outgoing_iv;

      std::uint32_t connect_data;  // Used for new clients with ML_FF_SESSION_ID_V1
      std::string expected_peer_address;  // Only used for legacy clients without ML_FF_SESSION_ID_V1

      net::peer_t peer;
      std::uint32_t seq;

      std::mutex send_mutex;

      platf::feedback_queue_t feedback_queue;
      safe::mail_raw_t::event_t<video::hdr_info_t> hdr_queue;
      safe::mail_raw_t::queue_t<input_timing::completed_input_t> input_timing_ack_queue;
    } control;

    std::uint32_t launch_session_id;
    std::string client_cert;
    std::string device_name;
    std::string device_uuid;
    std::chrono::steady_clock::time_point started_at;
    crypto::PERM permission = crypto::PERM::_all;
    std::list<crypto::command_entry_t> do_cmds;
    std::list<crypto::command_entry_t> undo_cmds;

    std::mutex clipboard_mutex;
    std::uint64_t clipboard_last_applied_hash = 0;
    std::uint64_t clipboard_last_observed_hash = 0;
    std::uint64_t clipboard_suppress_hash = 0;

    std::vector<std::uint8_t> clipboard_image_reassembly;
    std::uint32_t clipboard_image_reassembly_total_length = 0;
    std::uint32_t clipboard_image_reassembly_received_length = 0;
    std::uint64_t clipboard_image_reassembly_origin_id = 0;
    std::uint64_t clipboard_image_reassembly_content_hash = 0;
    std::uint64_t clipboard_image_reassembly_transfer_id = 0;
    std::uint64_t clipboard_image_last_applied_hash = 0;
    std::uint64_t clipboard_image_last_observed_hash = 0;
    std::uint64_t clipboard_image_suppress_hash = 0;

    std::unique_ptr<client_mic::receiver_t> client_mic;

    safe::mail_raw_t::event_t<bool> shutdown_event;
    safe::signal_t controlEnd;

    std::atomic<session::state_e> state;
  };

  /**
   * First part of cipher must be struct of type control_encrypted_t
   *
   * returns empty string_view on failure
   * returns string_view pointing to payload data
   */
  template<std::size_t max_payload_size>
  static inline std::string_view encode_control(session_t *session, const std::string_view &plaintext, std::array<std::uint8_t, max_payload_size> &tagged_cipher) {
    static_assert(
      max_payload_size >= sizeof(control_encrypted_t) + sizeof(crypto::cipher::tag_size),
      "max_payload_size >= sizeof(control_encrypted_t) + sizeof(crypto::cipher::tag_size)"
    );

    if (session->config.controlProtocolType != 13) {
      return plaintext;
    }

    auto seq = session->control.seq++;

    auto &iv = session->control.outgoing_iv;
    if (session->config.encryptionFlagsEnabled & SS_ENC_CONTROL_V2) {
      // We use the deterministic IV construction algorithm specified in NIST SP 800-38D
      // Section 8.2.1. The sequence number is our "invocation" field and the 'CH' in the
      // high bytes is the "fixed" field. Because each client provides their own unique
      // key, our values in the fixed field need only uniquely identify each independent
      // use of the client's key with AES-GCM in our code.
      //
      // The sequence number is 32 bits long which allows for 2^32 control stream messages
      // to be sent to each client before the IV repeats.
      iv.resize(12);
      std::copy_n((uint8_t *) &seq, sizeof(seq), std::begin(iv));
      iv[10] = 'H';  // Host originated
      iv[11] = 'C';  // Control stream
    } else {
      // Nvidia's old style encryption uses a 16-byte IV
      iv.resize(16);

      iv[0] = (std::uint8_t) seq;
    }

    auto packet = (control_encrypted_p) tagged_cipher.data();

    auto bytes = session->control.cipher.encrypt(plaintext, packet->payload(), &iv);
    if (bytes <= 0) {
      BOOST_LOG(error) << "Couldn't encrypt control data"sv;
      return {};
    }

    if (!control_stream_plaintext_fits(static_cast<std::size_t>(bytes))) {
      BOOST_LOG(warning) << "Encrypted control payload exceeds the uint16 length limit"sv;
      return {};
    }

    std::uint16_t packet_length = bytes + crypto::cipher::tag_size + sizeof(control_encrypted_t::seq);

    packet->encryptedHeaderType = util::endian::little(0x0001);
    packet->length = util::endian::little(packet_length);
    packet->seq = util::endian::little(seq);

    return std::string_view {(char *) tagged_cipher.data(), packet_length + sizeof(control_encrypted_t) - sizeof(control_encrypted_t::seq)};
  }

  static int send_host_clipboard_update(control_server_t *server, session_t *session, const std::string_view &text) {
    if (!config::sunshineseat.apollo_clipboard_enabled || session->config.controlProtocolType != 13 || session->state.load(std::memory_order_acquire) != session::state_e::RUNNING || !session->control.peer || !(session->permission & crypto::PERM::clipboard_read)) {
      return 0;
    }

    auto message = encode_clipboard_message(CLIPBOARD_ORIGIN_HOST, clipboard_origin_id(), text);
    if (message.empty()) {
      BOOST_LOG(warning) << "Clipboard update ignored: text is not valid UTF-8 or exceeds the size limit"sv;
      return -1;
    }

    std::string plaintext(sizeof(control_header_v2) + message.size(), '\0');
    auto header = reinterpret_cast<control_header_v2 *>(plaintext.data());
    header->type = packetTypes[IDX_CLIPBOARD_HOST_TO_CLIENT];
    header->payloadLength = static_cast<std::uint16_t>(message.size());
    std::copy(message.begin(), message.end(), plaintext.begin() + sizeof(control_header_v2));

    constexpr std::size_t max_plaintext_size = sizeof(control_header_v2) + CLIPBOARD_MESSAGE_HEADER_SIZE + CLIPBOARD_MAX_TEXT_BYTES;
    constexpr std::size_t max_encrypted_size = sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(max_plaintext_size) + crypto::cipher::tag_size;
    std::array<std::uint8_t, max_encrypted_size> encrypted_payload;

    std::lock_guard lock {session->control.send_mutex};
    auto payload = encode_control(session, plaintext, encrypted_payload);
    if (payload.empty()) {
      return -1;
    }

    return server->send(payload, session->control.peer);
  }

  static int send_host_clipboard_image_update(control_server_t *server, session_t *session, const std::vector<std::uint8_t> &image) {
    if (!config::sunshineseat.apollo_clipboard_enabled || session->config.controlProtocolType != 13 || session->state.load(std::memory_order_acquire) != session::state_e::RUNNING || !session->control.peer || !(session->permission & crypto::PERM::clipboard_read) || image.size() > CLIPBOARD_MAX_IMAGE_BYTES || !clipboard_valid_bmp(std::string_view {(const char *) image.data(), image.size()})) {
      return 0;
    }

    const auto total_length = static_cast<std::uint32_t>(image.size());
    const auto content_hash = clipboard_image_hash(std::string_view {(const char *) image.data(), image.size()});
    const auto transfer_id = clipboard_random_id();
    constexpr std::size_t max_plaintext_size = sizeof(control_header_v2) + CLIPBOARD_IMAGE_MESSAGE_HEADER_SIZE + CLIPBOARD_IMAGE_MAX_CHUNK_BYTES;
    constexpr std::size_t max_encrypted_size = sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(max_plaintext_size) + crypto::cipher::tag_size;
    std::array<std::uint8_t, max_encrypted_size> encrypted_payload;

    std::lock_guard lock {session->control.send_mutex};
    for (std::size_t offset = 0; offset < image.size();) {
      const auto chunk_length = std::min(CLIPBOARD_IMAGE_MAX_CHUNK_BYTES, image.size() - offset);
      auto message = encode_clipboard_image_chunk(
        CLIPBOARD_ORIGIN_HOST,
        clipboard_origin_id(),
        content_hash,
        transfer_id,
        total_length,
        static_cast<std::uint32_t>(offset),
        std::string_view {(const char *) image.data() + offset, chunk_length}
      );
      if (message.empty()) {
        return -1;
      }

      std::string plaintext(sizeof(control_header_v2) + message.size(), '\0');
      auto header = reinterpret_cast<control_header_v2 *>(plaintext.data());
      header->type = packetTypes[IDX_CLIPBOARD_IMAGE_HOST_TO_CLIENT];
      header->payloadLength = static_cast<std::uint16_t>(message.size());
      std::copy(message.begin(), message.end(), plaintext.begin() + sizeof(control_header_v2));

      auto payload = encode_control(session, plaintext, encrypted_payload);
      if (payload.empty() || server->send(payload, session->control.peer) != 0) {
        return -1;
      }
      offset += chunk_length;
    }

    return 0;
  }

  static void broadcast_host_clipboard_update(control_server_t *server, const std::string_view &text) {
    if (!config::sunshineseat.apollo_clipboard_enabled || text.size() > CLIPBOARD_MAX_TEXT_BYTES) {
      return;
    }

    auto hash = clipboard_text_hash(text);
    auto lg = server->_sessions.lock();
    for (auto *session : *server->_sessions) {
      {
        std::lock_guard clipboard_lock {session->clipboard_mutex};
        if (session->clipboard_suppress_hash == hash) {
          session->clipboard_suppress_hash = 0;
          session->clipboard_last_applied_hash = hash;
          session->clipboard_last_observed_hash = hash;
          continue;
        }

        if (session->clipboard_last_observed_hash == hash) {
          continue;
        }
        session->clipboard_last_observed_hash = hash;
      }

      send_host_clipboard_update(server, session, text);
    }
  }

  static void broadcast_host_clipboard_image_update(control_server_t *server, const std::vector<std::uint8_t> &image) {
    if (!config::sunshineseat.apollo_clipboard_enabled || image.size() > CLIPBOARD_MAX_IMAGE_BYTES || !clipboard_valid_bmp(std::string_view {(const char *) image.data(), image.size()})) {
      return;
    }

    auto hash = clipboard_image_hash(std::string_view {(const char *) image.data(), image.size()});
    auto lg = server->_sessions.lock();
    for (auto *session : *server->_sessions) {
      {
        std::lock_guard clipboard_lock {session->clipboard_mutex};
        if (session->clipboard_image_suppress_hash == hash) {
          session->clipboard_image_suppress_hash = 0;
          session->clipboard_image_last_applied_hash = hash;
          session->clipboard_image_last_observed_hash = hash;
          continue;
        }

        if (session->clipboard_image_last_observed_hash == hash) {
          continue;
        }
        session->clipboard_image_last_observed_hash = hash;
      }

      send_host_clipboard_image_update(server, session, image);
    }
  }

#ifdef _WIN32
  class clipboard_monitor_t {
  public:
    void start(std::function<void()> callback) {
      _callback = std::move(callback);
      _stop.store(false, std::memory_order_release);
      _thread = std::thread {[this] {
        run();
      }};
    }

    void stop() {
      _stop.store(true, std::memory_order_release);
      auto thread_id = _thread_id.load(std::memory_order_acquire);
      if (thread_id != 0) {
        PostThreadMessageW(thread_id, WM_QUIT, 0, 0);
      }
      if (_thread.joinable()) {
        _thread.join();
      }
    }

    ~clipboard_monitor_t() {
      stop();
    }

  private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
      auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
      auto *monitor = reinterpret_cast<clipboard_monitor_t *>(GetWindowLongPtrW(window, GWLP_USERDATA));
      if (message == WM_NCCREATE && create != nullptr) {
        monitor = static_cast<clipboard_monitor_t *>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(monitor));
      }

      if (message == WM_CLIPBOARDUPDATE && monitor != nullptr && !monitor->_stop.load(std::memory_order_acquire) && monitor->_callback) {
        monitor->_callback();
      }

      return DefWindowProcW(window, message, wparam, lparam);
    }

    void run() {
      _thread_id.store(GetCurrentThreadId(), std::memory_order_release);
      MSG message {};
      PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

      auto instance = GetModuleHandleW(nullptr);
      WNDCLASSEXW window_class {};
      window_class.cbSize = sizeof(window_class);
      window_class.hInstance = instance;
      window_class.lpfnWndProc = &clipboard_monitor_t::window_proc;
      window_class.lpszClassName = L"SunshineSeatClipboardMonitor";
      RegisterClassExW(&window_class);

      auto window = CreateWindowExW(
        0,
        window_class.lpszClassName,
        L"SunshineSeatClipboardMonitor",
        0,
        0,
        0,
        0,
        0,
        HWND_MESSAGE,
        nullptr,
        instance,
        this
      );

      if (window != nullptr) {
        _window = window;
        AddClipboardFormatListener(window);
      }

      while (!_stop.load(std::memory_order_acquire) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }

      if (_window != nullptr) {
        RemoveClipboardFormatListener(_window);
        DestroyWindow(_window);
        _window = nullptr;
      }
      UnregisterClassW(window_class.lpszClassName, instance);
      _thread_id.store(0, std::memory_order_release);
    }

    std::function<void()> _callback;
    std::thread _thread;
    std::atomic<bool> _stop {false};
    std::atomic<DWORD> _thread_id {0};
    HWND _window = nullptr;
  };
#endif

  int start_broadcast(broadcast_ctx_t &ctx);
  void end_broadcast(broadcast_ctx_t &ctx);

  static auto broadcast = safe::make_shared<broadcast_ctx_t>(start_broadcast, end_broadcast);

  session_t *control_server_t::get_session(const net::peer_t peer, uint32_t connect_data) {
    {
      // Fast path - look up existing session by peer
      auto lg = _peer_to_session.lock();
      auto it = _peer_to_session->find(peer);
      if (it != _peer_to_session->end()) {
        return it->second;
      }
    }

    // Slow path - process new session
    TUPLE_2D(peer_port, peer_addr, platf::from_sockaddr_ex((sockaddr *) &peer->address.address));
    auto lg = _sessions.lock();
    for (auto pos = std::begin(*_sessions); pos != std::end(*_sessions); ++pos) {
      auto session_p = *pos;

      // Skip sessions that are already established
      if (session_p->control.peer) {
        continue;
      }

      // Identify the connection by the unique connect data if the client supports it.
      // Only fall back to IP address matching for clients without session ID support.
      if (session_p->config.mlFeatureFlags & ML_FF_SESSION_ID_V1) {
        if (session_p->control.connect_data != connect_data) {
          continue;
        } else {
          BOOST_LOG(debug) << "Initialized new control stream session by connect data match [v2]"sv;
        }
      } else {
        if (session_p->control.expected_peer_address != peer_addr) {
          continue;
        } else {
          BOOST_LOG(debug) << "Initialized new control stream session by IP address match [v1]"sv;
        }
      }

      // Once the control stream connection is established, RTSP session state can be torn down
      rtsp_stream::launch_session_clear(session_p->launch_session_id);

      session_p->control.peer = peer;

      // Use the local address from the control connection as the source address
      // for other communications to the client. This is necessary to ensure
      // proper routing on multi-homed hosts.
      auto local_address = platf::from_sockaddr((sockaddr *) &peer->localAddress.address);
      try {
        session_p->localAddress = boost::asio::ip::make_address(local_address);
      } catch (const boost::system::system_error &e) {
        BOOST_LOG(error) << "boost::system::system_error in address parsing: " << e.what() << " (code: " << e.code() << ")"sv;
        throw;
      }

      BOOST_LOG(debug) << "Control local address ["sv << local_address << ']';
      BOOST_LOG(debug) << "Control peer address ["sv << peer_addr << ':' << peer_port << ']';

      // Insert this into the map for O(1) lookups in the future
      auto ptslg = _peer_to_session.lock();
      _peer_to_session->emplace(peer, session_p);
      return session_p;
    }

    return nullptr;
  }

  /**
   * @brief Call the handler for a given control stream message.
   * @param type The message type.
   * @param session The session the message was received on.
   * @param payload The payload of the message.
   * @param reinjected `true` if this message is being reprocessed after decryption.
   */
  void control_server_t::call(std::uint16_t type, session_t *session, const std::string_view &payload, bool reinjected) {
    // If we are using the encrypted control stream protocol, drop any messages that come off the wire unencrypted
    if (session->config.controlProtocolType == 13 && !reinjected && type != packetTypes[IDX_ENCRYPTED]) {
      BOOST_LOG(error) << "Dropping unencrypted message on encrypted control stream: "sv << util::hex(type).to_string_view();
      return;
    }

    auto cb = _map_type_cb.find(type);
    if (cb == std::end(_map_type_cb)) {
      BOOST_LOG(debug)
        << "type [Unknown] { "sv << util::hex(type).to_string_view() << " }"sv << std::endl
        << "---data---"sv << std::endl
        << util::hex_vec(payload) << std::endl
        << "---end data---"sv;
    } else {
      cb->second(session, payload);
    }
  }

  void control_server_t::iterate(std::chrono::milliseconds timeout) {
    ENetEvent event;
    auto res = enet_host_service(_host.get(), &event, (enet_uint32) timeout.count());

    if (res > 0) {
      auto session = get_session(event.peer, event.data);
      if (!session) {
        BOOST_LOG(warning) << "Rejected connection from ["sv << platf::from_sockaddr((sockaddr *) &event.peer->address.address) << "]: it's not properly set up"sv;
        enet_peer_disconnect_now(event.peer, 0);

        return;
      }

      session->pingTimeout = std::chrono::steady_clock::now() + config::stream.ping_timeout;

      switch (event.type) {
        case ENET_EVENT_TYPE_RECEIVE:
          {
            net::packet_t packet {event.packet};

            auto type = *(std::uint16_t *) packet->data;
            std::string_view payload {(char *) packet->data + sizeof(type), packet->dataLength - sizeof(type)};

            call(type, session, payload, false);
          }
          break;
        case ENET_EVENT_TYPE_CONNECT:
          BOOST_LOG(info) << "CLIENT CONNECTED"sv;
          break;
        case ENET_EVENT_TYPE_DISCONNECT:
          BOOST_LOG(info) << "CLIENT DISCONNECTED"sv;
          // No more clients to send video data to ^_^
          if (session->state == session::state_e::RUNNING) {
            session::stop(*session);
          }
          break;
        case ENET_EVENT_TYPE_NONE:
          break;
      }
    }
  }

  size_t bounded_video_fec_parity_shards(
    size_t data_shards,
    size_t fec_percentage,
    size_t min_parity_shards,
    size_t parity_capacity_floor
  ) {
    if (data_shards == 0 || fec_percentage == 0) {
      return 0;
    }

    const auto requested_parity_shards = (data_shards * fec_percentage + 99) / 100;
    const auto maximum_parity_shards = std::max(parity_capacity_floor, (data_shards * 8) / 100);
    return std::min(
      maximum_parity_shards,
      std::max({requested_parity_shards, min_parity_shards, parity_capacity_floor})
    );
  }

  size_t dual_display_video_parity_capacity_floor(
    const bool dual_display_video_tracks,
    const size_t data_shards
  ) {
    if (!dual_display_video_tracks) {
      return 1U;
    }
    return std::min<size_t>(4U, std::max<size_t>(1U, data_shards));
  }

  size_t bounded_video_fec_parity_shards(
    size_t data_shards,
    size_t fec_percentage,
    size_t min_parity_shards
  ) {
    return bounded_video_fec_parity_shards(data_shards, fec_percentage, min_parity_shards, 1);
  }

  namespace fec {
    using rs_t = util::safe_ptr<reed_solomon, [](reed_solomon *rs) {
      reed_solomon_release(rs);
    }>;

    struct fec_t {
      size_t data_shards;
      size_t nr_shards;
      size_t percentage;

      size_t blocksize;
      size_t prefixsize;
      util::buffer_t<char> shards;
      util::buffer_t<char> headers;
      util::buffer_t<uint8_t *> shards_p;

      std::vector<platf::buffer_descriptor_t> payload_buffers;

      char *data(size_t el) {
        return (char *) shards_p[el];
      }

      char *prefix(size_t el) {
        return prefixsize ? &headers[el * prefixsize] : nullptr;
      }

      size_t size() const {
        return nr_shards;
      }
    };

    static fec_t encode(
      const std::string_view &payload,
      size_t blocksize,
      size_t fecpercentage,
      size_t minparityshards,
      size_t prefixsize,
      size_t parity_capacity_floor = 1
    ) {
      auto payload_size = payload.size();

      auto pad = payload_size % blocksize != 0;

      auto aligned_data_shards = payload_size / blocksize;
      auto data_shards = aligned_data_shards + (pad ? 1 : 0);
      auto parity_shards = bounded_video_fec_parity_shards(
        data_shards,
        fecpercentage,
        minparityshards,
        parity_capacity_floor
      );
      if (parity_shards != 0) {
        const auto actual_fec_percentage = (100 * parity_shards) / data_shards;
        if (actual_fec_percentage != fecpercentage || parity_shards < minparityshards) {
          BOOST_LOG(verbose) << "Bounding FEC parity to "sv << parity_shards << " shards ("sv << actual_fec_percentage << " percent)"sv << std::endl;
        }
        fecpercentage = actual_fec_percentage;
      }

      auto nr_shards = data_shards + parity_shards;

      // If we need to store a zero-padded data shard, allocate that first to
      // to keep the shards in order and reduce buffer fragmentation
      auto parity_shard_offset = pad ? 1 : 0;
      util::buffer_t<char> shards {(parity_shard_offset + parity_shards) * blocksize};
      util::buffer_t<uint8_t *> shards_p {nr_shards};
      std::vector<platf::buffer_descriptor_t> payload_buffers;
      payload_buffers.reserve(2);

      // Point into the payload buffer for all except the final padded data shard
      auto next = std::begin(payload);
      for (auto x = 0; x < aligned_data_shards; ++x) {
        shards_p[x] = (uint8_t *) next;
        next += blocksize;
      }
      payload_buffers.emplace_back(std::begin(payload), aligned_data_shards * blocksize);

      // If the last data shard needs to be zero-padded, we must use the shards buffer
      if (pad) {
        shards_p[aligned_data_shards] = (uint8_t *) &shards[0];

        // GCC doesn't figure out that std::copy_n() can be replaced with memcpy() here
        // and ends up compiling a horribly slow element-by-element copy loop, so we
        // help it by using memcpy()/memset() directly.
        auto copy_len = std::min<size_t>(blocksize, std::end(payload) - next);
        std::memcpy(shards_p[aligned_data_shards], next, copy_len);
        if (copy_len < blocksize) {
          // Zero any additional space after the end of the payload
          std::memset(shards_p[aligned_data_shards] + copy_len, 0, blocksize - copy_len);
        }
      }

      // Add a payload buffer describing the shard buffer
      payload_buffers.emplace_back(std::begin(shards), shards.size());

      if (fecpercentage != 0) {
        // Point into our allocated buffer for the parity shards
        for (auto x = 0; x < parity_shards; ++x) {
          shards_p[data_shards + x] = (uint8_t *) &shards[(parity_shard_offset + x) * blocksize];
        }

        // The shard shape is normally stable for a 1080p60 stream. Reuse the
        // codec matrix on this send thread so every frame does not allocate
        // and initialize a new Reed-Solomon codec under the pacing budget.
        thread_local rs_t cached_rs;
        thread_local size_t cached_data_shards = 0;
        thread_local size_t cached_parity_shards = 0;
        if (cached_rs.get() == nullptr || cached_data_shards != data_shards || cached_parity_shards != parity_shards) {
          cached_rs.reset(reed_solomon_new((int) data_shards, (int) parity_shards));
          cached_data_shards = data_shards;
          cached_parity_shards = parity_shards;
        }

        reed_solomon_encode(cached_rs.get(), shards_p.begin(), (int) nr_shards, (int) blocksize);
      }

      return {
        data_shards,
        nr_shards,
        fecpercentage,
        blocksize,
        prefixsize,
        std::move(shards),
        util::buffer_t<char> {nr_shards * prefixsize},
        std::move(shards_p),
        std::move(payload_buffers),
      };
    }
  }  // namespace fec

  /**
   * @brief Combines two buffers and inserts new buffers at each slice boundary of the result.
   * @param insert_size The number of bytes to insert.
   * @param slice_size The number of bytes between insertions.
   * @param data1 The first data buffer.
   * @param data2 The second data buffer.
   */
  std::vector<uint8_t> concat_and_insert(uint64_t insert_size, uint64_t slice_size, const std::string_view &data1, const std::string_view &data2) {
    auto data_size = data1.size() + data2.size();
    auto pad = data_size % slice_size != 0;
    auto elements = data_size / slice_size + (pad ? 1 : 0);

    std::vector<uint8_t> result;
    result.resize(elements * insert_size + data_size);

    auto next = std::begin(data1);
    auto end = std::end(data1);
    for (auto x = 0; x < elements; ++x) {
      void *p = &result[x * (insert_size + slice_size)];

      // For the last iteration, only copy to the end of the data
      if (x == elements - 1) {
        slice_size = data_size - (x * slice_size);
      }

      // Test if this slice will extend into the next buffer
      if (next + slice_size > end) {
        // Copy the first portion from the first buffer
        auto copy_len = end - next;
        std::copy(next, end, (char *) p + insert_size);

        // Copy the remaining portion from the second buffer
        next = std::begin(data2);
        end = std::end(data2);
        std::copy(next, next + (slice_size - copy_len), (char *) p + copy_len + insert_size);
        next += slice_size - copy_len;
      } else {
        std::copy(next, next + slice_size, (char *) p + insert_size);
        next += slice_size;
      }
    }

    return result;
  }

  std::pair<size_t, size_t> calculate_video_send_pacing(size_t blocksize, int target_bitrate_kbps, int fec_percentage) {
    if (blocksize == 0) {
      return {1, 1};
    }

    if (target_bitrate_kbps <= 0) {
      auto packets_in_1ms = std::max<size_t>(1, std::giga::num * 80 / 100 / 1000 / blocksize / 8);
      auto send_batch_size = std::min<size_t>(64, 64 * 1024 / blocksize);
      send_batch_size = std::max<size_t>(1, send_batch_size);
      return {packets_in_1ms, send_batch_size};
    }

    auto fec_multiplier = std::max(100, 100 + fec_percentage);
    auto effective_bitrate_bps = static_cast<uint64_t>(target_bitrate_kbps) * 1000ULL * static_cast<uint64_t>(fec_multiplier) / 100ULL;
    auto bytes_per_ms = (effective_bitrate_bps + 7999ULL) / 8000ULL;
    auto packets_in_1ms = std::max<size_t>(1, (bytes_per_ms + blocksize - 1) / blocksize);

    // Keep each send batch within the pacing window to avoid large WAN-facing packet bursts.
    auto send_batch_size = std::min<size_t>(64, packets_in_1ms);
    send_batch_size = std::max<size_t>(1, send_batch_size);

    return {packets_in_1ms, send_batch_size};
  }

  int effective_video_send_bitrate_kbps(int requested_bitrate_kbps, int configured_max_bitrate_kbps) {
    if (configured_max_bitrate_kbps > 0 && requested_bitrate_kbps > configured_max_bitrate_kbps) {
      return configured_max_bitrate_kbps;
    }
    return requested_bitrate_kbps;
  }

  bool should_enter_video_fec_fallback(const std::chrono::steady_clock::duration elapsed) {
    constexpr auto fec_fallback_threshold = std::chrono::milliseconds(50);
    return elapsed >= fec_fallback_threshold;
  }

  bool should_drop_video_packet(const bool broadcast_shutdown, const bool session_running) {
    return broadcast_shutdown || !session_running;
  }

  bool should_release_virtual_display_allocation(
    const std::size_t active_sessions,
    const std::size_t pending_launch_sessions
  ) {
    return active_sessions == 0 && pending_launch_sessions == 0;
  }

  int effective_video_fec_percentage(int configured_fec_percentage, bool fallback_active) {
    // The configuration parser requires at least one percent, but the
    // low-latency LAN profile uses 1 as its explicit no-parity sentinel.
    // Otherwise a client's minRequiredFecPackets can turn that setting into
    // multiple Reed-Solomon shards and stall the video send thread.
    return fallback_active || configured_fec_percentage <= 1 ? 0 : std::clamp(configured_fec_percentage, 0, 255);
  }

  int effective_video_keyframe_fec_percentage(int configured_fec_percentage, bool fallback_active) {
    if (fallback_active) {
      return 0;
    }

    const auto effective_fec_percentage = effective_video_fec_percentage(configured_fec_percentage, false);
    if (effective_fec_percentage == 0) {
      // The parser requires at least one percent, so 1 is the low-latency
      // profile's explicit no-parity sentinel. Do not reintroduce parity on
      // IDR frames: a large keyframe can make Reed-Solomon work block the
      // video send thread long enough to cause a visible freeze.
      return 0;
    }

    // Keep explicit FEC settings protective on IDR frames. Losing one IDR
    // packet otherwise leaves the decoder waiting until the next reference
    // frame, which is a visible freeze even when the host encoder and sender
    // remain healthy.
    // fec::encode bounds the resulting parity to at most eight percent of the
    // data shards, so this floor cannot create an unbounded keyframe burst.
    constexpr auto keyframe_fec_percentage_floor = 8;
    return std::max(
      keyframe_fec_percentage_floor,
      effective_fec_percentage
    );
  }

  std::chrono::steady_clock::duration calculate_video_send_frame_budget(size_t frame_packets_sent, size_t packets_per_ms) {
    if (frame_packets_sent == 0 || packets_per_ms == 0) {
      return std::chrono::steady_clock::duration::zero();
    }

    const auto budget_ms = (frame_packets_sent + packets_per_ms - 1) / packets_per_ms;
    return std::chrono::milliseconds(budget_ms);
  }

  size_t calculate_video_send_frame_burst_packets_per_ms(size_t sustained_packets_per_ms, size_t frame_packets) {
    if (sustained_packets_per_ms == 0 || frame_packets == 0) {
      return sustained_packets_per_ms;
    }

    // A large IDR/RFI frame must reach the client promptly. Pacing every
    // packet at a low sustained WAN rate turns a 180-packet frame into a
    // 180-ms freeze. Keep the long-term schedule on the sustained rate, but
    // send an individual frame over a bounded 16-ms window. Eight packets/ms
    // caps each microburst at a small batch instead of restoring unbounded
    // keyframe bursts.
    constexpr size_t frame_pacing_window_ms = 16;
    constexpr size_t maximum_burst_packets_per_ms = 8;
    if (sustained_packets_per_ms >= maximum_burst_packets_per_ms) {
      return sustained_packets_per_ms;
    }
    const auto packets_for_frame_window = (frame_packets + frame_pacing_window_ms - 1) / frame_pacing_window_ms;
    return std::min(std::max(sustained_packets_per_ms, packets_for_frame_window), maximum_burst_packets_per_ms);
  }

  size_t calculate_video_send_frame_batch_size(size_t packets_per_ms) {
    if (packets_per_ms == 0) {
      return 0;
    }

    // Windows timer wakeups are not precise enough to pace an 8-packet burst
    // one millisecond at a time. Keep the same average packet rate, but issue
    // no more than a four-millisecond bounded burst to avoid multiplying timer
    // oversleep into a visible frame stall.
    constexpr size_t batch_pacing_window_ms = 4;
    constexpr size_t maximum_batch_packets = 32;
    return std::min(packets_per_ms * batch_pacing_window_ms, maximum_batch_packets);
  }

  size_t calculate_video_send_frame_batch_size(
    const size_t packets_per_ms,
    const bool dual_display_video_tracks
  ) {
    const auto batch_size = calculate_video_send_frame_batch_size(packets_per_ms);
    return dual_display_video_tracks ? std::min<size_t>(batch_size, 4U) : batch_size;
  }

  size_t calculate_video_send_frame_batch_size_for_frame(
    const size_t packets_per_ms,
    const bool dual_display_video_tracks,
    const bool keyframe
  ) {
    const auto batch_size = calculate_video_send_frame_batch_size(
      packets_per_ms,
      dual_display_video_tracks
    );

    // Keyframes are the one frame type where a bounded timer oversleep can
    // turn otherwise healthy pacing into a loss burst at the client.
    constexpr size_t maximum_keyframe_batch_packets = 2;
    return keyframe ? std::min(batch_size, maximum_keyframe_batch_packets) : batch_size;
  }

  std::chrono::steady_clock::time_point calculate_video_pacing_next_frame_start(
    std::chrono::steady_clock::time_point scheduled_start,
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::duration maximum_interframe_delay
  ) {
    if (maximum_interframe_delay <= std::chrono::steady_clock::duration::zero()) {
      return scheduled_start;
    }

    return std::min(scheduled_start, now + maximum_interframe_delay);
  }

  std::chrono::steady_clock::time_point calculate_video_pacing_frame_start(
    std::chrono::steady_clock::time_point scheduled_start,
    std::chrono::steady_clock::time_point now
  ) {
    // A new frame must never begin in the past, but a stale future schedule
    // must not turn into an unbounded series of timer waits either. Keep the
    // same short interframe bound used when advancing the next-frame cursor.
    constexpr auto max_video_pacing_interframe_delay = 4ms;
    return std::clamp(
      scheduled_start,
      now,
      now + max_video_pacing_interframe_delay
    );
  }

  size_t calculate_video_send_catch_up_credit(
    std::chrono::steady_clock::duration late_by,
    size_t packets_per_ms,
    std::chrono::steady_clock::duration maximum_credit
  ) {
    if (late_by <= std::chrono::steady_clock::duration::zero() || packets_per_ms == 0 || maximum_credit <= std::chrono::steady_clock::duration::zero()) {
      return 0;
    }

    const auto bounded_lateness = std::min(late_by, maximum_credit);
    const auto bounded_credit_ms = std::chrono::duration_cast<std::chrono::milliseconds>(bounded_lateness).count();
    return static_cast<size_t>(bounded_credit_ms) * packets_per_ms;
  }

  size_t calculate_video_send_catch_up_burst_size(size_t catch_up_credit, size_t send_batch_size) {
    return std::min(catch_up_credit, send_batch_size);
  }

  bool should_log_video_pacing_late_warning(
    std::chrono::steady_clock::duration late_by,
    size_t frame_packets_sent,
    std::chrono::steady_clock::duration max_idle_credit,
    std::chrono::steady_clock::duration slow_late_threshold
  ) {
    const auto effective_threshold =
      frame_packets_sent == 0 ?
        max_idle_credit + slow_late_threshold :
        slow_late_threshold;
    return late_by >= effective_threshold;
  }

  bool should_emit_video_pacing_late_warning(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point last_warning,
    std::chrono::steady_clock::duration minimum_interval
  ) {
    return last_warning == std::chrono::steady_clock::time_point::min() ||
           (now >= last_warning && now - last_warning >= minimum_interval);
  }

  bool should_emit_video_stream_heartbeat(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point last_heartbeat,
    std::chrono::steady_clock::duration minimum_interval
  ) {
    return last_heartbeat == std::chrono::steady_clock::time_point::min() ||
           (now >= last_heartbeat && now - last_heartbeat >= minimum_interval);
  }

  std::string video_keyframe_send_summary(
    int track_index,
    std::uint64_t frame_index,
    std::size_t fec_blocks,
    std::size_t shards,
    std::size_t batches,
    std::size_t fallback_batches,
    std::uint16_t sequence_first,
    std::uint16_t sequence_last
  ) {
    return "Video keyframe send summary [track=" + std::to_string(track_index) +
           ", frame=" + std::to_string(frame_index) +
           ", fec_blocks=" + std::to_string(fec_blocks) +
           ", shards=" + std::to_string(shards) +
           ", batches=" + std::to_string(batches) +
           ", fallback_batches=" + std::to_string(fallback_batches) +
           ", sequence_first=" + std::to_string(sequence_first) +
           ", sequence_last=" + std::to_string(sequence_last) + ']';
  }

  bool should_log_video_send_frame_warning(
    std::chrono::steady_clock::duration elapsed,
    size_t frame_packets_sent,
    size_t packets_per_ms,
    std::chrono::steady_clock::duration minimum_warning_threshold,
    std::chrono::steady_clock::duration expected_send_slack
  ) {
    const auto expected_budget = calculate_video_send_frame_budget(frame_packets_sent, packets_per_ms);
    const auto effective_threshold = std::max(minimum_warning_threshold, expected_budget + expected_send_slack);
    return elapsed >= effective_threshold;
  }

  namespace {
    constexpr auto max_video_pacing_idle_credit = std::chrono::milliseconds(20);
    constexpr auto slow_video_send_batch_threshold = std::chrono::milliseconds(20);
    constexpr auto slow_video_send_frame_threshold = std::chrono::milliseconds(40);
    constexpr auto slow_video_send_expected_slack = std::chrono::milliseconds(20);
    constexpr auto slow_video_pacing_late_threshold = std::chrono::milliseconds(10);
    constexpr auto video_stream_heartbeat_interval = std::chrono::seconds(10);

    double elapsed_ms(const std::chrono::steady_clock::duration duration) {
      return std::chrono::duration<double, std::milli>(duration).count();
    }
  }  // namespace

  std::vector<uint8_t> replace(const std::string_view &original, const std::string_view &old, const std::string_view &_new) {
    std::vector<uint8_t> replaced;
    replaced.reserve(original.size() + _new.size() - old.size());

    auto begin = std::begin(original);
    auto end = std::end(original);
    auto next = std::search(begin, end, std::begin(old), std::end(old));

    std::copy(begin, next, std::back_inserter(replaced));
    if (next != end) {
      std::copy(std::begin(_new), std::end(_new), std::back_inserter(replaced));
      std::copy(next + old.size(), end, std::back_inserter(replaced));
    }

    return replaced;
  }

  /**
   * @brief Pass gamepad feedback data back to the client.
   * @param session The session object.
   * @param msg The message to pass.
   * @return 0 on success.
   */
  int send_feedback_msg(session_t *session, platf::gamepad_feedback_msg_t &msg) {
    if (!session->control.peer) {
      BOOST_LOG(warning) << "Couldn't send gamepad feedback data, still waiting for PING from Moonlight"sv;
      // Still waiting for PING from Moonlight
      return -1;
    }

    std::lock_guard send_lock {session->control.send_mutex};

    std::string payload;
    if (msg.type == platf::gamepad_feedback_e::rumble) {
      control_rumble_t plaintext;
      plaintext.header.type = packetTypes[IDX_RUMBLE_DATA];
      plaintext.header.payloadLength = sizeof(plaintext) - sizeof(control_header_v2);

      auto &data = msg.data.rumble;

      plaintext.useless = 0xC0FFEE;
      plaintext.id = util::endian::little(msg.id);
      plaintext.lowfreq = util::endian::little(data.lowfreq);
      plaintext.highfreq = util::endian::little(data.highfreq);

      BOOST_LOG(verbose) << "Rumble: "sv << msg.id << " :: "sv << util::hex(data.lowfreq).to_string_view() << " :: "sv << util::hex(data.highfreq).to_string_view();
      std::array<std::uint8_t, sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
        encrypted_payload;

      payload = encode_control(session, util::view(plaintext), encrypted_payload);
    } else if (msg.type == platf::gamepad_feedback_e::rumble_triggers) {
      control_rumble_triggers_t plaintext;
      plaintext.header.type = packetTypes[IDX_RUMBLE_TRIGGER_DATA];
      plaintext.header.payloadLength = sizeof(plaintext) - sizeof(control_header_v2);

      auto &data = msg.data.rumble_triggers;

      plaintext.id = util::endian::little(msg.id);
      plaintext.left = util::endian::little(data.left_trigger);
      plaintext.right = util::endian::little(data.right_trigger);

      BOOST_LOG(verbose) << "Rumble triggers: "sv << msg.id << " :: "sv << util::hex(data.left_trigger).to_string_view() << " :: "sv << util::hex(data.right_trigger).to_string_view();
      std::array<std::uint8_t, sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
        encrypted_payload;

      payload = encode_control(session, util::view(plaintext), encrypted_payload);
    } else if (msg.type == platf::gamepad_feedback_e::set_motion_event_state) {
      control_set_motion_event_t plaintext;
      plaintext.header.type = packetTypes[IDX_SET_MOTION_EVENT];
      plaintext.header.payloadLength = sizeof(plaintext) - sizeof(control_header_v2);

      auto &data = msg.data.motion_event_state;

      plaintext.id = util::endian::little(msg.id);
      plaintext.reportrate = util::endian::little(data.report_rate);
      plaintext.type = data.motion_type;

      BOOST_LOG(verbose) << "Motion event state: "sv << msg.id << " :: "sv << util::hex(data.report_rate).to_string_view() << " :: "sv << util::hex(data.motion_type).to_string_view();
      std::array<std::uint8_t, sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
        encrypted_payload;

      payload = encode_control(session, util::view(plaintext), encrypted_payload);
    } else if (msg.type == platf::gamepad_feedback_e::set_rgb_led) {
      control_set_rgb_led_t plaintext;
      plaintext.header.type = packetTypes[IDX_SET_RGB_LED];
      plaintext.header.payloadLength = sizeof(plaintext) - sizeof(control_header_v2);

      auto &data = msg.data.rgb_led;

      plaintext.id = util::endian::little(msg.id);
      plaintext.r = data.r;
      plaintext.g = data.g;
      plaintext.b = data.b;

      BOOST_LOG(verbose) << "RGB: "sv << msg.id << " :: "sv << util::hex(data.r).to_string_view() << util::hex(data.g).to_string_view() << util::hex(data.b).to_string_view();
      std::array<std::uint8_t, sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
        encrypted_payload;

      payload = encode_control(session, util::view(plaintext), encrypted_payload);
    } else if (msg.type == platf::gamepad_feedback_e::set_adaptive_triggers) {
      control_adaptive_triggers_t plaintext;
      plaintext.header.type = packetTypes[IDX_SET_ADAPTIVE_TRIGGERS];
      plaintext.header.payloadLength = sizeof(plaintext) - sizeof(control_header_v2);

      plaintext.id = util::endian::little(msg.id);
      plaintext.event_flags = msg.data.adaptive_triggers.event_flags;
      plaintext.type_left = msg.data.adaptive_triggers.type_left;
      std::ranges::copy(msg.data.adaptive_triggers.left, plaintext.left);
      plaintext.type_right = msg.data.adaptive_triggers.type_right;
      std::ranges::copy(msg.data.adaptive_triggers.right, plaintext.right);

      std::array<std::uint8_t, sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
        encrypted_payload;

      payload = encode_control(session, util::view(plaintext), encrypted_payload);
    } else {
      BOOST_LOG(error) << "Unknown gamepad feedback message type"sv;
      return -1;
    }

    if (session->broadcast_ref->control_server.send(payload, session->control.peer)) {
      TUPLE_2D(port, addr, platf::from_sockaddr_ex((sockaddr *) &session->control.peer->address.address));
      BOOST_LOG(warning) << "Couldn't send gamepad feedback to ["sv << addr << ':' << port << ']';

      return -1;
    }

    return 0;
  }

  int send_hdr_mode(session_t *session, video::hdr_info_t hdr_info) {
    if (!session->control.peer) {
      BOOST_LOG(warning) << "Couldn't send HDR mode, still waiting for PING from Moonlight"sv;
      // Still waiting for PING from Moonlight
      return -1;
    }

    std::lock_guard send_lock {session->control.send_mutex};

    control_hdr_mode_t plaintext {};
    plaintext.header.type = packetTypes[IDX_HDR_MODE];
    plaintext.header.payloadLength = sizeof(control_hdr_mode_t) - sizeof(control_header_v2);

    plaintext.enabled = hdr_info->enabled;
    plaintext.metadata = hdr_info->metadata;

    std::array<std::uint8_t, sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
      encrypted_payload;

    auto payload = encode_control(session, util::view(plaintext), encrypted_payload);
    if (session->broadcast_ref->control_server.send(payload, session->control.peer)) {
      TUPLE_2D(port, addr, platf::from_sockaddr_ex((sockaddr *) &session->control.peer->address.address));
      BOOST_LOG(warning) << "Couldn't send HDR mode to ["sv << addr << ':' << port << ']';

      return -1;
    }

    BOOST_LOG(debug) << "Sent HDR mode: " << hdr_info->enabled;
    return 0;
  }

  int send_input_timing_ack(session_t *session, const input_timing::completed_input_t &completed) {
    if (!config::input.input_timing ||
        session->state.load(std::memory_order_acquire) != session::state_e::RUNNING ||
        session->config.controlProtocolType != 13 || !session->control.peer ||
        !session->broadcast_ref) {
      return -1;
    }

    SS_INPUT_TIMING_ACK_PACKET ack {};
    ack.version = SS_INPUT_TIMING_ACK_VERSION;
    ack.status = SS_INPUT_TIMING_STATUS_APPLIED;
    ack.family = completed.associated.marker.family == input_timing::event_family_e::keyboard ?
      SS_INPUT_TIMING_FAMILY_KEYBOARD : SS_INPUT_TIMING_FAMILY_MOUSE;
    ack.sequence = util::endian::little(completed.associated.sequence);
    ack.client_capture_time_us = util::endian::little(static_cast<std::uint64_t>(completed.associated.marker.client_capture_us));
    ack.client_send_time_us = util::endian::little(static_cast<std::uint64_t>(completed.associated.marker.client_send_us));
    ack.host_received_time_us = util::endian::little(static_cast<std::uint64_t>(completed.associated.host_received_us));
    ack.host_attached_time_us = util::endian::little(static_cast<std::uint64_t>(completed.associated.host_attached_us));
    ack.host_injected_time_us = util::endian::little(static_cast<std::uint64_t>(completed.host_injected_us));

    std::string plaintext(sizeof(control_header_v2) + sizeof(ack), '\0');
    auto header = reinterpret_cast<control_header_v2 *>(plaintext.data());
    header->type = packetTypes[IDX_INPUT_TIMING_ACK];
    header->payloadLength = sizeof(ack);
    std::memcpy(header->payload(), &ack, sizeof(ack));

    std::array<std::uint8_t, sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(control_header_v2) + sizeof(ack)) + crypto::cipher::tag_size>
      encrypted_payload;

    std::lock_guard send_lock {session->control.send_mutex};
    // This timestamp is deliberately taken at the last plaintext boundary
    // before control encryption and transport send.
    ack.host_send_time_us = util::endian::little(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()));
    std::memcpy(header->payload(), &ack, sizeof(ack));
    auto payload = encode_control(session, plaintext, encrypted_payload);
    if (payload.empty() || session->broadcast_ref->control_server.send(payload, session->control.peer)) {
      return -1;
    }
    return 0;
  }

  void controlBroadcastThread(control_server_t *server) {
    server->map(packetTypes[IDX_PERIODIC_PING], [](session_t *session, const std::string_view &payload) {
      BOOST_LOG(verbose) << "type [IDX_PERIODIC_PING]"sv;
    });

    server->map(packetTypes[IDX_START_A], [&](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_START_A]"sv;
    });

    server->map(packetTypes[IDX_START_B], [&](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_START_B]"sv;
    });

    server->map(packetTypes[IDX_LOSS_STATS], [&](session_t *session, const std::string_view &payload) {
      int32_t *stats = (int32_t *) payload.data();
      auto count = stats[0];
      std::chrono::milliseconds t {stats[1]};

      auto lastGoodFrame = stats[3];

      BOOST_LOG(verbose)
        << "type [IDX_LOSS_STATS]"sv << std::endl
        << "---begin stats---" << std::endl
        << "loss count since last report [" << count << ']' << std::endl
        << "time in milli since last report [" << t.count() << ']' << std::endl
        << "last good frame [" << lastGoodFrame << ']' << std::endl
        << "---end stats---";
    });

    server->map(packetTypes[IDX_REQUEST_IDR_FRAME], [&](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_REQUEST_IDR_FRAME]"sv;

      session->video.idr_events->raise(true);
    });

    server->map(packetTypes[IDX_INVALIDATE_REF_FRAMES], [&](session_t *session, const std::string_view &payload) {
      auto frames = (std::int64_t *) payload.data();
      auto firstFrame = frames[0];
      auto lastFrame = frames[1];

      BOOST_LOG(debug)
        << "type [IDX_INVALIDATE_REF_FRAMES]"sv << std::endl
        << "firstFrame [" << firstFrame << ']' << std::endl
        << "lastFrame [" << lastFrame << ']';

      session->video.invalidate_ref_frames_events->raise(std::make_pair(firstFrame, lastFrame));
    });

    server->map(packetTypes[IDX_INPUT_DATA], [&](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_INPUT_DATA]"sv;

      auto tagged_cipher_length = util::endian::big(*(int32_t *) payload.data());
      std::string_view tagged_cipher {payload.data() + sizeof(tagged_cipher_length), (size_t) tagged_cipher_length};

      std::vector<uint8_t> plaintext;

      auto &cipher = session->control.cipher;
      auto &iv = session->control.legacy_input_enc_iv;
      if (cipher.decrypt(tagged_cipher, plaintext, &iv)) {
        // something went wrong :(

        BOOST_LOG(error) << "Failed to verify tag"sv;

        session::stop(*session);
        return;
      }

      if (tagged_cipher_length >= 16 + iv.size()) {
        std::copy(payload.end() - 16, payload.end(), std::begin(iv));
      }

      input::passthrough(session->input, std::move(plaintext), session->permission);
    });

    server->map(packetTypes[IDX_EXEC_SERVER_CMD], [](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_EXEC_SERVER_CMD]"sv;

      if (!(session->permission & crypto::PERM::server_cmd)) {
        BOOST_LOG(debug) << "Permission denied for Apollo server command from ["sv << session->device_name << ']';
        return;
      }

      if (payload.empty()) {
        BOOST_LOG(warning) << "Apollo server command packet ignored: empty payload"sv;
        return;
      }

      auto cmd_index = static_cast<std::uint8_t>(payload.front());
      if (cmd_index >= config::sunshine.server_cmds.size()) {
        BOOST_LOG(warning) << "Apollo server command packet ignored: invalid index "sv << static_cast<int>(cmd_index);
        return;
      }

      const auto cmd = config::sunshine.server_cmds[cmd_index];
      BOOST_LOG(info) << "Executing Apollo server command ["sv << cmd.cmd_name << ']';
      auto exec_thread = std::thread([cmd] {
        std::error_code ec;
        auto env = proc::proc.get_env();
        boost::filesystem::path working_dir = proc::find_working_directory(cmd.cmd_val, env);
        auto child = platf::run_command(cmd.elevated, true, cmd.cmd_val, working_dir, env, nullptr, ec, nullptr);
        if (ec) {
          BOOST_LOG(error) << "Failed to execute Apollo server command ["sv << cmd.cmd_name << "]: "sv << ec.message();
        } else {
          child.detach();
        }
      });
      exec_thread.detach();
    });

    server->map(packetTypes[IDX_SET_CLIPBOARD], [](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_SET_CLIPBOARD] size="sv << payload.size();

      if (!config::sunshineseat.apollo_clipboard_enabled || !(session->permission & crypto::PERM::clipboard_set)) {
        BOOST_LOG(debug) << "Permission denied for clipboard set from ["sv << session->device_name << ']';
        return;
      }

#ifdef _WIN32
      if (!platf::set_clipboard(std::string {payload})) {
        BOOST_LOG(warning) << "Failed to set clipboard from Apollo control packet for ["sv << session->device_name << ']';
      }
#else
      BOOST_LOG(warning) << "Apollo clipboard set packet ignored on this platform"sv;
#endif
    });

    server->map(packetTypes[IDX_CLIPBOARD_CLIENT_TO_HOST], [](session_t *session, const std::string_view &payload) {
      if (!config::sunshineseat.apollo_clipboard_enabled || session->config.controlProtocolType != 13 || session->state.load(std::memory_order_acquire) != session::state_e::RUNNING || !session->control.peer || !(session->permission & crypto::PERM::clipboard_set)) {
        BOOST_LOG(debug) << "Permission denied for synchronized clipboard set from ["sv << session->device_name << ']';
        return;
      }

      clipboard_message_t message {};
      if (!decode_clipboard_message(payload, message) || message.origin != CLIPBOARD_ORIGIN_CLIENT || message.origin_id == clipboard_origin_id()) {
        BOOST_LOG(warning) << "Synchronized clipboard packet ignored: invalid envelope"sv;
        return;
      }

      {
        std::lock_guard clipboard_lock {session->clipboard_mutex};
        if (!should_apply_clipboard_update(message.content_hash, session->clipboard_last_applied_hash)) {
          return;
        }
        session->clipboard_suppress_hash = message.content_hash;
      }

#ifdef _WIN32
      if (platf::set_clipboard(message.text)) {
        std::lock_guard clipboard_lock {session->clipboard_mutex};
        session->clipboard_last_applied_hash = message.content_hash;
      } else {
        std::lock_guard clipboard_lock {session->clipboard_mutex};
        session->clipboard_suppress_hash = 0;
        BOOST_LOG(warning) << "Failed to set synchronized clipboard text for ["sv << session->device_name << ']';
      }
#else
      std::lock_guard clipboard_lock {session->clipboard_mutex};
      session->clipboard_suppress_hash = 0;
#endif
    });

    server->map(packetTypes[IDX_CLIPBOARD_IMAGE_CLIENT_TO_HOST], [](session_t *session, const std::string_view &payload) {
      if (!config::sunshineseat.apollo_clipboard_enabled || session->config.controlProtocolType != 13 || session->state.load(std::memory_order_acquire) != session::state_e::RUNNING || !session->control.peer || !(session->permission & crypto::PERM::clipboard_set)) {
        BOOST_LOG(debug) << "Permission denied for synchronized clipboard image set from ["sv << session->device_name << ']';
        return;
      }

      clipboard_image_chunk_t chunk {};
      if (!decode_clipboard_image_chunk(payload, chunk) || chunk.origin != CLIPBOARD_ORIGIN_CLIENT || chunk.format != CLIPBOARD_IMAGE_FORMAT_BMP || chunk.origin_id == clipboard_origin_id()) {
        BOOST_LOG(warning) << "Synchronized clipboard image packet ignored: invalid envelope"sv;
        return;
      }

      std::vector<std::uint8_t> complete_image;
      {
        std::lock_guard clipboard_lock {session->clipboard_mutex};
        if (chunk.offset == 0) {
          session->clipboard_image_reassembly.clear();
          session->clipboard_image_reassembly.resize(chunk.total_length);
          session->clipboard_image_reassembly_total_length = chunk.total_length;
          session->clipboard_image_reassembly_received_length = 0;
          session->clipboard_image_reassembly_origin_id = chunk.origin_id;
          session->clipboard_image_reassembly_content_hash = chunk.content_hash;
          session->clipboard_image_reassembly_transfer_id = chunk.transfer_id;
        } else if (session->clipboard_image_reassembly.empty() || session->clipboard_image_reassembly_total_length != chunk.total_length || session->clipboard_image_reassembly_origin_id != chunk.origin_id || session->clipboard_image_reassembly_content_hash != chunk.content_hash || session->clipboard_image_reassembly_transfer_id != chunk.transfer_id || session->clipboard_image_reassembly_received_length != chunk.offset) {
          session->clipboard_image_reassembly.clear();
          session->clipboard_image_reassembly_total_length = 0;
          session->clipboard_image_reassembly_received_length = 0;
          return;
        }

        std::copy(chunk.data.begin(), chunk.data.end(), session->clipboard_image_reassembly.begin() + chunk.offset);
        session->clipboard_image_reassembly_received_length = chunk.offset + static_cast<std::uint32_t>(chunk.data.size());
        if (session->clipboard_image_reassembly_received_length != session->clipboard_image_reassembly_total_length) {
          return;
        }

        complete_image = session->clipboard_image_reassembly;
        session->clipboard_image_reassembly.clear();
        session->clipboard_image_reassembly_total_length = 0;
        session->clipboard_image_reassembly_received_length = 0;
        session->clipboard_image_reassembly_origin_id = 0;
        session->clipboard_image_reassembly_content_hash = 0;
        session->clipboard_image_reassembly_transfer_id = 0;

        const auto image_view = std::string_view {(const char *) complete_image.data(), complete_image.size()};
        if (!clipboard_valid_bmp(image_view) || clipboard_image_hash(image_view) != chunk.content_hash || !should_apply_clipboard_update(chunk.content_hash, session->clipboard_image_last_applied_hash)) {
          return;
        }
        session->clipboard_image_suppress_hash = chunk.content_hash;
      }

#ifdef _WIN32
      if (platf::set_clipboard_image(complete_image)) {
        std::lock_guard clipboard_lock {session->clipboard_mutex};
        session->clipboard_image_last_applied_hash = chunk.content_hash;
      } else {
        std::lock_guard clipboard_lock {session->clipboard_mutex};
        session->clipboard_image_suppress_hash = 0;
        BOOST_LOG(warning) << "Failed to set synchronized clipboard image for ["sv << session->device_name << ']';
      }
#else
      std::lock_guard clipboard_lock {session->clipboard_mutex};
      session->clipboard_image_suppress_hash = 0;
#endif
    });

    server->map(packetTypes[IDX_MIC_CLIENT_TO_HOST], [](session_t *session, const std::string_view &payload) {
      if (!config::sunshineseat.client_mic_enabled || session->config.controlProtocolType != 13 || session->state.load(std::memory_order_acquire) != session::state_e::RUNNING || !session->control.peer || !(session->permission & crypto::PERM::client_microphone)) {
        BOOST_LOG(debug) << "Permission denied for client microphone from ["sv << session->device_name << ']';
        return;
      }
      if (session->client_mic == nullptr) {
        BOOST_LOG(warning) << "Client microphone packet ignored: host sink is not configured"sv;
        return;
      }
      if (!session->client_mic->push_frame(payload)) {
        BOOST_LOG(debug) << "Client microphone frame dropped for ["sv << session->device_name << ']';
      }
    });

    server->map(packetTypes[IDX_ADAPTIVE_BITRATE], [](session_t *session, const std::string_view &payload) {
      if (session->config.controlProtocolType != 13 || session->state.load(std::memory_order_acquire) != session::state_e::RUNNING || !session->control.peer || !session->adaptive_bitrate_kbps) {
        BOOST_LOG(debug) << "Adaptive bitrate request ignored for ["sv << session->device_name << ']';
        return;
      }

      const auto requested = decode_adaptive_bitrate_request(payload);
      if (!requested) {
        BOOST_LOG(warning) << "Adaptive bitrate request ignored: invalid payload size/value"sv;
        return;
      }

      const auto target = dual_display_launch::adaptive_video_encoder_bitrate_target_kbps(
        *requested,
        session->adaptive_bitrate_ceiling_kbps,
        session->config.monitor.dual_display_video_tracks,
        video::active_encoder_supports_adaptive_bitrate()
      );
      if (!target) {
        BOOST_LOG(info) << "Adaptive bitrate request ignored because the active encoder cannot apply it live [client="sv
                        << session->device_name
                        << ", requested_transport_kbps="sv << *requested << ']';
        return;
      }

      const auto previous = session->adaptive_bitrate_kbps->load(std::memory_order_acquire);
      std::optional<int> selected_target;
      {
        std::lock_guard lock {session->adaptive_bitrate_mutex};
        selected_target = select_adaptive_bitrate_target(
          session->adaptive_bitrate_policy,
          previous,
          *target,
          std::chrono::steady_clock::now()
        );
      }
      if (!selected_target) {
        return;
      }

      session->adaptive_bitrate_kbps->store(*selected_target, std::memory_order_release);
      BOOST_LOG(info) << "Adaptive bitrate applied without reconnect [client="sv << session->device_name
                      << ", requested_transport_kbps="sv << *requested
                      << ", previous_encoder_kbps="sv << previous
                      << ", target_encoder_kbps="sv << *selected_target
                      << ", target_transport_kbps="sv
                      << dual_display_launch::video_transport_bitrate_kbps(
                           *selected_target,
                           session->config.monitor.dual_display_video_tracks
                         )
                      << ']';
    });

    server->map(packetTypes[IDX_FILE_TRANSFER_NONCE_REQUEST], [](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_FILE_TRANSFER_NONCE_REQUEST] size="sv << payload.size();

      if (!(session->permission & crypto::PERM::file_upload)) {
        BOOST_LOG(debug) << "Permission denied for file upload nonce request from ["sv << session->device_name << ']';
        return;
      }

      BOOST_LOG(warning) << "Apollo file-transfer nonce request ignored: SunshineSeat has no file-transfer sidecar yet."sv;
    });

    server->map(packetTypes[IDX_ENCRYPTED], [server](session_t *session, const std::string_view &payload) {
      BOOST_LOG(verbose) << "type [IDX_ENCRYPTED]"sv;

      auto header = (control_encrypted_p) (payload.data() - 2);

      auto length = util::endian::little(header->length);
      auto seq = util::endian::little(header->seq);

      if (length < (16 + 4 + 4)) {
        BOOST_LOG(warning) << "Control: Runt packet"sv;
        return;
      }

      auto tagged_cipher_length = length - 4;
      std::string_view tagged_cipher {(char *) header->payload(), (size_t) tagged_cipher_length};

      auto &cipher = session->control.cipher;
      auto &iv = session->control.incoming_iv;
      if (session->config.encryptionFlagsEnabled & SS_ENC_CONTROL_V2) {
        // We use the deterministic IV construction algorithm specified in NIST SP 800-38D
        // Section 8.2.1. The sequence number is our "invocation" field and the 'CC' in the
        // high bytes is the "fixed" field. Because each client provides their own unique
        // key, our values in the fixed field need only uniquely identify each independent
        // use of the client's key with AES-GCM in our code.
        //
        // The sequence number is 32 bits long which allows for 2^32 control stream messages
        // to be received from each client before the IV repeats.
        iv.resize(12);
        std::copy_n((uint8_t *) &seq, sizeof(seq), std::begin(iv));
        iv[10] = 'C';  // Client originated
        iv[11] = 'C';  // Control stream
      } else {
        // Nvidia's old style encryption uses a 16-byte IV
        iv.resize(16);

        iv[0] = (std::uint8_t) seq;
      }

      std::vector<uint8_t> plaintext;
      if (cipher.decrypt(tagged_cipher, plaintext, &iv)) {
        // something went wrong :(

        BOOST_LOG(error) << "Failed to verify tag"sv;

        session::stop(*session);
        return;
      }

      auto type = *(std::uint16_t *) plaintext.data();
      std::string_view next_payload {(char *) plaintext.data() + 4, plaintext.size() - 4};

      if (type == packetTypes[IDX_ENCRYPTED]) {
        BOOST_LOG(error) << "Bad packet type [IDX_ENCRYPTED] found"sv;
        session::stop(*session);
        return;
      }

      // IDX_INPUT_DATA callback will attempt to decrypt unencrypted data, therefore we need pass it directly
      if (type == packetTypes[IDX_INPUT_DATA]) {
        plaintext.erase(std::begin(plaintext), std::begin(plaintext) + 4);
        input::passthrough(session->input, std::move(plaintext), session->permission);
      } else {
        server->call(type, session, next_payload, true);
      }
    });

#ifdef _WIN32
    clipboard_monitor_t clipboard_monitor;
    if (config::sunshineseat.apollo_clipboard_enabled) {
      clipboard_monitor.start([server] {
        auto image = platf::get_clipboard_image();
        if (!image.empty()) {
          clipboard_update_t update;
          update.is_image = true;
          update.image = std::move(image);
          server->clipboard_queue->raise(std::move(update));
          return;
        }

        auto text = platf::get_clipboard();
        if (text.size() <= CLIPBOARD_MAX_TEXT_BYTES) {
          clipboard_update_t update;
          update.text = std::move(text);
          server->clipboard_queue->raise(std::move(update));
        }
      });
    }
#endif

    // This thread handles latency-sensitive control messages
    platf::set_thread_name("stream::controlBroadcast");
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    // Check for both the full shutdown event and the shutdown event for this
    // broadcast to ensure we can inform connected clients of our graceful
    // termination when we shut down.
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);
    auto broadcast_shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);
    while (!shutdown_event->peek() && !broadcast_shutdown_event->peek()) {
      bool has_session_awaiting_peer = false;

      {
        auto lg = server->_sessions.lock();

        auto now = std::chrono::steady_clock::now();

        KITTY_WHILE_LOOP(auto pos = std::begin(*server->_sessions), pos != std::end(*server->_sessions), {
          // Don't perform additional session processing if we're shutting down
          if (shutdown_event->peek() || broadcast_shutdown_event->peek()) {
            break;
          }

          auto session = *pos;

          if (now > session->pingTimeout) {
            auto address = session->control.peer ? platf::from_sockaddr((sockaddr *) &session->control.peer->address.address) : session->control.expected_peer_address;
            BOOST_LOG(info) << address << ": Ping Timeout"sv;
            BOOST_LOG(warning) << "stale_session_detected reason=control_ping_timeout address="sv << address;
            BOOST_LOG(warning) << "forced_disconnect_cleanup_started reason=control_ping_timeout address="sv << address;
            session::stop(*session);
          }

          if (session->state.load(std::memory_order_acquire) == session::state_e::STOPPING) {
            pos = server->_sessions->erase(pos);

            if (session->control.peer) {
              {
                auto ptslg = server->_peer_to_session.lock();
                server->_peer_to_session->erase(session->control.peer);
              }

              enet_peer_disconnect_now(session->control.peer, 0);
            }

            session->controlEnd.raise(true);
            BOOST_LOG(warning) << "forced_disconnect_cleanup_complete"sv;
            continue;
          }

          // Remember if we have a session that's waiting for a peer to connect to the
          // control stream. This ensures the clients are properly notified even when
          // the app terminates before they finish connecting.
          if (!session->control.peer) {
            has_session_awaiting_peer = true;
          } else {
            auto &feedback_queue = session->control.feedback_queue;
            while (feedback_queue->peek()) {
              auto feedback_msg = feedback_queue->pop();

              send_feedback_msg(session, *feedback_msg);
            }

            auto &input_timing_ack_queue = session->control.input_timing_ack_queue;
            while (session->control.peer && input_timing_ack_queue->peek()) {
              auto completed_input = input_timing_ack_queue->pop();
              send_input_timing_ack(session, *completed_input);
            }

            auto &hdr_queue = session->control.hdr_queue;
            while (session->control.peer && hdr_queue->peek()) {
              auto hdr_info = hdr_queue->pop();

              send_hdr_mode(session, std::move(hdr_info));
            }
          }

          ++pos;
        })
      }

      while (server->clipboard_queue->peek()) {
        auto clipboard_update = server->clipboard_queue->pop();
        if (clipboard_update->is_image) {
          broadcast_host_clipboard_image_update(server, clipboard_update->image);
        } else {
          broadcast_host_clipboard_update(server, clipboard_update->text);
        }
      }

      // Don't break until any pending sessions either expire or connect
      if (proc::proc.running() == 0 && !has_session_awaiting_peer) {
        BOOST_LOG(info) << "Process terminated"sv;
        break;
      }

      server->iterate(150ms);
    }

#ifdef _WIN32
    clipboard_monitor.stop();
#endif

    // Let all remaining connections know the server is shutting down
    // reason: graceful termination
    std::uint32_t reason = 0x80030023;

    control_terminate_t plaintext;
    plaintext.header.type = packetTypes[IDX_TERMINATION];
    plaintext.header.payloadLength = sizeof(plaintext.ec);
    plaintext.ec = util::endian::big<uint32_t>(reason);

    std::array<std::uint8_t, sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
      encrypted_payload;

    auto lg = server->_sessions.lock();
    for (auto pos = std::begin(*server->_sessions); pos != std::end(*server->_sessions); ++pos) {
      auto session = *pos;

      // We may not have gotten far enough to have an ENet connection yet
      if (session->control.peer) {
        std::lock_guard send_lock {session->control.send_mutex};
        auto payload = encode_control(session, util::view(plaintext), encrypted_payload);

        if (server->send(payload, session->control.peer)) {
          TUPLE_2D(port, addr, platf::from_sockaddr_ex((sockaddr *) &session->control.peer->address.address));
          BOOST_LOG(warning) << "Couldn't send termination code to ["sv << addr << ':' << port << ']';
        }
      }

      session->shutdown_event->raise(true);
      session->controlEnd.raise(true);
    }

    server->flush();
  }

  void recvThread(broadcast_ctx_t &ctx) {
    std::map<av_session_id_t, message_queue_t> peer_to_video_session;
    std::map<av_session_id_t, message_queue_t> peer_to_audio_session;

    auto &video_sock = ctx.video_sock;
    auto &audio_sock = ctx.audio_sock;

    auto &message_queue_queue = ctx.message_queue_queue;
    auto broadcast_shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);

    auto &io = ctx.io_context;

    udp::endpoint peer;

    std::array<char, 2048> buf[2];
    std::function<void(const boost::system::error_code, size_t)> recv_func[2];

    platf::set_thread_name("stream::recv");

    auto populate_peer_to_session = [&]() {
      while (message_queue_queue->peek()) {
        auto message_queue_opt = message_queue_queue->pop();
        TUPLE_3D_REF(socket_type, session_id, message_queue, *message_queue_opt);

        switch (socket_type) {
          case socket_e::video:
            if (message_queue) {
              peer_to_video_session.emplace(session_id, message_queue);
            } else {
              peer_to_video_session.erase(session_id);
            }
            break;
          case socket_e::audio:
            if (message_queue) {
              peer_to_audio_session.emplace(session_id, message_queue);
            } else {
              peer_to_audio_session.erase(session_id);
            }
            break;
        }
      }
    };

    auto recv_func_init = [&](udp::socket &sock, int buf_elem, std::map<av_session_id_t, message_queue_t> &peer_to_session) {
      recv_func[buf_elem] = [&, buf_elem](const boost::system::error_code &ec, size_t bytes) {
        auto fg = util::fail_guard([&]() {
          sock.async_receive_from(asio::buffer(buf[buf_elem]), peer, 0, recv_func[buf_elem]);
        });

        auto type_str = buf_elem ? "AUDIO"sv : "VIDEO"sv;
        BOOST_LOG(verbose) << "Recv: "sv << peer.address().to_string() << ':' << peer.port() << " :: " << type_str;

        populate_peer_to_session();

        // No data, yet no error
        if (ec == boost::system::errc::connection_refused || ec == boost::system::errc::connection_reset) {
          return;
        }

        if (ec || !bytes) {
          BOOST_LOG(error) << "Couldn't receive data from udp socket: "sv << ec.message();
          return;
        }

        if (bytes == 4) {
          // For legacy PING packets, find the matching session by address.
          auto it = peer_to_session.find(peer.address());
          if (it != std::end(peer_to_session)) {
            BOOST_LOG(debug) << "RAISE: "sv << peer.address().to_string() << ':' << peer.port() << " :: " << type_str;
            it->second->raise(peer, std::string {buf[buf_elem].data(), bytes});
          }
        } else if (bytes >= sizeof(SS_PING)) {
          auto ping = (PSS_PING) buf[buf_elem].data();

          // For new PING packets that include a client identifier, search by payload.
          auto it = peer_to_session.find(std::string {ping->payload, sizeof(ping->payload)});
          if (it != std::end(peer_to_session)) {
            BOOST_LOG(debug) << "RAISE: "sv << peer.address().to_string() << ':' << peer.port() << " :: " << type_str;
            it->second->raise(peer, std::string {buf[buf_elem].data(), bytes});
          }
        }
      };
    };

    recv_func_init(video_sock, 0, peer_to_video_session);
    recv_func_init(audio_sock, 1, peer_to_audio_session);

    video_sock.async_receive_from(asio::buffer(buf[0]), peer, 0, recv_func[0]);
    audio_sock.async_receive_from(asio::buffer(buf[1]), peer, 0, recv_func[1]);

    while (!broadcast_shutdown_event->peek()) {
      io.run();
    }
  }

  void videoBroadcastThread(udp::socket &sock) {
    auto shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);
    auto packets = mail::man->queue<video::packet_t>(mail::video_packets);
    auto video_epoch = std::chrono::steady_clock::now();

    // Video traffic is sent on this thread
    platf::set_thread_name("stream::videoBroadcast");
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    logging::min_max_avg_periodic_logger<double> frame_processing_latency_logger(debug, "Frame processing latency", "ms");

    logging::time_delta_periodic_logger frame_send_batch_latency_logger(debug, "Network: each send_batch() latency");
    logging::time_delta_periodic_logger frame_fec_latency_logger(debug, "Network: each FEC block latency");
    logging::time_delta_periodic_logger frame_network_latency_logger(debug, "Network: frame's overall network latency");

    crypto::aes_t iv(12);

    auto timer = platf::create_high_precision_timer();
    if (!timer || !*timer) {
      BOOST_LOG(error) << "Failed to create timer, aborting video broadcast thread";
      return;
    }

    auto ratecontrol_next_frame_start = std::chrono::steady_clock::now();
    auto last_pacing_warning = std::chrono::steady_clock::time_point::min();
    auto last_stream_heartbeat = std::chrono::steady_clock::time_point::min();
    size_t suppressed_pacing_warnings = 0;
    constexpr auto pacing_warning_interval = 1s;

    while (auto packet = packets->pop()) {
      if (shutdown_event->peek()) {
        break;
      }

      const auto frame_network_started = std::chrono::steady_clock::now();
      frame_network_latency_logger.first_point_now();
      auto frame_packetization_elapsed = std::chrono::steady_clock::duration::zero();
      auto frame_fec_elapsed = std::chrono::steady_clock::duration::zero();
      auto frame_pacing_sleep_elapsed = std::chrono::steady_clock::duration::zero();
      auto frame_send_elapsed = std::chrono::steady_clock::duration::zero();

      auto session = (session_t *) packet->channel_data;
      if (should_drop_video_packet(shutdown_event->peek(), session->state.load(std::memory_order_acquire) == session::state_e::RUNNING)) {
        continue;
      }
      const auto resolved_track_index = dual_display_launch::video_packet_track_index(
        session->config.monitor.dual_display_video_tracks,
        packet->track_index
      );
      if (!resolved_track_index) {
        BOOST_LOG(error) << "dual_display_invalid_video_packet_track: index="
                         << packet->track_index << " action=drop";
        continue;
      }
      const auto track_index = *resolved_track_index;

      auto &video_track = session->video.tracks[track_index];
      auto lowseq = video_track.lowseq;
      const auto frame_sequence_first = lowseq;
      std::size_t frame_total_shards = 0;
      std::size_t frame_send_batches = 0;
      std::size_t frame_fallback_batches = 0;

      std::string_view payload {(char *) packet->data(), packet->data_size()};
      std::vector<uint8_t> payload_with_replacements;

      // Apply replacements on the packet payload before performing any other operations.
      // We need to know the final frame size to calculate the last packet size, and we
      // must avoid matching replacements against the frame header or any other non-video
      // part of the payload.
      if (packet->is_idr() && packet->replacements) {
        for (auto &replacement : *packet->replacements) {
          auto frame_old = replacement.old;
          auto frame_new = replacement._new;

          payload_with_replacements = replace(payload, frame_old, frame_new);
          payload = {(char *) payload_with_replacements.data(), payload_with_replacements.size()};
        }
      }

      video_short_frame_header_t frame_header = {};
      frame_header.headerType = 0x01;  // Short header type
      frame_header.frameType = packet->is_idr()                     ? 2 :
                               packet->after_ref_frame_invalidation ? 5 :
                                                                      1;
      frame_header.lastPayloadLen = (payload.size() + sizeof(frame_header)) % (session->config.packetsize - sizeof(NV_VIDEO_PACKET));
      if (frame_header.lastPayloadLen == 0) {
        frame_header.lastPayloadLen = session->config.packetsize - sizeof(NV_VIDEO_PACKET);
      }

      if (packet->frame_timestamp) {
        auto duration_to_latency = [](const std::chrono::steady_clock::duration &duration) {
          const auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
          return (uint16_t) std::clamp<decltype(duration_us)>((duration_us + 50) / 100, 0, std::numeric_limits<uint16_t>::max());
        };

        uint16_t latency = duration_to_latency(std::chrono::steady_clock::now() - *packet->frame_timestamp);
        frame_header.frame_processing_latency = latency;
        frame_processing_latency_logger.collect_and_log(latency / 10.);
      } else {
        frame_header.frame_processing_latency = 0;
      }

      const auto fec_fallback_active = std::chrono::steady_clock::now() < video_track.fec_fallback_until;
      auto fecPercentage = packet->is_idr() ?
                             effective_video_keyframe_fec_percentage(config::stream.fec_percentage, fec_fallback_active) :
                             effective_video_fec_percentage(config::stream.fec_percentage, fec_fallback_active);

      // Insert space for packet headers
      auto blocksize = session->config.packetsize + MAX_RTP_HEADER_SIZE;
      auto payload_blocksize = blocksize - sizeof(video_packet_raw_t);
      auto payload_new = concat_and_insert(sizeof(video_packet_raw_t), payload_blocksize, std::string_view {(char *) &frame_header, sizeof(frame_header)}, payload);

      payload = std::string_view {(char *) payload_new.data(), payload_new.size()};

      // There are 2 bits for FEC block count for a maximum of 4 FEC blocks
      constexpr auto MAX_FEC_BLOCKS = 4;

      // The max number of data shards per block is found by solving this system of equations for D:
      // D = 255 - P
      // P = D * F
      // which results in the solution:
      // D = 255 / (1 + F)
      // multiplied by 100 since F is the percentage as an integer:
      // D = (255 * 100) / (100 + F)
      auto max_data_shards_per_fec_block = (DATA_SHARDS_MAX * 100) / (100 + fecPercentage);

      // Compute the number of FEC blocks needed for this frame using the block size and max shards
      auto max_data_per_fec_block = max_data_shards_per_fec_block * blocksize;
      auto fec_blocks_needed = (payload.size() + (max_data_per_fec_block - 1)) / max_data_per_fec_block;

      // If the number of FEC blocks needed exceeds the protocol limit, turn off FEC for this frame.
      // For normal FEC percentages, this should only happen for enormous frames (over 800 packets at 20%).
      if (fec_blocks_needed > MAX_FEC_BLOCKS) {
        BOOST_LOG(warning) << "Skipping FEC for abnormally large encoded frame (needed "sv << fec_blocks_needed << " FEC blocks)"sv;
        fecPercentage = 0;
        fec_blocks_needed = MAX_FEC_BLOCKS;
      }

      std::array<std::string_view, MAX_FEC_BLOCKS> fec_blocks;
      auto fec_blocks_begin = std::begin(fec_blocks);
      auto fec_blocks_end = std::begin(fec_blocks) + fec_blocks_needed;

      BOOST_LOG(verbose) << "Generating "sv << fec_blocks_needed << " FEC blocks"sv;

      // Align individual FEC blocks to blocksize
      auto unaligned_size = payload.size() / fec_blocks_needed;
      auto aligned_size = ((unaligned_size + (blocksize - 1)) / blocksize) * blocksize;

      // If we exceed the 10-bit FEC packet index (which means our frame exceeded 4096 packets),
      // the frame will be unrecoverable. Log an error for this case.
      if (aligned_size / blocksize >= 1024) {
        BOOST_LOG(error) << "Encoder produced a frame too large to send! Is the encoder broken? (needed "sv << (aligned_size / blocksize) << " packets)"sv;
      }

      // Split the data into aligned FEC blocks
      for (int x = 0; x < fec_blocks_needed; ++x) {
        if (x == fec_blocks_needed - 1) {
          // The last block must extend to the end of the payload
          fec_blocks[x] = payload.substr(x * aligned_size);
        } else {
          // Earlier blocks just extend to the next block offset
          fec_blocks[x] = payload.substr(x * aligned_size, aligned_size);
        }
      }

      frame_packetization_elapsed = std::chrono::steady_clock::now() - frame_network_started;

      try {
        const auto encoder_bitrate_kbps = session->adaptive_bitrate_kbps ? session->adaptive_bitrate_kbps->load(std::memory_order_acquire) : session->config.monitor.bitrate;
        const auto requested_bitrate_kbps = dual_display_launch::video_transport_bitrate_kbps(
          encoder_bitrate_kbps,
          session->config.monitor.dual_display_video_tracks
        );
        const auto pacing_bitrate_kbps = effective_video_send_bitrate_kbps(
          requested_bitrate_kbps,
          config::video.max_bitrate
        );
        auto [ratecontrol_packets_in_1ms, send_batch_size] =
          calculate_video_send_pacing(blocksize, pacing_bitrate_kbps, fecPercentage);

        // Don't ignore the last ratecontrol group of the previous frame
        auto ratecontrol_frame_start = calculate_video_pacing_frame_start(
          ratecontrol_next_frame_start,
          std::chrono::steady_clock::now()
        );

        size_t ratecontrol_frame_packets_sent = 0;
        size_t ratecontrol_group_packets_sent = 0;
        size_t ratecontrol_burst_packets_in_1ms = ratecontrol_packets_in_1ms;
        size_t ratecontrol_frame_batch_size = send_batch_size;
        size_t ratecontrol_catch_up_credit = 0;

        auto blockIndex = 0;
        std::for_each(fec_blocks_begin, fec_blocks_end, [&](std::string_view &current_payload) {
          auto packets = (current_payload.size() + (blocksize - 1)) / blocksize;

          for (int x = 0; x < packets; ++x) {
            auto *inspect = (video_packet_raw_t *) &current_payload[x * blocksize];

            inspect->packet.frameIndex = (uint32_t) packet->frame_index();
            inspect->packet.streamPacketIndex = ((uint32_t) lowseq + x) << 8;

            // Match multiFecFlags with Moonlight
            inspect->packet.multiFecFlags = 0x10;
            inspect->packet.multiFecBlocks = (blockIndex << 4) | ((fec_blocks_needed - 1) << 6);

            inspect->packet.flags = FLAG_CONTAINS_PIC_DATA;
            if (x == 0) {
              inspect->packet.flags |= FLAG_SOF;
            }
            if (x == packets - 1) {
              inspect->packet.flags |= FLAG_EOF;
            }
          }

          frame_fec_latency_logger.first_point_now();
          const auto fec_started = std::chrono::steady_clock::now();
          // If video encryption is enabled, we allocate space for the encryption header before each shard
          // Small dual-display frames need room for four parity shards so a
          // short burst loss does not strand either decoder waiting for IDR.
          // Tiny one-data-shard blocks remain capped at one parity shard for
          // protocol compatibility.
          const auto data_shards = (current_payload.size() + blocksize - 1) / blocksize;
          const auto parity_capacity_floor = dual_display_video_parity_capacity_floor(
            session->config.monitor.dual_display_video_tracks,
            data_shards
          );
          auto shards = fec::encode(
            current_payload,
            blocksize,
            fecPercentage,
            session->config.minRequiredFecPackets,
            video_track.cipher ? sizeof(video_packet_enc_prefix_t) : 0,
            parity_capacity_floor
          );
          frame_total_shards += shards.size();
          frame_fec_elapsed += std::chrono::steady_clock::now() - fec_started;
          frame_fec_latency_logger.second_point_now_and_log();

          ratecontrol_burst_packets_in_1ms = calculate_video_send_frame_burst_packets_per_ms(
            ratecontrol_packets_in_1ms,
            shards.size()
          );
          ratecontrol_frame_batch_size = calculate_video_send_frame_batch_size_for_frame(
            ratecontrol_burst_packets_in_1ms,
            session->config.monitor.dual_display_video_tracks,
            packet->is_idr()
          );
          ratecontrol_catch_up_credit = calculate_video_send_catch_up_burst_size(
            calculate_video_send_catch_up_credit(
              std::chrono::steady_clock::now() - ratecontrol_frame_start,
              ratecontrol_burst_packets_in_1ms,
              max_video_pacing_idle_credit
            ),
            ratecontrol_frame_batch_size
          );

          auto peer_address = session->video.peer.address();
          auto batch_info = platf::batched_send_info_t {
            shards.headers.begin(),
            shards.prefixsize,
            shards.payload_buffers,
            shards.blocksize,
            0,
            0,
            (uintptr_t) sock.native_handle(),
            peer_address,
            session->video.peer.port(),
            session->localAddress,
            packet->is_idr(),
            track_index,
            static_cast<std::uint64_t>(packet->frame_index()),
          };

          size_t next_shard_to_send = 0;

          // RTP video timestamps use a 90 KHz clock and the frame_timestamp from when the frame was captured
          // When a timestamp isn't available (duplicate frames), the timestamp from rate control is used instead.
          bool frame_is_dupe = false;
          if (!packet->frame_timestamp) {
            packet->frame_timestamp = ratecontrol_next_frame_start;
            frame_is_dupe = true;
          }
          using rtp_tick = std::chrono::duration<uint32_t, std::ratio<1, 90000>>;
          uint32_t timestamp = std::chrono::round<rtp_tick>(*packet->frame_timestamp - video_epoch).count();

          // set FEC info now that we know for sure what our percentage will be for this frame
          for (auto x = 0; x < shards.size(); ++x) {
            auto *inspect = (video_packet_raw_t *) shards.data(x);

            inspect->packet.fecInfo =
              (uint32_t) (x << 12 |
                          shards.data_shards << 22 |
                          shards.percentage << 4);

            inspect->rtp.header = 0x80 | FLAG_EXTENSION;
            inspect->rtp.sequenceNumber = util::endian::big<uint16_t>(lowseq + x);
            inspect->rtp.timestamp = util::endian::big<uint32_t>(timestamp);
            inspect->rtp.ssrc = util::endian::big<std::uint32_t>(
              session->config.monitor.dual_display_video_tracks ? dual_display_launch::video_track_ssrc(track_index) : 0
            );

            inspect->packet.multiFecBlocks = (blockIndex << 4) | ((fec_blocks_needed - 1) << 6);
            inspect->packet.frameIndex = (uint32_t) packet->frame_index();

            // Encrypt this shard if video encryption is enabled
            if (video_track.cipher) {
              // We use the deterministic IV construction algorithm specified in NIST SP 800-38D
              // Section 8.2.1. The sequence number is our "invocation" field and the 'V' in the
              // high bytes is the "fixed" field. Because each client provides their own unique
              // key, our values in the fixed field need only uniquely identify each independent
              // use of the client's key with AES-GCM in our code.
              //
              // The IV counter is 64 bits long which allows for 2^64 encrypted video packets
              // to be sent to each client before the IV repeats.
              std::copy_n((uint8_t *) &video_track.gcm_iv_counter, sizeof(video_track.gcm_iv_counter), std::begin(iv));
              iv[10] = session->config.monitor.dual_display_video_tracks ? (std::uint8_t) track_index : 0;
              iv[11] = 'V';  // Video stream
              video_track.gcm_iv_counter++;

              // Encrypt the target buffer in place
              auto *prefix = (video_packet_enc_prefix_t *) shards.prefix(x);
              prefix->frameNumber = (std::uint32_t) packet->frame_index();
              std::copy(std::begin(iv), std::end(iv), prefix->iv);
              video_track.cipher->encrypt(std::string_view {(char *) inspect, (size_t) blocksize}, prefix->tag, (uint8_t *) inspect, &iv);
            }

            if (x - next_shard_to_send + 1 >= ratecontrol_frame_batch_size || x + 1 == shards.size()) {
              // Do pacing within the frame.
              // Also trigger pacing before the first send_batch() of the frame
              // to account for the last send_batch() of the previous frame.
              const auto ratecontrol_group_budget = ratecontrol_burst_packets_in_1ms + ratecontrol_catch_up_credit;
              if (ratecontrol_group_packets_sent >= ratecontrol_group_budget || ratecontrol_frame_packets_sent == 0) {
                auto now = std::chrono::steady_clock::now();
                if (ratecontrol_group_packets_sent >= ratecontrol_group_budget && ratecontrol_catch_up_credit > 0) {
                  ratecontrol_frame_start = now;
                  ratecontrol_catch_up_credit = 0;
                }
                auto due = ratecontrol_frame_start +
                           std::chrono::duration_cast<std::chrono::nanoseconds>(1ms) *
                             ratecontrol_frame_packets_sent / ratecontrol_burst_packets_in_1ms;

                now = std::chrono::steady_clock::now();
                if (now < due) {
                  const auto sleep_started = now;
                  // A corrupted/far-future pacing deadline must not turn into
                  // a multi-second stream freeze. Keep each timer wait inside
                  // the same bounded window used for inter-frame pacing; the
                  // next batch will re-evaluate the deadline and preserve the
                  // long-term bitrate without blocking the video thread.
                  constexpr auto max_video_pacing_sleep = 4ms;
                  const auto bounded_pacing_sleep = std::min(
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(due - now),
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(max_video_pacing_sleep)
                  );
                  timer->sleep_for(bounded_pacing_sleep);
                  frame_pacing_sleep_elapsed += std::chrono::steady_clock::now() - sleep_started;
                  ratecontrol_catch_up_credit = 0;
                } else {
                  const auto late_by = now - due;
                  ratecontrol_catch_up_credit = calculate_video_send_catch_up_burst_size(
                    calculate_video_send_catch_up_credit(
                      late_by,
                      ratecontrol_burst_packets_in_1ms,
                      max_video_pacing_idle_credit
                    ),
                    ratecontrol_frame_batch_size
                  );
                  if (should_log_video_pacing_late_warning(late_by, ratecontrol_frame_packets_sent, max_video_pacing_idle_credit, slow_video_pacing_late_threshold)) {
                    if (should_emit_video_pacing_late_warning(now, last_pacing_warning, pacing_warning_interval)) {
                      BOOST_LOG(warning) << "Video pacing_late_diagnostic [frame="sv << packet->frame_index()
                                         << ", late_ms="sv << elapsed_ms(late_by)
                                         << ", frame_packets_sent="sv << ratecontrol_frame_packets_sent
                                         << ", packets_per_ms="sv << ratecontrol_burst_packets_in_1ms
                                         << ", sustained_packets_per_ms="sv << ratecontrol_packets_in_1ms
                                         << ", batch_target="sv << ratecontrol_frame_batch_size
                                         << ", catch_up_credit_packets="sv << ratecontrol_catch_up_credit
                                         << ", suppressed="sv << suppressed_pacing_warnings << ']';
                      last_pacing_warning = now;
                      suppressed_pacing_warnings = 0;
                    } else {
                      ++suppressed_pacing_warnings;
                    }
                  }
                }

                ratecontrol_group_packets_sent = 0;
              }

              size_t current_batch_size = x - next_shard_to_send + 1;
              batch_info.block_offset = next_shard_to_send;
              batch_info.block_count = current_batch_size;

              frame_send_batch_latency_logger.first_point_now();
              const auto send_batch_started = std::chrono::steady_clock::now();
              // Use a batched send if it's supported on this platform
              ++frame_send_batches;
              if (!platf::send_batch(batch_info)) {
                ++frame_fallback_batches;
                // Batched send is not available, so send each packet individually
                BOOST_LOG(verbose) << "Falling back to unbatched send"sv;
                for (auto y = 0; y < current_batch_size; y++) {
                  auto send_info = platf::send_info_t {
                    shards.prefix(next_shard_to_send + y),
                    shards.prefixsize,
                    shards.data(next_shard_to_send + y),
                    shards.blocksize,
                    (uintptr_t) sock.native_handle(),
                    peer_address,
                    session->video.peer.port(),
                    session->localAddress,
                  };

                  platf::send(send_info);
                }
              }
              frame_send_batch_latency_logger.second_point_now_and_log();
              const auto send_batch_elapsed = std::chrono::steady_clock::now() - send_batch_started;
              frame_send_elapsed += send_batch_elapsed;
              if (send_batch_elapsed >= slow_video_send_batch_threshold) {
                BOOST_LOG(debug) << "Video send_batch latency diagnostic [frame="sv << packet->frame_index()
                                 << ", elapsed_ms="sv << elapsed_ms(send_batch_elapsed)
                                 << ", batch_size="sv << current_batch_size
                                 << ", packets_per_ms="sv << ratecontrol_burst_packets_in_1ms
                                 << ", blocksize="sv << blocksize << ']';
              }

              ratecontrol_group_packets_sent += current_batch_size;
              ratecontrol_frame_packets_sent += current_batch_size;
              next_shard_to_send = x + 1;
            }
          }

          // A burst-safe keyframe may be sent in small bounded batches faster
          // than its sustained-rate duration. Do not turn the remaining pacing
          // debt into a single long idle gap before the next frame: that gap is
          // visible as a stream freeze. Batches remain bounded, while any
          // interframe delay is limited to four milliseconds.
          constexpr auto max_video_pacing_interframe_delay = 4ms;
          const auto scheduled_next_frame_start = ratecontrol_frame_start +
                                                  std::chrono::duration_cast<std::chrono::nanoseconds>(1ms) *
                                                    ratecontrol_frame_packets_sent / ratecontrol_packets_in_1ms;
          ratecontrol_next_frame_start = calculate_video_pacing_next_frame_start(
            scheduled_next_frame_start,
            std::chrono::steady_clock::now(),
            max_video_pacing_interframe_delay
          );

          frame_network_latency_logger.second_point_now_and_log();
          const auto frame_network_elapsed = std::chrono::steady_clock::now() - frame_network_started;
          if (should_log_video_send_frame_warning(frame_network_elapsed, ratecontrol_frame_packets_sent, ratecontrol_burst_packets_in_1ms, slow_video_send_frame_threshold, slow_video_send_expected_slack)) {
            const auto expected_budget = calculate_video_send_frame_budget(ratecontrol_frame_packets_sent, ratecontrol_burst_packets_in_1ms);
            const auto overshoot = std::max(frame_network_elapsed - expected_budget, std::chrono::steady_clock::duration::zero());
            BOOST_LOG(warning) << "Video send_path_slow [frame="sv << packet->frame_index()
                               << ", elapsed_ms="sv << elapsed_ms(frame_network_elapsed)
                               << ", expected_budget_ms="sv << elapsed_ms(expected_budget)
                               << ", overshoot_ms="sv << elapsed_ms(overshoot)
                               << ", idr="sv << packet->is_idr()
                               << ", shards="sv << shards.size()
                               << ", fec_percent="sv << shards.percentage
                               << ", frame_packets_sent="sv << ratecontrol_frame_packets_sent
                               << ", packets_per_ms="sv << ratecontrol_burst_packets_in_1ms
                               << ", sustained_packets_per_ms="sv << ratecontrol_packets_in_1ms
                               << ", batch_target="sv << ratecontrol_frame_batch_size
                               << ", packetize_ms="sv << elapsed_ms(frame_packetization_elapsed)
                               << ", fec_ms="sv << elapsed_ms(frame_fec_elapsed)
                               << ", pacing_sleep_ms="sv << elapsed_ms(frame_pacing_sleep_elapsed)
                               << ", send_ms="sv << elapsed_ms(frame_send_elapsed) << ']';
          }

          BOOST_LOG(verbose) << "Sent Frame seq ["sv << packet->frame_index() << "] pts ["sv << timestamp
                             << "] shards ["sv << shards.size() << "/"sv << shards.percentage << "%]"sv
                             << (frame_is_dupe ? " Dupe" : "")
                             << (packet->is_idr() ? " Key" : "")
                             << (packet->after_ref_frame_invalidation ? " RFI" : "");

          ++blockIndex;
          lowseq += shards.size();
        });

        if (packet->is_idr() && frame_total_shards > 0) {
          BOOST_LOG(info) << video_keyframe_send_summary(
            track_index,
            static_cast<std::uint64_t>(packet->frame_index()),
            fec_blocks_needed,
            frame_total_shards,
            frame_send_batches,
            frame_fallback_batches,
            frame_sequence_first,
            static_cast<std::uint16_t>(frame_sequence_first + frame_total_shards - 1)
          );
        }

        // FEC pressure can surface as expensive block preparation or batched
        // send work rather than inside fec::encode itself. Use the largest
        // measured send-path component so the next frame can avoid repeating
        // a pathological FEC/block layout after any real stall.
        const auto send_path_pressure = std::max({
          frame_fec_elapsed,
          frame_packetization_elapsed,
          frame_send_elapsed,
        });
        if (should_enter_video_fec_fallback(send_path_pressure)) {
          video_track.fec_fallback_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
          BOOST_LOG(warning) << "Video adaptive FEC fallback activated after send-path pressure [pressure_ms="sv
                             << std::chrono::duration<double, std::milli>(send_path_pressure).count()
                             << ", packetize_ms="sv << elapsed_ms(frame_packetization_elapsed)
                             << ", fec_ms="sv << elapsed_ms(frame_fec_elapsed)
                             << ", send_ms="sv << elapsed_ms(frame_send_elapsed)
                             << ", fallback_seconds=5]"sv;
        }

        // Catch-up credit is scoped to one encoded frame and must never carry
        // forward into the next frame as an unbounded microburst allowance.
        ratecontrol_catch_up_credit = 0;

        const auto heartbeat_now = std::chrono::steady_clock::now();
        if (should_emit_video_stream_heartbeat(heartbeat_now, last_stream_heartbeat, video_stream_heartbeat_interval)) {
          BOOST_LOG(info) << "Video stream_heartbeat [frame="sv << packet->frame_index()
                          << ", packets="sv << ratecontrol_frame_packets_sent
                          << ", requested_bitrate_kbps="sv << requested_bitrate_kbps
                          << ", pacing_bitrate_kbps="sv << pacing_bitrate_kbps
                          << ", elapsed_ms="sv << elapsed_ms(heartbeat_now - frame_network_started) << ']';
          last_stream_heartbeat = heartbeat_now;
        }

        video_track.lowseq = lowseq;
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Broadcast video failed "sv << e.what();
        std::this_thread::sleep_for(100ms);
      }
    }

    shutdown_event->raise(true);
  }

  void audioBroadcastThread(udp::socket &sock) {
    auto shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);
    auto packets = mail::man->queue<audio::packet_t>(mail::audio_packets);

    audio_packet_t audio_packet;
    fec::rs_t rs {reed_solomon_new(RTPA_DATA_SHARDS, RTPA_FEC_SHARDS)};
    crypto::aes_t iv(16);

    // For unknown reasons, the RS parity matrix computed by our RS implementation
    // doesn't match the one Nvidia uses for audio data. I'm not exactly sure why,
    // but we can simply replace it with the matrix generated by OpenFEC which
    // works correctly. This is possible because the data and FEC shard count is
    // constant and known in advance.
    const unsigned char parity[] = {0x77, 0x40, 0x38, 0x0e, 0xc7, 0xa7, 0x0d, 0x6c};
    memcpy(rs.get()->p, parity, sizeof(parity));

    audio_packet.rtp.header = 0x80;
    audio_packet.rtp.packetType = 97;
    audio_packet.rtp.ssrc = 0;

    // Audio traffic is sent on this thread
    platf::set_thread_name("stream::audioBroadcast");
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    while (auto packet = packets->pop()) {
      if (shutdown_event->peek()) {
        break;
      }

      TUPLE_2D_REF(channel_data, packet_data, *packet);
      auto session = (session_t *) channel_data;

      auto sequenceNumber = session->audio.sequenceNumber;
      auto timestamp = session->audio.timestamp;

      *(std::uint32_t *) iv.data() = util::endian::big<std::uint32_t>(session->audio.avRiKeyId + sequenceNumber);

      auto &shards_p = session->audio.shards_p;

      auto bytes = encode_audio(session->config.encryptionFlagsEnabled & SS_ENC_AUDIO, packet_data, shards_p[sequenceNumber % RTPA_DATA_SHARDS], iv, session->audio.cipher);
      if (bytes < 0) {
        BOOST_LOG(error) << "Couldn't encode audio packet"sv;
        break;
      }

      BOOST_LOG(verbose) << "Audio [seq "sv << sequenceNumber << ", pts "sv << timestamp << "] ::  send..."sv;

      audio_packet.rtp.sequenceNumber = util::endian::big(sequenceNumber);
      audio_packet.rtp.timestamp = util::endian::big(timestamp);

      session->audio.sequenceNumber++;
      session->audio.timestamp += session->config.audio.packetDuration;

      auto peer_address = session->audio.peer.address();
      try {
        auto send_info = platf::send_info_t {
          (const char *) &audio_packet,
          sizeof(audio_packet),
          (const char *) shards_p[sequenceNumber % RTPA_DATA_SHARDS],
          (size_t) bytes,
          (uintptr_t) sock.native_handle(),
          peer_address,
          session->audio.peer.port(),
          session->localAddress,
        };
        platf::send(send_info);

        auto &fec_packet = session->audio.fec_packet;
        // initialize the FEC header at the beginning of the FEC block
        if (sequenceNumber % RTPA_DATA_SHARDS == 0) {
          fec_packet.fecHeader.baseSequenceNumber = util::endian::big(sequenceNumber);
          fec_packet.fecHeader.baseTimestamp = util::endian::big(timestamp);
        }

        // generate parity shards at the end of the FEC block
        if ((sequenceNumber + 1) % RTPA_DATA_SHARDS == 0) {
          reed_solomon_encode(rs.get(), shards_p.begin(), RTPA_TOTAL_SHARDS, bytes);

          for (auto x = 0; x < RTPA_FEC_SHARDS; ++x) {
            fec_packet.rtp.sequenceNumber = util::endian::big<std::uint16_t>(sequenceNumber + x + 1);
            fec_packet.fecHeader.fecShardIndex = x;

            auto send_info = platf::send_info_t {
              (const char *) &fec_packet,
              sizeof(fec_packet),
              (const char *) shards_p[RTPA_DATA_SHARDS + x],
              (size_t) bytes,
              (uintptr_t) sock.native_handle(),
              peer_address,
              session->audio.peer.port(),
              session->localAddress,
            };
            platf::send(send_info);
            BOOST_LOG(verbose) << "Audio FEC ["sv << (sequenceNumber & ~(RTPA_DATA_SHARDS - 1)) << ' ' << x << "] ::  send..."sv;
          }
        }
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Broadcast audio failed "sv << e.what();
        std::this_thread::sleep_for(100ms);
      }
    }

    shutdown_event->raise(true);
  }

  int start_broadcast(broadcast_ctx_t &ctx) {
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);
    auto protocol = address_family == net::IPV4 ? udp::v4() : udp::v6();
    auto control_port = net::map_port(CONTROL_PORT);
    auto video_port = net::map_port(VIDEO_STREAM_PORT);
    auto audio_port = net::map_port(AUDIO_STREAM_PORT);

    if (ctx.control_server.bind(address_family, control_port)) {
      BOOST_LOG(error) << "Couldn't bind Control server to port ["sv << control_port << "], likely another process already bound to the port"sv;

      return -1;
    }

    boost::system::error_code ec;
    ctx.video_sock.open(protocol, ec);
    if (ec) {
      BOOST_LOG(fatal) << "Couldn't open socket for Video server: "sv << ec.message();

      return -1;
    }

    // Set video socket send buffer size (SO_SENDBUF) to 1MB
    try {
      ctx.video_sock.set_option(boost::asio::socket_base::send_buffer_size(1024 * 1024));
    } catch (...) {
      BOOST_LOG(error) << "Failed to set video socket send buffer size (SO_SENDBUF)";
    }

#ifdef _WIN32
    // Never let a full UDP send queue block the video worker.  The video
    // socket is also used by the input/control path's shared task pool, so a
    // multi-second WSASendMsg stall can make keyboard and mouse input appear
    // dead even when the client connection itself is healthy.
    u_long non_blocking = 1;
    if (ioctlsocket(ctx.video_sock.native_handle(), FIONBIO, &non_blocking) != 0) {
      BOOST_LOG(warning) << "Failed to make video UDP socket non-blocking [error="sv
                         << WSAGetLastError() << ']';
    } else {
      BOOST_LOG(info) << "Video UDP socket configured non-blocking to bound send backpressure"sv;
    }
#endif

    auto bind_addr_str = net::get_bind_address(address_family);
    const auto bind_addr = boost::asio::ip::make_address(bind_addr_str, ec);
    if (ec) {
      BOOST_LOG(fatal) << "Invalid bind address: "sv << bind_addr_str << " - " << ec.message();
      return -1;
    }

    ctx.video_sock.bind(udp::endpoint(bind_addr, video_port), ec);
    if (ec) {
      BOOST_LOG(fatal) << "Couldn't bind Video server to port ["sv << video_port << "]: "sv << ec.message();

      return -1;
    }

    ctx.audio_sock.open(protocol, ec);
    if (ec) {
      BOOST_LOG(fatal) << "Couldn't open socket for Audio server: "sv << ec.message();

      return -1;
    }

    ctx.audio_sock.bind(udp::endpoint(bind_addr, audio_port), ec);
    if (ec) {
      BOOST_LOG(fatal) << "Couldn't bind Audio server to port ["sv << audio_port << "]: "sv << ec.message();

      return -1;
    }

    ctx.message_queue_queue = std::make_shared<message_queue_queue_t::element_type>(30);

    ctx.video_thread = std::thread {videoBroadcastThread, std::ref(ctx.video_sock)};
    ctx.audio_thread = std::thread {audioBroadcastThread, std::ref(ctx.audio_sock)};
    ctx.control_thread = std::thread {controlBroadcastThread, &ctx.control_server};

    ctx.recv_thread = std::thread {recvThread, std::ref(ctx)};

    return 0;
  }

  void end_broadcast(broadcast_ctx_t &ctx) {
    auto broadcast_shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);

    broadcast_shutdown_event->raise(true);

    auto video_packets = mail::man->queue<video::packet_t>(mail::video_packets);
    auto audio_packets = mail::man->queue<audio::packet_t>(mail::audio_packets);

    // Minimize delay stopping video/audio threads
    video_packets->stop();
    audio_packets->stop();

    ctx.message_queue_queue->stop();
    ctx.io_context.stop();

    ctx.video_sock.close();
    ctx.audio_sock.close();

    video_packets.reset();
    audio_packets.reset();

    BOOST_LOG(debug) << "Waiting for main listening thread to end..."sv;
    ctx.recv_thread.join();
    BOOST_LOG(debug) << "Waiting for main video thread to end..."sv;
    ctx.video_thread.join();
    BOOST_LOG(debug) << "Waiting for main audio thread to end..."sv;
    ctx.audio_thread.join();
    BOOST_LOG(debug) << "Waiting for main control thread to end..."sv;
    ctx.control_thread.join();
    BOOST_LOG(debug) << "All broadcasting threads ended"sv;

    broadcast_shutdown_event->reset();
  }

  int recv_ping(session_t *session, decltype(broadcast)::ptr_t ref, socket_e type, std::string_view expected_payload, udp::endpoint &peer, std::chrono::milliseconds timeout) {
    auto messages = std::make_shared<message_queue_t::element_type>(30);
    av_session_id_t session_id = std::string {expected_payload};

    // Only allow matches on the peer address for legacy clients
    if (!(session->config.mlFeatureFlags & ML_FF_SESSION_ID_V1)) {
      ref->message_queue_queue->raise(type, peer.address(), messages);
    }
    ref->message_queue_queue->raise(type, session_id, messages);

    auto fg = util::fail_guard([&]() {
      messages->stop();

      // remove message queue from session
      if (!(session->config.mlFeatureFlags & ML_FF_SESSION_ID_V1)) {
        ref->message_queue_queue->raise(type, peer.address(), nullptr);
      }
      ref->message_queue_queue->raise(type, session_id, nullptr);
    });

    auto start_time = std::chrono::steady_clock::now();
    auto current_time = start_time;

    while (current_time - start_time < config::stream.ping_timeout) {
      auto delta_time = current_time - start_time;

      auto msg_opt = messages->pop(config::stream.ping_timeout - delta_time);
      if (!msg_opt) {
        break;
      }

      TUPLE_2D_REF(recv_peer, msg, *msg_opt);
      if (msg.find(expected_payload) != std::string::npos) {
        // Match the new PING payload format
        BOOST_LOG(debug) << "Received ping [v2] from "sv << recv_peer.address() << ':' << recv_peer.port() << " ["sv << util::hex_vec(msg) << ']';
      } else if (!(session->config.mlFeatureFlags & ML_FF_SESSION_ID_V1) && msg == "PING"sv) {
        // Match the legacy fixed PING payload only if the new type is not supported
        BOOST_LOG(debug) << "Received ping [v1] from "sv << recv_peer.address() << ':' << recv_peer.port() << " ["sv << util::hex_vec(msg) << ']';
      } else {
        BOOST_LOG(debug) << "Received non-ping from "sv << recv_peer.address() << ':' << recv_peer.port() << " ["sv << util::hex_vec(msg) << ']';
        current_time = std::chrono::steady_clock::now();
        continue;
      }

      // Update connection details.
      peer = recv_peer;
      return 0;
    }

    BOOST_LOG(error) << "Initial Ping Timeout"sv;
    return -1;
  }

  void videoThread(session_t *session) {
    platf::set_thread_name("session::video");
    auto fg = util::fail_guard([&]() {
      session::stop(*session);
    });

    while_starting_do_nothing(session->state);

    auto ref = broadcast.ref();
    auto error = recv_ping(session, ref, socket_e::video, session->video.ping_payload, session->video.peer, config::stream.ping_timeout);
    if (error < 0) {
      return;
    }

    // Enable local prioritization and QoS tagging on video traffic if requested by the client
    auto address = session->video.peer.address();
    session->video.qos = platf::enable_socket_qos(ref->video_sock.native_handle(), address, session->video.peer.port(), platf::qos_data_type_e::video, session->config.videoQosType != 0);

    BOOST_LOG(debug) << "Start capturing Video"sv;
    if (video::uses_dual_display_video_tracks(session->config.monitor)) {
      video::capture_dual(session->mail, session->config.monitor, session);
    } else {
      video::capture(session->mail, session->config.monitor, session);
    }
  }

  void audioThread(session_t *session) {
    platf::set_thread_name("session::audio");
    auto fg = util::fail_guard([&]() {
      session::stop(*session);
    });

    while_starting_do_nothing(session->state);

    auto ref = broadcast.ref();
    auto error = recv_ping(session, ref, socket_e::audio, session->audio.ping_payload, session->audio.peer, config::stream.ping_timeout);
    if (error < 0) {
      return;
    }

    // Enable local prioritization and QoS tagging on audio traffic if requested by the client
    auto address = session->audio.peer.address();
    session->audio.qos = platf::enable_socket_qos(ref->audio_sock.native_handle(), address, session->audio.peer.port(), platf::qos_data_type_e::audio, session->config.audioQosType != 0);

    BOOST_LOG(debug) << "Start capturing Audio"sv;
    audio::capture(session->mail, session->config.audio, session);
  }

  namespace session {
    std::atomic_uint running_sessions;
    std::atomic_bool shutdown_in_progress {false};
    std::atomic_int64_t reconnect_block_until_ms {0};

    namespace {
      constexpr auto SLOW_SHUTDOWN_THRESHOLD = 10s;
      constexpr auto SLOW_SHUTDOWN_RECONNECT_COOLDOWN = 5s;
      constexpr auto EMERGENCY_SHUTDOWN_TIMEOUT = 45s;

      std::int64_t steady_now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now().time_since_epoch()
        )
          .count();
      }
    }  // namespace

    state_e state(session_t &session) {
      return session.state.load(std::memory_order_relaxed);
    }

    const std::string &client_cert(session_t &session) {
      return session.client_cert;
    }

    std::string uuid(session_t &session) {
      return session.device_uuid;
    }

    std::optional<std::string> restart_guard_reason() {
      if (shutdown_in_progress.load(std::memory_order_acquire)) {
        return "previous session cleanup is still in progress";
      }

      const auto block_until = reconnect_block_until_ms.load(std::memory_order_acquire);
      const auto remaining_ms = block_until - steady_now_ms();
      if (remaining_ms > 0) {
        return "previous slow video shutdown cooldown has " + std::to_string(remaining_ms) + "ms remaining";
      }

      return std::nullopt;
    }

    void stop(session_t &session) {
      while_starting_do_nothing(session.state);
      auto expected = state_e::RUNNING;
      auto already_stopping = !session.state.compare_exchange_strong(expected, state_e::STOPPING);
      if (already_stopping) {
        return;
      }

      session.shutdown_event->raise(true);
    }

    void join(session_t &session) {
      const auto shutdown_started_at = std::chrono::steady_clock::now();
      shutdown_in_progress.store(true, std::memory_order_release);
      std::atomic<const char *> shutdown_phase {"initializing"};
      auto elapsed_ms = [shutdown_started_at]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - shutdown_started_at).count();
      };
      auto session_duration = std::chrono::duration_cast<std::chrono::seconds>(shutdown_started_at - session.started_at).count();
      BOOST_LOG(info) << "SunshineSeat session shutdown started [client="sv << session.device_name
                      << ", uuid="sv << session.device_uuid
                      << ", duration_s="sv << session_duration << ']';

      auto warn_task = [&session, &shutdown_phase, &elapsed_ms]() {
        BOOST_LOG(warning) << "SunshineSeat session shutdown is slow [client="sv << session.device_name
                           << ", uuid="sv << session.device_uuid
                           << ", phase="sv << shutdown_phase.load(std::memory_order_relaxed)
                           << ", elapsed_ms="sv << elapsed_ms() << ']';
        logging::log_flush();
      };
      auto warn_slow_shutdown = task_pool.pushDelayed(warn_task, 10s).task_id;

      // Current GPU encoders can wedge the encoder thread during teardown. If this happens,
      // terminate ourselves quickly enough for SunshineSeatAgent to restore availability.
      auto task = [&session, &shutdown_phase, &elapsed_ms]() {
        BOOST_LOG(fatal) << "SunshineSeat session shutdown exceeded emergency timeout [client="sv << session.device_name
                         << ", uuid="sv << session.device_uuid
                         << ", phase="sv << shutdown_phase.load(std::memory_order_relaxed)
                         << ", elapsed_ms="sv << elapsed_ms() << ']';
        BOOST_LOG(fatal) << "SUNSHINESEAT_HEALTH=Wedged reason=shutdown_emergency_timeout phase="sv
                         << shutdown_phase.load(std::memory_order_relaxed);
        logging::log_flush();
        std::quick_exit(70);
      };
      auto force_kill = task_pool.pushDelayed(task, EMERGENCY_SHUTDOWN_TIMEOUT).task_id;
      auto fg = util::fail_guard([&warn_slow_shutdown, &force_kill]() {
        // Cancel the kill task if we manage to return from this function
        task_pool.cancel(warn_slow_shutdown);
        task_pool.cancel(force_kill);
        shutdown_in_progress.store(false, std::memory_order_release);
      });

      auto log_phase_done = [&session, &elapsed_ms](const char *phase_name) {
        BOOST_LOG(info) << "SunshineSeat session shutdown phase complete [client="sv << session.device_name
                        << ", phase="sv << phase_name
                        << ", elapsed_ms="sv << elapsed_ms() << ']';
      };

      shutdown_phase.store("video", std::memory_order_relaxed);
      BOOST_LOG(debug) << "Waiting for video to end..."sv;
      session.videoThread.join();
      log_phase_done("video");
      shutdown_phase.store("audio", std::memory_order_relaxed);
      BOOST_LOG(debug) << "Waiting for audio to end..."sv;
      session.audioThread.join();
      log_phase_done("audio");
      shutdown_phase.store("control", std::memory_order_relaxed);
      BOOST_LOG(debug) << "Waiting for control to end..."sv;
      session.controlEnd.view();
      log_phase_done("control");
      // Reset input on session stop to avoid stuck repeated keys
      shutdown_phase.store("input_reset", std::memory_order_relaxed);
      BOOST_LOG(debug) << "Resetting Input..."sv;
      if (!input::reset(session.input)) {
        BOOST_LOG(warning) << "SunshineSeat input reset used direct fallback after queued release timeout"sv;
      }
      log_phase_done("input_reset");

      shutdown_phase.store("client_mic_reset", std::memory_order_relaxed);
      if (session.client_mic != nullptr) {
        session.client_mic->stop_stream();
        session.client_mic.reset();
      }
      log_phase_done("client_mic_reset");

      if (!session.undo_cmds.empty()) {
        shutdown_phase.store("disconnect_hooks", std::memory_order_relaxed);
        auto exec_thread = std::thread([cmd_list = session.undo_cmds] {
          for (const auto &cmd : cmd_list) {
            std::error_code ec;
            auto env = proc::proc.get_env();
            boost::filesystem::path working_dir = proc::find_working_directory(cmd.cmd, env);
            BOOST_LOG(info) << "Spawning Apollo client disconnect command ["sv << cmd.cmd << "] in ["sv << working_dir << ']';
            auto child = platf::run_command(cmd.elevated, true, cmd.cmd, working_dir, env, nullptr, ec, nullptr);
            if (ec) {
              BOOST_LOG(warning) << "Couldn't spawn Apollo client disconnect command ["sv << cmd.cmd << "]: "sv << ec.message();
            } else {
              child.detach();
            }
          }
        });
        exec_thread.detach();
        log_phase_done("disconnect_hooks");
      }

      // If this is the last session, invoke the platform callbacks
      if (--running_sessions == 0) {
        shutdown_phase.store("last_session_cleanup", std::memory_order_relaxed);
        bool revert_display_config {display_device::revert_display_config_on_disconnect(config::video, config::seat, config::sunshineseat)};
        if (proc::proc.running()) {
          proc::proc.pause();
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
          // The application remains available for fast reconnect, but the tray
          // must describe the stream state: there is no active stream now.
          system_tray::update_tray_stream_stopped(proc::proc.get_last_run_app_name());
#endif
        } else {
          // We have no app running and also no clients anymore.
          revert_display_config = true;
        }

        if (revert_display_config) {
          display_device::revert_configuration();
        }

        platf::streaming_will_stop();
        log_phase_done("last_session_cleanup");
      }

      shutdown_phase.store("complete", std::memory_order_relaxed);
      const auto total_elapsed = std::chrono::steady_clock::now() - shutdown_started_at;
      if (total_elapsed >= SLOW_SHUTDOWN_THRESHOLD) {
        const auto block_until = std::chrono::steady_clock::now() + SLOW_SHUTDOWN_RECONNECT_COOLDOWN;
        reconnect_block_until_ms.store(
          std::chrono::duration_cast<std::chrono::milliseconds>(block_until.time_since_epoch()).count(),
          std::memory_order_release
        );
        BOOST_LOG(warning) << "SunshineSeat slow shutdown cooldown armed [client="sv << session.device_name
                           << ", uuid="sv << session.device_uuid
                           << ", cooldown_ms="sv << std::chrono::duration_cast<std::chrono::milliseconds>(SLOW_SHUTDOWN_RECONNECT_COOLDOWN).count()
                           << ", elapsed_ms="sv << elapsed_ms() << ']';
      }
      BOOST_LOG(info) << "SunshineSeat session shutdown complete [client="sv << session.device_name
                      << ", uuid="sv << session.device_uuid
                      << ", elapsed_ms="sv << elapsed_ms() << ']';
    }

    int start(session_t &session, const std::string &addr_string) {
      session.input = input::alloc(session.mail);

      session.broadcast_ref = broadcast.ref();
      if (!session.broadcast_ref) {
        return -1;
      }

      session.control.expected_peer_address = addr_string;
      BOOST_LOG(debug) << "Expecting incoming session connections from "sv << addr_string;

      // Insert this session into the session list
      {
        auto lg = session.broadcast_ref->control_server._sessions.lock();
        session.broadcast_ref->control_server._sessions->push_back(&session);
      }

      auto addr = boost::asio::ip::make_address(addr_string);
      session.video.peer.address(addr);
      session.video.peer.port(0);

      session.audio.peer.address(addr);
      session.audio.peer.port(0);

      session.pingTimeout = std::chrono::steady_clock::now() + config::stream.ping_timeout;

      session.audioThread = std::thread {audioThread, &session};
      session.videoThread = std::thread {videoThread, &session};

      session.state.store(state_e::RUNNING, std::memory_order_relaxed);
      if (session.client_mic != nullptr) {
        session.client_mic->start_stream();
      }

      // If this is the first session, invoke the platform callbacks
      if (++running_sessions == 1) {
        platf::streaming_will_start();
        proc::proc.resume();
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
        system_tray::update_tray_playing(proc::proc.get_last_run_app_name());
#endif
      }

      if (!session.do_cmds.empty()) {
        auto exec_thread = std::thread([cmd_list = session.do_cmds] {
          for (const auto &cmd : cmd_list) {
            std::error_code ec;
            auto env = proc::proc.get_env();
            boost::filesystem::path working_dir = proc::find_working_directory(cmd.cmd, env);
            BOOST_LOG(info) << "Spawning Apollo client connect command ["sv << cmd.cmd << "] in ["sv << working_dir << ']';
            auto child = platf::run_command(cmd.elevated, true, cmd.cmd, working_dir, env, nullptr, ec, nullptr);
            if (ec) {
              BOOST_LOG(warning) << "Couldn't spawn Apollo client connect command ["sv << cmd.cmd << "]: "sv << ec.message();
            } else {
              child.detach();
            }
          }
        });
        exec_thread.detach();
      }

      return 0;
    }

    std::shared_ptr<session_t> alloc(config_t &config, rtsp_stream::launch_session_t &launch_session) {
      auto session = std::make_shared<session_t>();

      auto mail = std::make_shared<safe::mail_raw_t>();

      session->shutdown_event = mail->event<bool>(mail::shutdown);
      session->launch_session_id = launch_session.id;
      session->client_cert = launch_session.client_cert;
      session->client_cert = launch_session.client_cert;
      session->device_name = launch_session.device_name;
      session->device_uuid = launch_session.unique_id;
      session->started_at = std::chrono::steady_clock::now();
      session->permission = launch_session.perm;
      session->do_cmds = launch_session.client_do_cmds;
      session->undo_cmds = launch_session.client_undo_cmds;

      session->config = config;
      session->adaptive_bitrate_ceiling_kbps = std::max(3000, config.monitor.bitrate);
      session->adaptive_bitrate_kbps = std::make_shared<std::atomic<int>>(session->adaptive_bitrate_ceiling_kbps);
      session->config.monitor.adaptive_bitrate_kbps = session->adaptive_bitrate_kbps;
      if (config::sunshineseat.client_mic_enabled && client_mic::allows_routing(config::seat.audio_policy)) {
        session->client_mic = std::make_unique<client_mic::receiver_t>(config::sunshineseat.client_mic_sink);
      }

      session->control.connect_data = launch_session.control_connect_data;
      session->control.feedback_queue = mail->queue<platf::gamepad_feedback_msg_t>(mail::gamepad_feedback);
      session->control.hdr_queue = mail->event<video::hdr_info_t>(mail::hdr);
      session->control.input_timing_ack_queue = mail->queue<input_timing::completed_input_t>(mail::input_timing_ack);
      session->control.legacy_input_enc_iv = launch_session.iv;
      session->control.cipher = crypto::cipher::gcm_t {
        launch_session.gcm_key,
        false
      };

      session->video.idr_events = mail->event<bool>(mail::idr);
      session->video.invalidate_ref_frames_events = mail->event<std::pair<int64_t, int64_t>>(mail::invalidate_ref_frames);
      session->video.ping_payload = launch_session.av_ping_payload;
      for (auto &track : session->video.tracks) {
        track.lowseq = 0;
        track.gcm_iv_counter = 0;
        if (config.encryptionFlagsEnabled & SS_ENC_VIDEO) {
          BOOST_LOG(info) << "Video encryption enabled"sv;
          track.cipher = crypto::cipher::gcm_t {
            launch_session.gcm_key,
            false
          };
        }
      }

      constexpr auto max_block_size = crypto::cipher::round_to_pkcs7_padded(2048);

      util::buffer_t<char> shards {RTPA_TOTAL_SHARDS * max_block_size};
      util::buffer_t<uint8_t *> shards_p {RTPA_TOTAL_SHARDS};

      for (auto x = 0; x < RTPA_TOTAL_SHARDS; ++x) {
        shards_p[x] = (uint8_t *) &shards[x * max_block_size];
      }

      // Audio FEC spans multiple audio packets,
      // therefore its session specific
      session->audio.shards = std::move(shards);
      session->audio.shards_p = std::move(shards_p);

      session->audio.fec_packet.rtp.header = 0x80;
      session->audio.fec_packet.rtp.packetType = 127;
      session->audio.fec_packet.rtp.timestamp = 0;
      session->audio.fec_packet.rtp.ssrc = 0;

      session->audio.fec_packet.fecHeader.payloadType = 97;
      session->audio.fec_packet.fecHeader.ssrc = 0;

      session->audio.cipher = crypto::cipher::cbc_t {
        launch_session.gcm_key,
        true
      };

      session->audio.ping_payload = launch_session.av_ping_payload;
      session->audio.avRiKeyId = util::endian::big(*(std::uint32_t *) launch_session.iv.data());
      session->audio.sequenceNumber = 0;
      session->audio.timestamp = 0;

      session->control.peer = nullptr;
      session->state.store(state_e::STOPPED, std::memory_order_relaxed);

      session->mail = std::move(mail);

      return session;
    }
  }  // namespace session
}  // namespace stream
