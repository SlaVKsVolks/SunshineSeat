#include "src/dual_display_launch_contract.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {

  using namespace dual_display_launch;

  request_t two_track_request() {
    request_t request;
    request.schema_version = current_schema_version;
    request.session_generation = 17;
    request.display_pair_id = "dual-v1-contract-test";
    request.topology_fingerprint = std::string(64, 'a');
    request.input_mapping_version = current_input_mapping_version;
    request.video_transport = std::string(two_video_tracks_transport);

    request.panes[0] = {
      "pane-a",
      "client-left",
      1920,
      1080,
      60,
      "PixelPerfect",
      "Sdr",
      "Bgra8Unorm",
      "Landscape"
    };
    request.panes[1] = {
      "pane-b",
      "client-right",
      1920,
      1080,
      60,
      "PixelPerfect",
      "Sdr",
      "Bgra8Unorm",
      "Landscape"
    };
    request.video_tracks[0] = {
      0,
      "pane-a",
      video_track_ssrc(0),
      1920,
      1080,
      60,
      8000
    };
    request.video_tracks[1] = {
      1,
      "pane-b",
      video_track_ssrc(1),
      1920,
      1080,
      60,
      8000
    };
    return request;
  }

  observed_pair_t observed_pair() {
    return {
      {{{"pane-a", "host-a"}, {"pane-b", "host-b"}}},
      "2099-01-01T00:00:00Z"
    };
  }

  [[noreturn]] void fail(const char *message) {
    std::cerr << "dual-display-launch-contract: FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }

}  // namespace

