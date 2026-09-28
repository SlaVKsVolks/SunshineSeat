/**
 * @file src/dual_display_launch_contract.cpp
 * @brief Pure validation and serialization for the dual-display launch extension.
 */

// standard includes
#include <algorithm>
#include <exception>
#include <limits>
#include <string>
#include <utility>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "dual_display_launch_contract.h"

namespace dual_display_launch {

  namespace {

    bool valid_bounded_text(const std::string &value, const std::size_t maximum) {
      return !value.empty() && value.size() <= maximum;
    }

    bool valid_pane_profile(const request_pane_t &pane) {
      return pane.scaling_policy == "PixelPerfect" || pane.scaling_policy == "Letterbox";
    }

    template<typename T>
    bool has_duplicate(const std::array<T, display_count> &values) {
      return values[0] == values[1];
    }

    std::optional<std::string> validate_manifest_identity(
      const request_t &request,
      const std::array<observed_pane_t, display_count> &observed,
      std::string_view lease_expires_utc
    ) {
      if (lease_expires_utc.empty()) {
        return "missing_lease_expiration";
      }

      for (std::size_t index = 0; index < display_count; ++index) {
        if (!valid_bounded_text(observed[index].host_display_identity, 256)) {
          return "missing_host_display_identity";
        }
        if (observed[index].logical_display_id != request.panes[index].logical_display_id) {
          return "observed_logical_identity_mismatch";
        }
      }

      if (observed[0].host_display_identity == observed[1].host_display_identity) {
        return "duplicate_host_display_identity";
      }

      return std::nullopt;
    }

    void set_parse_failure(std::string &failure, std::string value) {
      failure = std::move(value);
    }

  }  // namespace

  std::optional<std::size_t> video_packet_track_index(
    const bool dual_display_video_tracks,
    const int encoded_track_index
  ) {
    if (!dual_display_video_tracks) {
      return std::size_t {0};
    }
    if (encoded_track_index < 0 ||
        encoded_track_index >= static_cast<int>(display_count)) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(encoded_track_index);
  }

  int video_encoder_bitrate_kbps(
    const int transport_bitrate_kbps,
    const bool dual_display_video_tracks
  ) {
    if (!dual_display_video_tracks) {
      return transport_bitrate_kbps;
    }
    return std::max(1, transport_bitrate_kbps / static_cast<int>(display_count));
  }

  int video_transport_bitrate_kbps(
    const int encoder_bitrate_kbps,
    const bool dual_display_video_tracks
  ) {
    if (!dual_display_video_tracks || encoder_bitrate_kbps <= 0) {
      return encoder_bitrate_kbps;
    }
    constexpr auto track_count = static_cast<int>(display_count);
    if (encoder_bitrate_kbps > std::numeric_limits<int>::max() / track_count) {
      return std::numeric_limits<int>::max();
    }
    return encoder_bitrate_kbps * track_count;
  }

  int adaptive_video_encoder_bitrate_kbps(
    const int requested_transport_bitrate_kbps,
    const int encoder_bitrate_ceiling_kbps,
    const bool dual_display_video_tracks
  ) {
    constexpr int minimum_encoder_bitrate_kbps = 3000;
    const auto encoder_ceiling = std::max(minimum_encoder_bitrate_kbps, encoder_bitrate_ceiling_kbps);
    const auto minimum_transport = video_transport_bitrate_kbps(
      minimum_encoder_bitrate_kbps,
      dual_display_video_tracks
    );
    const auto transport_ceiling = video_transport_bitrate_kbps(
      encoder_ceiling,
      dual_display_video_tracks
    );
    const auto bounded_transport = std::clamp(
      requested_transport_bitrate_kbps,
      minimum_transport,
      transport_ceiling
    );
    return video_encoder_bitrate_kbps(bounded_transport, dual_display_video_tracks);
  }

  std::optional<int> adaptive_video_encoder_bitrate_target_kbps(
    const int requested_transport_bitrate_kbps,
    const int encoder_bitrate_ceiling_kbps,
    const bool dual_display_video_tracks,
    const bool encoder_supports_live_reconfiguration
  ) {
    if (!encoder_supports_live_reconfiguration) {
      return std::nullopt;
    }
    return adaptive_video_encoder_bitrate_kbps(
      requested_transport_bitrate_kbps,
      encoder_bitrate_ceiling_kbps,
      dual_display_video_tracks
    );
  }

