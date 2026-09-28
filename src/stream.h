/**
 * @file src/stream.h
 * @brief Declarations for the streaming protocols.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// lib includes
#include <boost/asio.hpp>

// local includes
#include "audio.h"
#include "crypto.h"
#include "stream_protocol.h"
#include "video.h"

namespace stream {
  constexpr auto VIDEO_STREAM_PORT = 9;
  constexpr auto CONTROL_PORT = 10;
  constexpr auto AUDIO_STREAM_PORT = 11;

  struct session_t;

  struct clipboard_message_t {
    std::uint8_t origin;
    std::uint64_t origin_id;
    std::uint64_t content_hash;
    std::string text;
  };

  struct clipboard_image_chunk_t {
    std::uint8_t origin;
    std::uint8_t format;
    std::uint64_t origin_id;
    std::uint64_t content_hash;
    std::uint64_t transfer_id;
    std::uint32_t total_length;
    std::uint32_t offset;
    std::string data;
  };

  struct clipboard_update_t {
    bool is_image = false;
    std::string text;
    std::vector<std::uint8_t> image;
  };

  std::uint64_t clipboard_text_hash(const std::string_view &text);
  std::uint64_t clipboard_image_hash(const std::string_view &image);
  bool decode_clipboard_message(const std::string_view &payload, clipboard_message_t &message);
  bool decode_clipboard_image_chunk(const std::string_view &payload, clipboard_image_chunk_t &chunk);
  bool should_apply_clipboard_update(std::uint64_t incoming_hash, std::uint64_t last_applied_hash);
  bool should_drop_video_packet(bool broadcast_shutdown, bool session_running);
  bool should_release_virtual_display_allocation(std::size_t active_sessions, std::size_t pending_launch_sessions);
  std::optional<int> decode_adaptive_bitrate_request(const std::string_view &payload);
  enum class adaptive_bitrate_action_e : std::uint8_t {
    ignore,
    defer,
    apply,
  };
  adaptive_bitrate_action_e evaluate_adaptive_bitrate_request(
    int current_kbps,
    int target_kbps,
    std::chrono::steady_clock::duration target_age,
    std::chrono::steady_clock::duration time_since_last_apply
  );
  int effective_video_keyframe_fec_percentage(int configured_fec_percentage, bool fallback_active);

  struct config_t {
    audio::config_t audio;
    video::config_t monitor;

    int packetsize;
    int minRequiredFecPackets;
    int mlFeatureFlags;
    int controlProtocolType;
    int audioQosType;
    int videoQosType;

    uint32_t encryptionFlagsEnabled;

    std::optional<int> gcmap;
  };

  namespace session {
    enum class state_e : int {
      STOPPED,  ///< The session is stopped
      STOPPING,  ///< The session is stopping
      STARTING,  ///< The session is starting
      RUNNING,  ///< The session is running
    };

    std::shared_ptr<session_t> alloc(config_t &config, rtsp_stream::launch_session_t &launch_session);
    int start(session_t &session, const std::string &addr_string);
    void stop(session_t &session);
    void join(session_t &session);
    state_e state(session_t &session);
    const std::string &client_cert(session_t &session);
    std::string uuid(session_t &session);
    std::optional<std::string> restart_guard_reason();
  }  // namespace session
}  // namespace stream
