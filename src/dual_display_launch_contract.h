/**
 * @file src/dual_display_launch_contract.h
 * @brief Pure validation and serialization for the dual-display launch extension.
 */
#pragma once

// standard includes
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace dual_display_launch {

  constexpr int current_schema_version = 1;
  constexpr int current_input_mapping_version = 1;
  constexpr std::size_t display_count = 2;
  constexpr std::size_t maximum_request_bytes = 16 * 1024;
  constexpr std::uint32_t maximum_canvas_width = 7680;
  constexpr std::uint32_t maximum_canvas_height = 4320;
  inline constexpr std::string_view composite_transport = "Composite";
  inline constexpr std::string_view two_video_tracks_transport = "TwoVideoTracks";

  constexpr std::uint32_t video_track_ssrc(const std::size_t track_id) {
    return 0x44560001U + static_cast<std::uint32_t>(track_id);
  }

  /**
   * @brief Resolve an encoded packet to its transport track without aliasing.
   *
   * Legacy single-display sessions always use track zero. Two-track sessions
   * reject out-of-range encoder metadata instead of clamping it onto another
   * pane's sequence, FEC, SSRC, and cipher state.
   */
  [[nodiscard]] std::optional<std::size_t> video_packet_track_index(
    bool dual_display_video_tracks,
    int encoded_track_index
  );

  /**
   * @brief Convert the client-selected aggregate video bitrate to one encoder.
   *
   * A two-track session has one encoder per display, while the client bitrate
   * remains the aggregate budget for the whole stream.
   */
  [[nodiscard]] int video_encoder_bitrate_kbps(
    int transport_bitrate_kbps,
    bool dual_display_video_tracks
  );

  /**
   * @brief Recover the aggregate pacing rate used by the shared RTP sender.
   *
   * The conversion saturates instead of overflowing on malformed or extreme
   * configuration values.
   */
  [[nodiscard]] int video_transport_bitrate_kbps(
    int encoder_bitrate_kbps,
    bool dual_display_video_tracks
  );

  /**
   * @brief Bound an aggregate adaptive request and return one encoder target.
   */
  [[nodiscard]] int adaptive_video_encoder_bitrate_kbps(
    int requested_transport_bitrate_kbps,
    int encoder_bitrate_ceiling_kbps,
    bool dual_display_video_tracks
  );

  /**
   * @brief Return a converted adaptive target only when the active encoder can
   * actually apply it without disconnecting the stream.
   */
  [[nodiscard]] std::optional<int> adaptive_video_encoder_bitrate_target_kbps(
    int requested_transport_bitrate_kbps,
    int encoder_bitrate_ceiling_kbps,
    bool dual_display_video_tracks,
    bool encoder_supports_live_reconfiguration
  );

  struct request_pane_t {
    std::string logical_display_id;
    std::string client_display_id;
    std::uint32_t width {0};
    std::uint32_t height {0};
    std::uint32_t refresh_rate {0};
    std::string scaling_policy;
    std::string color_mode;
    std::string pixel_format;
    std::string orientation;
  };

  struct video_track_t {
    std::uint32_t track_id {0};
    std::string logical_display_id;
    std::uint32_t rtp_ssrc {0};
    std::uint32_t width {0};
    std::uint32_t height {0};
    std::uint32_t refresh_rate {0};
    std::uint32_t bitrate_kbps {0};
  };

  struct request_t {
    int schema_version {0};
    std::uint64_t session_generation {0};
    std::string display_pair_id;
    std::string topology_fingerprint;
    int input_mapping_version {0};
    std::string video_transport = "Composite";
    std::array<request_pane_t, display_count> panes;
    std::array<video_track_t, display_count> video_tracks;
  };

  struct observed_pane_t {
    std::string logical_display_id;
    std::string host_display_identity;
  };

  struct manifest_pane_t {
    std::string logical_display_id;
    std::string host_display_identity;
    std::string client_display_id;
    std::uint32_t source_x {0};
    std::uint32_t source_y {0};
    std::uint32_t output_x {0};
    std::uint32_t output_y {0};
    std::uint32_t width {0};
    std::uint32_t height {0};
    std::uint32_t refresh_rate {0};
    std::string scaling_policy;
    std::string color_mode;
    std::string pixel_format;
    std::string orientation;
  };

  struct manifest_t {
    int schema_version {current_schema_version};
    std::uint64_t session_generation {0};
    std::string display_pair_id;
    std::string topology_fingerprint;
    int input_mapping_version {current_input_mapping_version};
    std::string video_transport = "Composite";
    std::string layout = "Horizontal";
    std::uint32_t composite_width {0};
    std::uint32_t composite_height {0};
    std::string composite_pixel_format = "Bgra8Unorm";
    std::string composite_color_mode = "Sdr";
    std::array<manifest_pane_t, display_count> panes;
    std::array<video_track_t, display_count> video_tracks;
    std::string lease_expires_utc;
  };

  struct observed_pair_t {
    std::array<observed_pane_t, display_count> panes;
    std::string lease_expires_utc;
  };

  /**
   * @brief Type-erased RAII owner for a host-created dual display pair.
   *
   * This stays platform-neutral so an authenticated launch session and an
   * active stream configuration can retain one Windows pair lease without
   * exposing provider allocation details across platform boundaries.
   */
  class prepared_pair_owner_t {
  public:
    virtual ~prepared_pair_owner_t() = default;
  };

  struct prepared_pair_t {
    // Public data that may be serialized in the authenticated HTTPS response.
    manifest_t manifest;

    // Provider-facing DXGI/SudoVDA names. These must never be serialized to a
    // client or inferred from client-supplied host identities.
    std::array<std::string, display_count> provider_display_names;
    std::string owner_identity;

    // One shared owner keeps the underlying platform pair alive through RTSP,
    // stream configuration copies, encoder reinitialization, and teardown.
    std::shared_ptr<prepared_pair_owner_t> owner;
  };

  using prepared_pair_ptr = std::shared_ptr<prepared_pair_t>;

  using prepared_pair_provider_t = std::function<prepared_pair_ptr(
    const request_t &request,
    std::string_view authenticated_client_identity,
    std::string &failure
  )>;

  // The provider is deliberately an injection seam. An empty provider is a
  // hard failure; the launch path must never invent host display identities.
  using observation_provider_t = std::function<std::optional<observed_pair_t>(const request_t &)>;

  [[nodiscard]] std::optional<std::string> validate_request(const request_t &request);

  [[nodiscard]] std::optional<request_t> parse_optional_request(
    bool present,
    std::string_view encoded_request,
    std::string &failure
  );

  [[nodiscard]] std::optional<manifest_t> make_manifest(
    const request_t &request,
    const std::array<observed_pane_t, display_count> &observed,
    std::string_view lease_expires_utc,
    std::string &failure
  );

  [[nodiscard]] std::optional<manifest_t> negotiate(
    const request_t &request,
    const observation_provider_t &provider,
    std::string &failure
  );

  [[nodiscard]] std::string serialize_manifest(const manifest_t &manifest);

  [[nodiscard]] std::optional<std::string> validate_prepared_pair(const prepared_pair_t &pair);

  [[nodiscard]] bool prepared_pair_matches_request(const prepared_pair_t &pair, const request_t &request);

  /**
   * @brief Validate the composite video profile announced over RTSP/SDP.
   *
   * Lease expiry is intentionally not consulted here. It protects handshake
   * freshness only; an active stream owns its prepared pair until teardown.
   */
  [[nodiscard]] bool validate_composite_announcement(
    const prepared_pair_t &pair,
    int width,
    int height,
    int framerate,
    std::string &failure
  );

  [[nodiscard]] bool validate_dual_track_announcement(
    const prepared_pair_t &pair,
    int width,
    int height,
    int framerate,
    int track_count,
    std::string &failure
  );

}  // namespace dual_display_launch