int main() {
  using namespace dual_display_launch;

  const auto request = two_track_request();
  std::string failure;
  const auto manifest = make_manifest(request, observed_pair().panes, observed_pair().lease_expires_utc, failure);
  if (!manifest.has_value()) {
    fail(failure.c_str());
  }

  if (manifest->video_transport != two_video_tracks_transport || manifest->video_tracks[0].width != 1920 || manifest->video_tracks[1].height != 1080 || manifest->video_tracks[0].bitrate_kbps != 8000 || manifest->video_tracks[1].bitrate_kbps != 8000 || manifest->video_tracks[0].rtp_ssrc == manifest->video_tracks[1].rtp_ssrc) {
    fail("manifest did not preserve two independent requested-bitrate tracks");
  }

  const auto serialized = serialize_manifest(*manifest);
  if (serialized.find("\"videoTransport\":\"TwoVideoTracks\"") == std::string::npos || serialized.find("\"videoTracks\"") == std::string::npos) {
    fail("serialized manifest omitted the dual-track transport declaration");
  }
  if (serialized.find("SudoVDA") != std::string::npos || serialized.find("contract-test-owner") != std::string::npos) {
    fail("serialized manifest leaked provider-owned capture identity");
  }

  const std::string requestJson = R"json({
    "schemaVersion":1,
    "sessionGeneration":17,
    "displayPairId":"dual-v1-contract-test",
    "topologyFingerprint":"topology-test",
    "inputMappingVersion":1,
    "videoTransport":"TwoVideoTracks",
    "panes":[
      {"logicalIdentity":"pane-a","clientDisplayIdentity":"client-left","width":1920,"height":1080,"refreshRate":60,"scalingPolicy":"PixelPerfect","colorMode":"Sdr","pixelFormat":"Bgra8Unorm","orientation":"Landscape"},
      {"logicalIdentity":"pane-b","clientDisplayIdentity":"client-right","width":1920,"height":1080,"refreshRate":60,"scalingPolicy":"PixelPerfect","colorMode":"Sdr","pixelFormat":"Bgra8Unorm","orientation":"Landscape"}
    ],
    "videoTracks":[
      {"trackId":0,"logicalIdentity":"pane-a","rtpSsrc":1146486785,"width":1920,"height":1080,"refreshRate":60,"bitrateKbps":8000},
      {"trackId":1,"logicalIdentity":"pane-b","rtpSsrc":1146486786,"width":1920,"height":1080,"refreshRate":60,"bitrateKbps":8000}
    ]
  })json";
  const auto parsed = parse_optional_request(true, requestJson, failure);
  if (!parsed.has_value() || parsed->video_transport != two_video_tracks_transport || parsed->panes[1].width != 1920 || parsed->video_tracks[1].bitrate_kbps != 8000) {
    fail("valid dual-track request did not round-trip through the parser");
  }
  if (parse_optional_request(true, "{}", failure).has_value() || failure != "malformed_dual_display_request") {
    fail("incomplete request was not rejected as malformed");
  }
  if (parse_optional_request(true, std::string(maximum_request_bytes + 1, 'x'), failure).has_value() || failure != "dual_display_request_too_large") {
    fail("oversized request was not rejected before parsing");
  }

  auto invalid_request = request;
  invalid_request.video_transport = "UnknownTransport";
  if (validate_request(invalid_request) != "unsupported_video_transport") {
    fail("unknown video transport was accepted");
  }
  invalid_request = request;
  invalid_request.panes[1].width = 1919;
  if (validate_request(invalid_request) != "unsupported_two_video_track_profile") {
    fail("non-1920 dual-track pane width was accepted");
  }
  invalid_request = request;
  invalid_request.panes[1].refresh_rate = 59;
  if (validate_request(invalid_request) != "mixed_refresh_rate") {
    fail("mixed refresh rates were accepted");
  }
  invalid_request = request;
  invalid_request.panes[1].color_mode = "Hdr";
  if (validate_request(invalid_request) != "unsupported_pane_profile") {
    fail("non-SDR pane profile was accepted");
  }

  prepared_pair_t pair;
  pair.manifest = *manifest;
  pair.owner_identity = "contract-test-owner";
  pair.provider_display_names = {"SudoVDA-A", "SudoVDA-B"};
  pair.owner = std::make_shared<prepared_pair_owner_t>();
  if (validate_prepared_pair(pair).has_value() || !prepared_pair_matches_request(pair, request)) {
    fail("valid prepared pair did not match its request");
  }
  if (!validate_dual_track_announcement(pair, 1920, 1080, 60, 2, failure)) {
    fail(failure.c_str());
  }
  if (validate_dual_track_announcement(pair, 1920, 1080, 60, 1, failure) || failure != "dual_display_video_track_count_mismatch") {
    fail("single-track announcement was accepted for a dual-track pair");
  }
  if (validate_dual_track_announcement(pair, 1920, 1080, 59, 2, failure) || failure != "dual_display_sdp_framerate_mismatch") {
    fail("wrong dual-track framerate was accepted");
  }
  auto invalid_pair = pair;
  invalid_pair.manifest.video_tracks[1].rtp_ssrc = video_track_ssrc(0);
  if (validate_dual_track_announcement(invalid_pair, 1920, 1080, 60, 2, failure) || failure != "invalid_prepared_pair_video_tracks") {
    fail("duplicate prepared-pair SSRC was accepted");
  }
  auto mismatched_request = request;
  mismatched_request.panes[1].width = 1919;
  if (prepared_pair_matches_request(pair, mismatched_request)) {
    fail("prepared pair matched a request with different pane geometry");
  }
  if (validate_dual_track_announcement(pair, 3840, 1080, 60, 2, failure)) {
    fail("composite-width announcement was accepted for a dual-track session");
  }

  if (video_packet_track_index(false, -1) != std::optional<std::size_t> {0} || video_packet_track_index(true, 0) != std::optional<std::size_t> {0} || video_packet_track_index(true, 1) != std::optional<std::size_t> {1}) {
    fail("valid video packet track routing was rejected");
  }
  if (video_packet_track_index(true, -1).has_value() || video_packet_track_index(true, static_cast<int>(display_count)).has_value()) {
    fail("out-of-range dual-display video packet track was clamped onto another pane");
  }

  if (video_encoder_bitrate_kbps(16000, false) != 16000 || video_transport_bitrate_kbps(16000, false) != 16000 || adaptive_video_encoder_bitrate_kbps(12000, 16000, false) != 12000) {
    fail("single-display bitrate conversion changed legacy behavior");
  }
  if (video_encoder_bitrate_kbps(16000, true) != 8000 || video_transport_bitrate_kbps(8000, true) != 16000) {
    fail("dual-display bitrate was not split once for encoders and restored for shared transport pacing");
  }
  if (adaptive_video_encoder_bitrate_kbps(8000, 8000, true) != 4000 || adaptive_video_encoder_bitrate_kbps(20000, 8000, true) != 8000 || adaptive_video_encoder_bitrate_kbps(3000, 8000, true) != 3000) {
    fail("dual-display adaptive aggregate bitrate was not bounded and converted per track");
  }
  if (adaptive_video_encoder_bitrate_target_kbps(8000, 8000, true, false).has_value() || adaptive_video_encoder_bitrate_target_kbps(8000, 8000, true, true) != std::optional<int> {4000}) {
    fail("adaptive bitrate target did not honor active encoder reconfiguration support");
  }
  if (video_transport_bitrate_kbps(std::numeric_limits<int>::max(), true) != std::numeric_limits<int>::max()) {
    fail("dual-display aggregate bitrate conversion did not saturate safely");
  }

  std::cout << "dual-display-launch-contract: PASS\n";
  return EXIT_SUCCESS;
}
