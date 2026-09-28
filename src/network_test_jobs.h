/**
 * @file src/network_test_jobs.h
 * @brief Bounded, paired-client network diagnostic job lifecycle.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// local includes
#include "network_test.h"

namespace network_test {

  class public_speed_provider_t;

  enum class stream_state_e {
    idle,
    active,
    unknown,
  };

  /** Time is injected so expiry and retention have deterministic offline tests. */
  class diagnostic_clock_t {
  public:
    virtual ~diagnostic_clock_t() = default;

    virtual std::chrono::steady_clock::time_point steady_now() const = 0;
    virtual std::chrono::system_clock::time_point system_now() const = 0;
  };

  [[nodiscard]] std::shared_ptr<const diagnostic_clock_t> make_system_diagnostic_clock();

  struct route_job_t {
    std::string job_id;
    std::string nonce;
    std::uint64_t expires_in_ms;
    limits_t limits;
  };

  struct job_response_t {
    int http_status = 500;
    std::string error_code;
    std::optional<route_job_t> job;
    std::optional<std::vector<std::uint8_t>> payload;
    std::optional<result_t> result;
  };

  struct latest_results_t {
    std::optional<result_t> client_result;
    std::optional<result_t> host_public_result;
  };

  /**
   * Owns pending paired route jobs, the single host-public job, and the
   * privacy-safe latest-result store. The HTTP adapter supplies only an
   * already verified paired-device UUID; it never accepts an ID from the
   * request body as authorization.
   */
  class diagnostic_jobs_t {
  public:
    using stream_state_observer_t = std::function<stream_state_e()>;

    diagnostic_jobs_t(
      std::filesystem::path state_file,
      std::shared_ptr<const diagnostic_clock_t> clock,
      stream_state_observer_t stream_state
    );

    job_response_t create_route_job(std::string_view paired_client_id);
    job_response_t read_download(
      std::string_view paired_client_id,
      std::string_view job_id,
      std::string_view nonce,
      std::uint64_t requested_bytes
    );
    job_response_t consume_upload(
      std::string_view paired_client_id,
      std::string_view job_id,
      std::string_view nonce,
      const std::vector<std::uint8_t> &body
    );
    job_response_t ping_route_job(
      std::string_view paired_client_id,
      std::string_view job_id,
      std::string_view nonce
    );
    job_response_t cancel_route_job(
      std::string_view paired_client_id,
      std::string_view job_id,
      std::string_view nonce
    );

    job_response_t create_host_public_job();
    job_response_t cancel_host_public_job(std::string_view job_id);
    job_response_t host_public_job_status(std::string_view job_id);
    job_response_t run_host_public_job(std::string_view job_id, public_speed_provider_t &provider);

    job_response_t submit_client_result(std::string_view paired_client_id, const result_t &result);
    job_response_t submit_host_public_result(const result_t &result);
    latest_results_t latest_results(std::string_view paired_client_id) const;

    void remove_paired_client(std::string_view paired_client_id);
    void remove_all_paired_clients();
    void prune_expired_results();

    /** Releases every throughput lease when a stream-start observer fires. */
    void cancel_for_stream_start();

  private:
    struct route_job_record_t {
      std::string job_id;
      std::string nonce;
      std::string paired_client_id;
      std::chrono::steady_clock::time_point expires_at;
    };

    struct stored_result_t {
      result_t result;
      std::chrono::system_clock::time_point stored_at;
    };

    job_response_t validate_route_access_locked(
      std::string_view paired_client_id,
      std::string_view job_id,
      std::string_view nonce
    );
    void expire_jobs_locked();
    void cancel_for_stream_start_locked();
    bool persist_results_locked() const;
    void load_results_locked();

    std::filesystem::path state_file_;
    std::shared_ptr<const diagnostic_clock_t> clock_;
    stream_state_observer_t stream_state_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, route_job_record_t> route_jobs_;
    std::unordered_map<std::string, std::string> active_route_job_by_client_;
    std::unordered_map<std::string, std::deque<std::chrono::steady_clock::time_point>> route_job_creation_attempts_;
    std::optional<route_job_record_t> host_public_job_;
    std::shared_ptr<std::atomic_bool> host_public_cancel_;
    std::unordered_map<std::string, stored_result_t> client_results_;
    std::optional<stored_result_t> host_public_result_;
    std::uint64_t job_sequence_ = 0;
  };

}  // namespace network_test