  std::optional<std::string> validate_request(const request_t &request) {
    if (request.schema_version != current_schema_version) {
      return "unsupported_schema_version";
    }
    if (request.session_generation == 0) {
      return "invalid_session_generation";
    }
    if (!valid_bounded_text(request.display_pair_id, 256)) {
      return "missing_display_pair_id";
    }
    if (!valid_bounded_text(request.topology_fingerprint, 512)) {
      return "missing_topology_fingerprint";
    }
    if (request.input_mapping_version != current_input_mapping_version) {
      return "unsupported_input_mapping_version";
    }
    if (request.video_transport != composite_transport && request.video_transport != two_video_tracks_transport) {
      return "unsupported_video_transport";
    }

    std::array<std::string, display_count> logical_ids;
    std::array<std::string, display_count> client_ids;
    std::uint64_t composite_width = 0;
    std::uint32_t composite_height = 0;
    for (std::size_t index = 0; index < display_count; ++index) {
      const auto &pane = request.panes[index];
      if (!valid_bounded_text(pane.logical_display_id, 256)) {
        return "missing_logical_display_identity";
      }
      if (!valid_bounded_text(pane.client_display_id, 256)) {
        return "missing_client_display_identity";
      }
      if (pane.width == 0 || pane.width > maximum_canvas_width || pane.height == 0 || pane.height > maximum_canvas_height ||
          pane.refresh_rate < 24 || pane.refresh_rate > 240) {
        return "invalid_pane_profile";
      }
      if (!valid_pane_profile(pane) || pane.color_mode != "Sdr" || pane.pixel_format != "Bgra8Unorm" || pane.orientation != "Landscape") {
        return "unsupported_pane_profile";
      }

      logical_ids[index] = pane.logical_display_id;
      client_ids[index] = pane.client_display_id;
      composite_width += pane.width;
      composite_height = std::max(composite_height, pane.height);
    }

    if (has_duplicate(logical_ids)) {
      return "duplicate_logical_display_identity";
    }
    if (has_duplicate(client_ids)) {
      return "duplicate_client_display_identity";
    }
    if (request.panes[0].refresh_rate != request.panes[1].refresh_rate) {
      return "mixed_refresh_rate";
    }
    if (request.video_transport == two_video_tracks_transport &&
        (request.panes[0].width != 1920 || request.panes[0].height != 1080 ||
         request.panes[1].width != 1920 || request.panes[1].height != 1080 ||
         request.panes[0].refresh_rate != 60)) {
      return "unsupported_two_video_track_profile";
    }
    if (request.video_transport == two_video_tracks_transport) {
      for (std::size_t index = 0; index < display_count; ++index) {
        const auto &track = request.video_tracks[index];
        const auto &pane = request.panes[index];
        if (track.track_id != index ||
            track.logical_display_id != pane.logical_display_id ||
            track.rtp_ssrc != video_track_ssrc(index) ||
            track.width != pane.width ||
            track.height != pane.height ||
            track.refresh_rate != pane.refresh_rate ||
            track.bitrate_kbps < 1000 || track.bitrate_kbps > 100000) {
          return "invalid_video_track";
        }
      }
    }
    if (composite_width > maximum_canvas_width || composite_height > maximum_canvas_height ||
        composite_width > std::numeric_limits<std::uint32_t>::max()) {
      return "composite_dimension_overflow";
    }

    return std::nullopt;
  }

