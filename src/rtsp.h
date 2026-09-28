/**
 * @file src/rtsp.h
 * @brief Declarations for RTSP streaming.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <cstddef>
#include <list>
#include <string>

// local includes
#include "crypto.h"
#include "dual_display_launch_contract.h"
#include "thread_safe.h"

namespace rtsp_stream {
  constexpr auto RTSP_SETUP_PORT = 21;

  struct launch_session_t {
    uint32_t id;

    crypto::aes_t gcm_key;
    crypto::aes_t iv;

    std::string av_ping_payload;
    uint32_t control_connect_data;

    bool host_audio;
    std::string unique_id;
    std::string device_name;
    crypto::PERM perm = crypto::PERM::_all;
    bool input_only = false;
    bool virtual_display = false;
    std::uint32_t scale_factor = 100;
    std::list<crypto::command_entry_t> client_do_cmds;
    std::list<crypto::command_entry_t> client_undo_cmds;
    int width;
    int height;
    int fps;
    int gcmap;
    int appid;
    int surround_info;
    std::string surround_params;
    bool continuous_audio;
    bool enable_hdr;
    bool enable_sops;
    // The authenticated prepared pair is carried by shared ownership from
    // HTTPS through RTSP and the active stream configuration. RTSP only
    // validates and forwards it; it never creates a display pair itself.
    dual_display_launch::prepared_pair_ptr dual_display_pair;
    std::string authenticated_client_identity;

    std::optional<crypto::cipher::gcm_t> rtsp_cipher;
    std::string rtsp_url_scheme;
    uint32_t rtsp_iv_counter;
    std::string client_cert;
    std::string remote_address;
    std::chrono::steady_clock::time_point pending_created_at {};
    std::uint64_t pending_generation = 0;
    std::atomic_bool pending_superseded {false};
  };

  void launch_session_raise(std::shared_ptr<launch_session_t> launch_session);

  /**
   * @brief Clear state for the specified launch session.
   * @param launch_session_id The ID of the session to clear.
   */
  void launch_session_clear(uint32_t launch_session_id);

  /**
   * @brief Get the number of active sessions.
   * @return Count of active sessions.
   */
  int session_count();
  std::size_t pending_launch_session_count();
  bool pending_launch_session_is_current(bool pending_superseded);
  std::list<std::string> get_all_session_uuids();

  /**
   * @brief Terminates all running streaming sessions.
   */
  void terminate_sessions();
  void terminate_sessions_by_cert(std::string_view cert);

  /**
   * @brief Runs the RTSP server loop.
   */
  void start();
}  // namespace rtsp_stream
