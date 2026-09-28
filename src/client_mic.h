/**
 * @file src/client_mic.h
 * @brief Authenticated client-microphone frame validation and host sink.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace client_mic {
  constexpr std::uint8_t MESSAGE_VERSION = 1;
  constexpr std::uint8_t CODEC_OPUS = 1;
  constexpr std::uint8_t CHANNELS_MONO = 1;
  constexpr std::uint32_t SAMPLE_RATE = 48000;
  constexpr std::uint16_t FRAME_SAMPLES = 960;
  constexpr std::size_t MESSAGE_HEADER_SIZE = 16;
  constexpr std::size_t MAX_PACKET_BYTES = 4096;

  /**
   * @brief Allow client-microphone routing only for the proven host-wide path.
   *
   * Isolated routing requires an endpoint/session-bound backend; fail closed
   * until that protocol owner exists.
   */
  [[nodiscard]] bool allows_routing(std::string_view policy) noexcept;

  struct frame_t {
    std::uint32_t sequence = 0;
    std::uint16_t sample_count = 0;
    std::uint16_t encoded_length = 0;
    std::uint8_t channels = 0;
    std::uint8_t codec = 0;
    std::uint32_t sample_rate = 0;
    std::string_view encoded;
  };

  bool decode_frame(const std::string_view &payload, frame_t &frame);

  /**
   * @brief Determine whether a capture endpoint is the virtual companion of a render sink.
   * @param render_friendly_name The configured render endpoint's friendly name.
   * @param render_device_description The configured render endpoint's device description.
   * @param capture_friendly_name The candidate capture endpoint's friendly name.
   * @param capture_device_description The candidate capture endpoint's device description.
   * @return `true` only for a matching virtual-audio/cable endpoint pair.
   */
  bool is_matching_capture_endpoint(
    std::string_view render_friendly_name,
    std::string_view render_device_description,
    std::string_view capture_friendly_name,
    std::string_view capture_device_description
  );

  /**
   * @brief Determine whether a capture endpoint shares the render endpoint's adapter.
   *
   * The adapter friendly name is stable across user-editable endpoint renames, so
   * this is the matcher used for automatic client-microphone routing.
   */
  bool is_matching_capture_endpoint_with_adapter(
    std::string_view render_friendly_name,
    std::string_view render_device_description,
    std::string_view render_adapter_friendly_name,
    std::string_view capture_friendly_name,
    std::string_view capture_device_description,
    std::string_view capture_adapter_friendly_name
  );

  /**
   * @brief Identify AMD Voice's processed microphone capture endpoint.
   *
   * AMD Voice exposes the processed signal as a separate Windows capture
   * endpoint. Keeping this predicate name/metadata based avoids binding to a
   * device instance ID that can change after a driver or USB re-enumeration.
   */
  bool is_amd_voice_capture_endpoint(
    std::string_view friendly_name,
    std::string_view device_description,
    std::string_view adapter_friendly_name
  );

  /**
   * @brief Identify Krisp's processed microphone capture endpoint.
   *
   * Krisp exposes the processed signal as a separate Windows capture
   * endpoint. Keeping this predicate name/metadata based avoids binding to a
   * device instance ID that can change after an app or driver update.
   */
  bool is_krisp_capture_endpoint(
    std::string_view friendly_name,
    std::string_view device_description,
    std::string_view adapter_friendly_name
  );

  /**
   * @brief Repair a stale virtual capture default while no stream is active.
   *
   * This is a no-op outside Windows. On Windows it only changes the default
   * when the configured virtual cable capture endpoint is still selected and
   * a unique physical microphone can be identified.
   */
  void repair_stale_default_capture(std::string_view sink_id);

  /**
   * @brief Make the best installed call-processed endpoint the idle default.
   *
   * This is called only during host startup, before a client stream owns the
   * temporary Moonlight route. Krisp is preferred when installed; AMD Voice
   * remains the fallback. If neither is available, the existing physical-
   * microphone fallback remains in place.
   */
  void set_preferred_idle_capture(std::string_view sink_id);

  struct metrics_t {
    std::uint64_t received_frames = 0;
    std::uint64_t decoded_frames = 0;
    std::uint64_t dropped_frames = 0;
    std::uint64_t decode_failures = 0;
    std::uint64_t sink_failures = 0;
  };

  /**
   * Bounded host-side sink for client microphone audio. The sink writes to a
   * configured Windows render endpoint. Pairing that endpoint with a virtual
   * cable exposes the signal to host applications as a microphone; no virtual
   * driver is installed or modified by SunshineSeat.
   */
  class receiver_t {
  public:
    explicit receiver_t(std::string sink_id);
    ~receiver_t();

    receiver_t(const receiver_t &) = delete;
    receiver_t &operator=(const receiver_t &) = delete;

    void start_stream();
    void stop_stream();
    bool push_frame(const std::string_view &payload);
    metrics_t metrics() const;

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl_;
  };
}  // namespace client_mic