  std::optional<request_t> parse_optional_request(
    const bool present,
    const std::string_view encoded_request,
    std::string &failure
  ) {
    failure.clear();
    if (!present) {
      return std::nullopt;
    }
    if (encoded_request.empty()) {
      set_parse_failure(failure, "malformed_dual_display_request");
      return std::nullopt;
    }
    if (encoded_request.size() > maximum_request_bytes) {
      set_parse_failure(failure, "dual_display_request_too_large");
      return std::nullopt;
    }

    try {
      const auto json = nlohmann::json::parse(encoded_request.begin(), encoded_request.end());
      if (!json.is_object() || !json.contains("schemaVersion")) {
        set_parse_failure(failure, "malformed_dual_display_request");
        return std::nullopt;
      }

      request_t request;
      request.schema_version = json.at("schemaVersion").get<int>();
      if (request.schema_version != current_schema_version) {
        set_parse_failure(failure, "unsupported_schema_version");
        return std::nullopt;
      }
      request.session_generation = json.at("sessionGeneration").get<std::uint64_t>();
      request.display_pair_id = json.at("displayPairId").get<std::string>();
      request.topology_fingerprint = json.at("topologyFingerprint").get<std::string>();
      request.input_mapping_version = json.at("inputMappingVersion").get<int>();
      if (json.contains("videoTransport")) {
        request.video_transport = json.at("videoTransport").get<std::string>();
      }

      const auto &panes = json.at("panes");
      if (!panes.is_array() || panes.size() != display_count) {
        set_parse_failure(failure, "exactly_two_panes_required");
        return std::nullopt;
      }
      for (std::size_t index = 0; index < display_count; ++index) {
        const auto &pane = panes.at(index);
        auto &target = request.panes[index];
        target.logical_display_id = pane.at("logicalIdentity").get<std::string>();
        target.client_display_id = pane.at("clientDisplayIdentity").get<std::string>();
        target.width = pane.at("width").get<std::uint32_t>();
        target.height = pane.at("height").get<std::uint32_t>();
        target.refresh_rate = pane.at("refreshRate").get<std::uint32_t>();
        target.scaling_policy = pane.at("scalingPolicy").get<std::string>();
        target.color_mode = pane.at("colorMode").get<std::string>();
        target.pixel_format = pane.at("pixelFormat").get<std::string>();
        target.orientation = pane.at("orientation").get<std::string>();
      }

      if (request.video_transport == two_video_tracks_transport) {
        const auto &video_tracks = json.at("videoTracks");
        if (!video_tracks.is_array() || video_tracks.size() != display_count) {
          set_parse_failure(failure, "exactly_two_video_tracks_required");
          return std::nullopt;
        }
        for (std::size_t index = 0; index < display_count; ++index) {
          const auto &track = video_tracks.at(index);
          auto &target = request.video_tracks[index];
          target.track_id = track.at("trackId").get<std::uint32_t>();
          target.logical_display_id = track.at("logicalIdentity").get<std::string>();
          target.rtp_ssrc = track.at("rtpSsrc").get<std::uint32_t>();
          target.width = track.at("width").get<std::uint32_t>();
          target.height = track.at("height").get<std::uint32_t>();
          target.refresh_rate = track.at("refreshRate").get<std::uint32_t>();
          target.bitrate_kbps = track.at("bitrateKbps").get<std::uint32_t>();
        }
      }

      if (const auto error = validate_request(request)) {
        set_parse_failure(failure, *error);
        return std::nullopt;
      }
      return request;
    } catch (const nlohmann::json::exception &) {
      set_parse_failure(failure, "malformed_dual_display_request");
      return std::nullopt;
    } catch (const std::exception &) {
      set_parse_failure(failure, "malformed_dual_display_request");
      return std::nullopt;
    }
  }

  std::optional<manifest_t> make_manifest(
    const request_t &request,
    const std::array<observed_pane_t, display_count> &observed,
    const std::string_view lease_expires_utc,
    std::string &failure
  ) {
    failure.clear();
    if (const auto error = validate_request(request)) {
      failure = *error;
      return std::nullopt;
    }
    if (const auto error = validate_manifest_identity(request, observed, lease_expires_utc)) {
      failure = *error;
      return std::nullopt;
    }

    manifest_t manifest;
    manifest.session_generation = request.session_generation;
    manifest.display_pair_id = request.display_pair_id;
    manifest.topology_fingerprint = request.topology_fingerprint;
    manifest.input_mapping_version = request.input_mapping_version;
    manifest.video_transport = request.video_transport;
    manifest.composite_width = request.panes[0].width + request.panes[1].width;
    manifest.composite_height = std::max(request.panes[0].height, request.panes[1].height);
    manifest.lease_expires_utc = std::string {lease_expires_utc};

    std::uint32_t output_x = 0;
    for (std::size_t index = 0; index < display_count; ++index) {
      const auto &request_pane = request.panes[index];
      const auto &observed_pane = observed[index];
      auto &manifest_pane = manifest.panes[index];
      manifest_pane.logical_display_id = request_pane.logical_display_id;
      manifest_pane.host_display_identity = observed_pane.host_display_identity;
      manifest_pane.client_display_id = request_pane.client_display_id;
      manifest_pane.source_x = output_x;
      manifest_pane.source_y = 0;
      manifest_pane.output_x = output_x;
      manifest_pane.output_y = 0;
      manifest_pane.width = request_pane.width;
      manifest_pane.height = request_pane.height;
      manifest_pane.refresh_rate = request_pane.refresh_rate;
      manifest_pane.scaling_policy = request_pane.scaling_policy;
      manifest_pane.color_mode = request_pane.color_mode;
      manifest_pane.pixel_format = request_pane.pixel_format;
      manifest_pane.orientation = request_pane.orientation;
      output_x += request_pane.width;

      if (manifest.video_transport == two_video_tracks_transport) {
        manifest.video_tracks[index] = request.video_tracks[index];
      }
    }

    return manifest;
  }

