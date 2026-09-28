/**
 * @file src/network_test_curl_provider.cpp
 * @brief Fixed-endpoint libcurl transport for Cloudflare public speed checks.
 */

// standard includes
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>

// lib includes
#include <curl/curl.h>

// local includes
#include "network_test_provider.h"

namespace network_test {

  namespace {

    constexpr std::string_view cloudflare_download_endpoint = "https://speed.cloudflare.com/__down?bytes=";
    constexpr std::string_view cloudflare_upload_endpoint = "https://speed.cloudflare.com/__up";

    struct transfer_context_t {
      std::uint64_t expected_bytes;
      std::uint64_t transferred_bytes {0};
      std::uint64_t generator_state {1469598103934665603ULL};
      const cancel_observer_t &cancel_requested;
      bool cancelled {false};
    };

    std::uint64_t next_payload_word(std::uint64_t &state) {
      state ^= state >> 12;
      state ^= state << 25;
      state ^= state >> 27;
      return state * 2685821657736338717ULL;
    }

    size_t consume_download(char *, const size_t size, const size_t count, void *const user_data) {
      auto &context = *static_cast<transfer_context_t *>(user_data);
      const auto bytes = size * count;
      if (bytes > context.expected_bytes - context.transferred_bytes) {
        return 0;
      }
      context.transferred_bytes += bytes;
      return bytes;
    }

    size_t discard_response(char *, const size_t size, const size_t count, void *) {
      return size * count;
    }

    size_t generate_upload(char *buffer, const size_t size, const size_t count, void *const user_data) {
      auto &context = *static_cast<transfer_context_t *>(user_data);
      const auto capacity = size * count;
      const auto remaining = context.expected_bytes - context.transferred_bytes;
      const auto bytes = std::min<std::uint64_t>(capacity, remaining);
      for (std::uint64_t index = 0; index < bytes; ++index) {
        buffer[index] = static_cast<char>(next_payload_word(context.generator_state) >> 56);
      }
      context.transferred_bytes += bytes;
      return static_cast<size_t>(bytes);
    }

    int observe_cancellation(
      void *const user_data,
      const curl_off_t,
      const curl_off_t,
      const curl_off_t,
      const curl_off_t
    ) {
      auto &context = *static_cast<transfer_context_t *>(user_data);
      if (context.cancel_requested && context.cancel_requested()) {
        context.cancelled = true;
        return 1;
      }
      return 0;
    }

    std::chrono::milliseconds measured_transfer_duration(CURL *const curl) {
      curl_off_t total_microseconds = 0;
      curl_off_t first_byte_microseconds = 0;
      static_cast<void>(curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME_T, &total_microseconds));
      static_cast<void>(curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME_T, &first_byte_microseconds));
      const auto transfer_microseconds = total_microseconds > first_byte_microseconds ? total_microseconds - first_byte_microseconds : total_microseconds;
      return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::microseconds {transfer_microseconds});
    }

    void configure_common(
      CURL *const curl,
      transfer_context_t &context,
      const public_speed_request_t &request,
      const std::string &url
    ) {
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
      curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 0L);
      curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
      curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
      curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(request.connect_timeout.count() * 1000));
      curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(request.direction_timeout.count() * 1000));
      curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, observe_cancellation);
      curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
    }

  }  // namespace

  public_speed_sample_t curl_public_speed_provider_t::measure(
    const public_speed_request_t &request,
    const cancel_observer_t &cancel_requested
  ) {
    if (request.bytes == 0 || request.bytes > default_limits().direction_byte_cap || request.connect_timeout > default_limits().latency_timeout || request.direction_timeout > default_limits().direction_timeout) {
      return {public_speed_outcome_e::invalid_response, 0, std::chrono::milliseconds {0}};
    }
    if (cancel_requested && cancel_requested()) {
      return {public_speed_outcome_e::cancelled, 0, std::chrono::milliseconds {0}};
    }

    CURL *const curl = curl_easy_init();
    if (!curl) {
      return {public_speed_outcome_e::transport_failure, 0, std::chrono::milliseconds {0}};
    }

    transfer_context_t context {
      request.bytes,
      0,
      1469598103934665603ULL,
      cancel_requested,
      false,
    };
    curl_slist *headers = nullptr;
    std::string url;
    if (request.direction == public_speed_direction_e::download) {
      url = std::string {cloudflare_download_endpoint} + std::to_string(request.bytes);
      configure_common(curl, context, request, url);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, consume_download);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
    } else {
      url = std::string {cloudflare_upload_endpoint};
      configure_common(curl, context, request, url);
      headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
      const auto content_length = "Content-Length: " + std::to_string(request.bytes);
      headers = curl_slist_append(headers, content_length.c_str());
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
      curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(request.bytes));
      curl_easy_setopt(curl, CURLOPT_READFUNCTION, generate_upload);
      curl_easy_setopt(curl, CURLOPT_READDATA, &context);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_response);
    }

    const auto curl_result = curl_easy_perform(curl);
    long http_status = 0;
    static_cast<void>(curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status));
    const auto elapsed = measured_transfer_duration(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (context.cancelled || (curl_result == CURLE_ABORTED_BY_CALLBACK && cancel_requested && cancel_requested())) {
      return {public_speed_outcome_e::cancelled, context.transferred_bytes, elapsed};
    }
    if (curl_result == CURLE_OPERATION_TIMEDOUT) {
      return {public_speed_outcome_e::timeout, context.transferred_bytes, elapsed};
    }
    if (curl_result != CURLE_OK) {
      return {public_speed_outcome_e::transport_failure, context.transferred_bytes, elapsed};
    }
    if (http_status < 200 || http_status >= 300 || context.transferred_bytes != request.bytes) {
      return {public_speed_outcome_e::invalid_response, context.transferred_bytes, elapsed};
    }
    return {public_speed_outcome_e::succeeded, context.transferred_bytes, elapsed};
  }

}  // namespace network_test