  std::optional<manifest_t> negotiate(
    const request_t &request,
    const observation_provider_t &provider,
    std::string &failure
  ) {
    failure.clear();
    if (!provider) {
      failure = "host_observation_unavailable";
      return std::nullopt;
    }
    const auto observed = provider(request);
    if (!observed) {
      failure = "host_observation_unavailable";
      return std::nullopt;
    }
    return make_manifest(request, observed->panes, observed->lease_expires_utc, failure);
  }

  std::string serialize_manifest(const manifest_t &manifest) {
    nlohmann::json panes = nlohmann::json::array();
    for (const auto &pane : manifest.panes) {
      panes.push_back({
        {"logicalIdentity", pane.logical_display_id},
        {"hostDisplayIdentity", pane.host_display_identity},
        {"clientDisplayIdentity", pane.client_display_id},
        {"sourceRect", {
          {"x", pane.source_x},
          {"y", pane.source_y},
          {"width", pane.width},
          {"height", pane.height},
        }},
        {"outputRect", {
          {"x", pane.output_x},
          {"y", pane.output_y},
          {"width", pane.width},
          {"height", pane.height},
        }},
        {"width", pane.width},
        {"height", pane.height},
        {"refreshRate", pane.refresh_rate},
        {"scalingPolicy", pane.scaling_policy},
        {"colorMode", pane.color_mode},
        {"pixelFormat", pane.pixel_format},
        {"orientation", pane.orientation},
      });
    }

    nlohmann::json video_tracks = nlohmann::json::array();
    if (manifest.video_transport == two_video_tracks_transport) {
      for (const auto &track : manifest.video_tracks) {
        video_tracks.push_back({
          {"trackId", track.track_id},
          {"logicalIdentity", track.logical_display_id},
          {"rtpSsrc", track.rtp_ssrc},
          {"width", track.width},
          {"height", track.height},
          {"refreshRate", track.refresh_rate},
          {"bitrateKbps", track.bitrate_kbps},
        });
      }
    }

    return nlohmann::json {
      {"schemaVersion", manifest.schema_version},
      {"sessionGeneration", manifest.session_generation},
      {"displayPairId", manifest.display_pair_id},
      {"topologyFingerprint", manifest.topology_fingerprint},
      {"inputMappingVersion", manifest.input_mapping_version},
      {"videoTransport", manifest.video_transport},
      {"layout", manifest.layout},
      {"compositeCanvasWidth", manifest.composite_width},
      {"compositeCanvasHeight", manifest.composite_height},
      {"compositePixelFormat", manifest.composite_pixel_format},
      {"compositeColorMode", manifest.composite_color_mode},
      {"videoTracks", std::move(video_tracks)},
      {"panes", std::move(panes)},
      {"leaseExpiresUtc", manifest.lease_expires_utc},
    }.dump();
  }

  std::optional<std::string> validate_prepared_pair(const prepared_pair_t &pair) {
    const auto &manifest = pair.manifest;
    if (manifest.schema_version != current_schema_version ||
        manifest.session_generation == 0 ||
        !valid_bounded_text(manifest.display_pair_id, 256) ||
        !valid_bounded_text(manifest.topology_fingerprint, 512) ||
        manifest.input_mapping_version != current_input_mapping_version ||
        manifest.composite_width == 0 || manifest.composite_height == 0 ||
        manifest.panes[0].refresh_rate == 0 ||
        manifest.panes[0].refresh_rate != manifest.panes[1].refresh_rate ||
        manifest.panes[0].logical_display_id.empty() ||
        manifest.panes[1].logical_display_id.empty() ||
        manifest.panes[0].logical_display_id == manifest.panes[1].logical_display_id ||
        manifest.panes[0].host_display_identity.empty() ||
        manifest.panes[1].host_display_identity.empty() ||
        manifest.panes[0].host_display_identity == manifest.panes[1].host_display_identity) {
      return "invalid_prepared_pair_manifest";
    }

    if (manifest.video_transport != composite_transport && manifest.video_transport != two_video_tracks_transport) {
      return "invalid_prepared_pair_manifest";
    }
    if (manifest.video_transport == two_video_tracks_transport) {
      for (std::size_t index = 0; index < display_count; ++index) {
        const auto &track = manifest.video_tracks[index];
        if (track.track_id != index ||
            track.rtp_ssrc != video_track_ssrc(index) ||
            track.logical_display_id != manifest.panes[index].logical_display_id ||
            track.width != manifest.panes[index].width ||
            track.height != manifest.panes[index].height ||
            track.refresh_rate != manifest.panes[index].refresh_rate ||
            track.bitrate_kbps == 0) {
          return "invalid_prepared_pair_video_tracks";
        }
      }
      if (manifest.video_tracks[0].rtp_ssrc == manifest.video_tracks[1].rtp_ssrc) {
        return "duplicate_prepared_pair_video_track_ssrc";
      }
    }

    if (!valid_bounded_text(pair.owner_identity, 256)) {
      return "missing_prepared_pair_owner_identity";
    }
    if (!pair.owner) {
      return "missing_prepared_pair_owner";
    }
    if (!valid_bounded_text(pair.provider_display_names[0], 512) ||
        !valid_bounded_text(pair.provider_display_names[1], 512)) {
      return "missing_provider_capture_name";
    }
    if (pair.provider_display_names[0] == pair.provider_display_names[1]) {
      return "duplicate_provider_capture_name";
    }

    return std::nullopt;
  }

  bool prepared_pair_matches_request(const prepared_pair_t &pair, const request_t &request) {
    if (validate_request(request) || validate_prepared_pair(pair)) {
      return false;
    }

    const auto &manifest = pair.manifest;
    if (manifest.session_generation != request.session_generation ||
        manifest.display_pair_id != request.display_pair_id ||
        manifest.topology_fingerprint != request.topology_fingerprint ||
        manifest.input_mapping_version != request.input_mapping_version ||
        manifest.video_transport != request.video_transport) {
      return false;
    }

    for (std::size_t index = 0; index < display_count; ++index) {
      const auto &request_pane = request.panes[index];
      const auto &manifest_pane = manifest.panes[index];
      if (manifest_pane.logical_display_id != request_pane.logical_display_id ||
          manifest_pane.client_display_id != request_pane.client_display_id ||
          manifest_pane.width != request_pane.width ||
          manifest_pane.height != request_pane.height ||
          manifest_pane.refresh_rate != request_pane.refresh_rate) {
        return false;
      }
      if (manifest.video_transport == two_video_tracks_transport) {
        const auto &request_track = request.video_tracks[index];
        const auto &manifest_track = manifest.video_tracks[index];
        if (manifest_track.track_id != request_track.track_id ||
            manifest_track.logical_display_id != request_track.logical_display_id ||
            manifest_track.rtp_ssrc != request_track.rtp_ssrc ||
            manifest_track.width != request_track.width ||
            manifest_track.height != request_track.height ||
            manifest_track.refresh_rate != request_track.refresh_rate ||
            manifest_track.bitrate_kbps != request_track.bitrate_kbps) {
          return false;
        }
      }
    }

    return manifest.composite_width == request.panes[0].width + request.panes[1].width &&
           manifest.composite_height == std::max(request.panes[0].height, request.panes[1].height);
  }

  bool validate_composite_announcement(
    const prepared_pair_t &pair,
    const int width,
    const int height,
    const int framerate,
    std::string &failure
  ) {
    failure.clear();
    if (const auto pair_failure = validate_prepared_pair(pair)) {
      failure = *pair_failure;
      return false;
    }

    const auto &manifest = pair.manifest;
    if (width <= 0 || height <= 0 ||
        static_cast<std::uint32_t>(width) != manifest.composite_width ||
        static_cast<std::uint32_t>(height) != manifest.composite_height) {
      failure = "dual_display_sdp_dimensions_mismatch";
      return false;
    }
    if (framerate <= 0 || static_cast<std::uint32_t>(framerate) != manifest.panes[0].refresh_rate) {
      failure = "dual_display_sdp_framerate_mismatch";
      return false;
    }

    return true;
  }

  bool validate_dual_track_announcement(
    const prepared_pair_t &pair,
    const int width,
    const int height,
    const int framerate,
    const int track_count,
    std::string &failure
  ) {
    failure.clear();
    if (const auto pair_failure = validate_prepared_pair(pair)) {
      failure = *pair_failure;
      return false;
    }

    const auto &manifest = pair.manifest;
    if (manifest.video_transport != two_video_tracks_transport || track_count != static_cast<int>(display_count)) {
      failure = "dual_display_video_track_count_mismatch";
      return false;
    }
    if (width <= 0 || height <= 0 ||
        static_cast<std::uint32_t>(width) != manifest.video_tracks[0].width ||
        static_cast<std::uint32_t>(height) != manifest.video_tracks[0].height) {
      failure = "dual_display_sdp_dimensions_mismatch";
      return false;
    }
    if (framerate <= 0 || static_cast<std::uint32_t>(framerate) != manifest.video_tracks[0].refresh_rate) {
      failure = "dual_display_sdp_framerate_mismatch";
      return false;
    }

    return true;
  }

}  // namespace dual_display_launch
