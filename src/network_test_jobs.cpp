/**
 * @file src/network_test_jobs.cpp
 * @brief Implementation of bounded network diagnostic jobs and result storage.
 */

// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "network_test_jobs.h"
#include "network_test_provider.h"

#if defined(_WIN32)
  #include <windows.h>
#endif

namespace network_test {

  namespace {

    using json = nlohmann::json;

    constexpr auto route_job_attempt_window = std::chrono::minutes {1};
    constexpr std::size_t maximum_route_job_creations_per_window = 6;

    class system_diagnostic_clock_t final: public diagnostic_clock_t {
    public:
      std::chrono::steady_clock::time_point steady_now() const override {
        return std::chrono::steady_clock::now();
      }

      std::chrono::system_clock::time_point system_now() const override {
        return std::chrono::system_clock::now();
      }
    };

    bool valid_paired_client_id(const std::string_view value) {
      if (value.empty() || value.size() > 128) {
        return false;
      }

      return std::all_of(value.begin(), value.end(), [](const char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-' || character == '_';
      });
    }

    std::string random_token() {
      std::random_device random_device;
      constexpr std::array<char, 16> hexadecimal {
        '0',
        '1',
        '2',
        '3',
        '4',
        '5',
        '6',
        '7',
        '8',
        '9',
        'a',
        'b',
        'c',
        'd',
        'e',
        'f',
      };

      std::string token;
      token.reserve(32);
      for (std::size_t index = 0; index < 8; ++index) {
        const auto value = random_device();
        for (std::size_t shift = 0; shift < 32; shift += 4) {
          token.push_back(hexadecimal[(value >> shift) & 0x0fU]);
        }
      }
      return token;
    }

    std::vector<std::uint8_t> deterministic_payload(const std::string_view job_id, const std::uint64_t bytes) {
      std::uint64_t state = 1469598103934665603ULL;
      for (const auto character : job_id) {
        state ^= static_cast<std::uint8_t>(character);
        state *= 1099511628211ULL;
      }

      std::vector<std::uint8_t> payload(static_cast<std::size_t>(bytes));
      for (auto &byte : payload) {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        byte = static_cast<std::uint8_t>((state * 2685821657736338717ULL) >> 56);
      }
      return payload;
    }

    std::int64_t to_epoch_milliseconds(const std::chrono::system_clock::time_point time) {
      return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
    }

    std::chrono::system_clock::time_point from_epoch_milliseconds(const std::int64_t milliseconds) {
      return std::chrono::system_clock::time_point {std::chrono::milliseconds {milliseconds}};
    }

    bool replace_atomically(const std::filesystem::path &temporary, const std::filesystem::path &target) {
#if defined(_WIN32)
      return MoveFileExW(
               temporary.c_str(),
               target.c_str(),
               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
             ) != 0;
#else
      std::error_code error;
      std::filesystem::rename(temporary, target, error);
      return !error;
#endif
    }

    job_response_t response(const int http_status, std::string error_code = {}) {
      job_response_t result;
      result.http_status = http_status;
      result.error_code = std::move(error_code);
      return result;
    }

    std::string format_rfc3339(const std::chrono::system_clock::time_point time) {
      const auto timestamp = std::chrono::system_clock::to_time_t(time);
      std::tm utc {};
#if defined(_WIN32)
      gmtime_s(&utc, &timestamp);
#else
      gmtime_r(&timestamp, &utc);
#endif
      std::ostringstream output;
      output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
      return output.str();
    }

  }  // namespace

  std::shared_ptr<const diagnostic_clock_t> make_system_diagnostic_clock() {
    return std::make_shared<system_diagnostic_clock_t>();
  }

  diagnostic_jobs_t::diagnostic_jobs_t(
    std::filesystem::path state_file,
    std::shared_ptr<const diagnostic_clock_t> clock,
    stream_state_observer_t stream_state
  ):
      state_file_ {std::move(state_file)},
      clock_ {std::move(clock)},
      stream_state_ {std::move(stream_state)} {
    std::scoped_lock lock {mutex_};
    load_results_locked();
  }

  void diagnostic_jobs_t::expire_jobs_locked() {
    const auto now = clock_->steady_now();
    for (auto iterator = route_jobs_.begin(); iterator != route_jobs_.end();) {
      if (iterator->second.expires_at <= now) {
        active_route_job_by_client_.erase(iterator->second.paired_client_id);
        iterator = route_jobs_.erase(iterator);
      } else {
        ++iterator;
      }
    }
    if (host_public_job_ && host_public_job_->expires_at <= now) {
      if (host_public_cancel_) {
        host_public_cancel_->store(true);
      }
      host_public_job_.reset();
      host_public_cancel_.reset();
    }
  }

  void diagnostic_jobs_t::cancel_for_stream_start_locked() {
    route_jobs_.clear();
    active_route_job_by_client_.clear();
    if (host_public_cancel_) {
      host_public_cancel_->store(true);
    }
    host_public_job_.reset();
    host_public_cancel_.reset();
  }

  job_response_t diagnostic_jobs_t::create_route_job(const std::string_view paired_client_id) {
    std::scoped_lock lock {mutex_};
    if (!valid_paired_client_id(paired_client_id)) {
      return response(400, "invalid_paired_client");
    }
    if (!clock_ || !stream_state_ || stream_state_() != stream_state_e::idle) {
      cancel_for_stream_start_locked();
      return response(409, "stream_active");
    }

    expire_jobs_locked();
    const auto client_id = std::string {paired_client_id};
    if (active_route_job_by_client_.contains(client_id)) {
      return response(409, "route_job_active");
    }
    const auto now = clock_->steady_now();
    auto &attempts = route_job_creation_attempts_[client_id];
    while (!attempts.empty() && attempts.front() <= now - route_job_attempt_window) {
      attempts.pop_front();
    }
    if (attempts.size() >= maximum_route_job_creations_per_window) {
      return response(429, "route_job_rate_limited");
    }
    attempts.push_back(now);

    route_job_record_t record {
      .job_id = "route-" + std::to_string(++job_sequence_) + "-" + random_token(),
      .nonce = random_token(),
      .paired_client_id = client_id,
      .expires_at = clock_->steady_now() + default_limits().workflow_timeout,
    };
    const auto expires_in_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(record.expires_at - clock_->steady_now()).count()
    );
    auto result = response(201);
    result.job = route_job_t {
      record.job_id,
      record.nonce,
      expires_in_ms,
      default_limits(),
    };
    active_route_job_by_client_.emplace(client_id, record.job_id);
    route_jobs_.emplace(record.job_id, std::move(record));
    return result;
  }

  job_response_t diagnostic_jobs_t::validate_route_access_locked(
    const std::string_view paired_client_id,
    const std::string_view job_id,
    const std::string_view nonce
  ) {
    expire_jobs_locked();
    const auto iterator = route_jobs_.find(std::string {job_id});
    if (iterator == route_jobs_.end()) {
      return response(410, "route_job_expired");
    }
    if (iterator->second.paired_client_id != paired_client_id) {
      return response(403, "route_job_owner_mismatch");
    }
    if (iterator->second.nonce != nonce) {
      return response(410, "route_job_expired");
    }
    return response(200);
  }

  job_response_t diagnostic_jobs_t::read_download(
    const std::string_view paired_client_id,
    const std::string_view job_id,
    const std::string_view nonce,
    const std::uint64_t requested_bytes
  ) {
    std::scoped_lock lock {mutex_};
    if (!clock_ || !stream_state_ || stream_state_() != stream_state_e::idle) {
      cancel_for_stream_start_locked();
      return response(409, "stream_active");
    }
    if (requested_bytes == 0 || requested_bytes > default_limits().direction_byte_cap) {
      return response(413, "download_bytes_exceeds_cap");
    }
    auto access = validate_route_access_locked(paired_client_id, job_id, nonce);
    if (access.http_status != 200) {
      return access;
    }
    access.payload = deterministic_payload(job_id, requested_bytes);
    return access;
  }

  job_response_t diagnostic_jobs_t::consume_upload(
    const std::string_view paired_client_id,
    const std::string_view job_id,
    const std::string_view nonce,
    const std::vector<std::uint8_t> &body
  ) {
    std::scoped_lock lock {mutex_};
    if (!clock_ || !stream_state_ || stream_state_() != stream_state_e::idle) {
      cancel_for_stream_start_locked();
      return response(409, "stream_active");
    }
    if (body.empty() || body.size() > default_limits().direction_byte_cap) {
      return response(413, "upload_bytes_exceeds_cap");
    }
    const auto access = validate_route_access_locked(paired_client_id, job_id, nonce);
    if (access.http_status != 200) {
      return access;
    }

    // The bounded request body is intentionally discarded. Payload data must
    // never reach logs, disk, or the persisted latest-result projection.
    return response(204);
  }

  job_response_t diagnostic_jobs_t::ping_route_job(
    const std::string_view paired_client_id,
    const std::string_view job_id,
    const std::string_view nonce
  ) {
    std::scoped_lock lock {mutex_};
    if (!clock_ || !stream_state_ || stream_state_() != stream_state_e::idle) {
      cancel_for_stream_start_locked();
      return response(409, "stream_active");
    }
    const auto access = validate_route_access_locked(paired_client_id, job_id, nonce);
    if (access.http_status != 200) {
      return access;
    }
    return response(204);
  }

  job_response_t diagnostic_jobs_t::cancel_route_job(
    const std::string_view paired_client_id,
    const std::string_view job_id,
    const std::string_view nonce
  ) {
    std::scoped_lock lock {mutex_};
    const auto access = validate_route_access_locked(paired_client_id, job_id, nonce);
    if (access.http_status != 200) {
      return access;
    }
    const auto iterator = route_jobs_.find(std::string {job_id});
    active_route_job_by_client_.erase(iterator->second.paired_client_id);
    route_jobs_.erase(iterator);
    return response(204);
  }

  job_response_t diagnostic_jobs_t::create_host_public_job() {
    std::scoped_lock lock {mutex_};
    if (!clock_ || !stream_state_ || stream_state_() != stream_state_e::idle) {
      cancel_for_stream_start_locked();
      return response(409, "stream_active");
    }
    expire_jobs_locked();
    if (host_public_job_) {
      return response(409, "host_public_job_active");
    }

    route_job_record_t record {
      .job_id = "host-public-" + std::to_string(++job_sequence_) + "-" + random_token(),
      .nonce = {},
      .paired_client_id = {},
      .expires_at = clock_->steady_now() + default_limits().workflow_timeout,
    };
    const auto expires_in_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(record.expires_at - clock_->steady_now()).count()
    );
    auto result = response(202);
    result.job = route_job_t {
      record.job_id,
      {},
      expires_in_ms,
      default_limits(),
    };
    host_public_job_ = std::move(record);
    host_public_cancel_ = std::make_shared<std::atomic_bool>(false);
    return result;
  }

  job_response_t diagnostic_jobs_t::cancel_host_public_job(const std::string_view job_id) {
    std::scoped_lock lock {mutex_};
    expire_jobs_locked();
    if (!host_public_job_ || host_public_job_->job_id != job_id) {
      return response(410, "host_public_job_expired");
    }
    if (host_public_cancel_) {
      host_public_cancel_->store(true);
    }
    host_public_job_.reset();
    host_public_cancel_.reset();
    return response(204);
  }

  job_response_t diagnostic_jobs_t::host_public_job_status(const std::string_view job_id) {
    std::scoped_lock lock {mutex_};
    if (!clock_) {
      return response(500, "clock_unavailable");
    }
    expire_jobs_locked();
    if (host_public_job_ && host_public_job_->job_id == job_id) {
      const auto expires_in_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(host_public_job_->expires_at - clock_->steady_now()).count());
      auto current = response(200);
      current.job = route_job_t {
        host_public_job_->job_id,
        {},
        expires_in_ms,
        default_limits(),
      };
      return current;
    }
    if (host_public_result_ && host_public_result_->result.test_id == job_id) {
      auto completed = response(200);
      completed.result = host_public_result_->result;
      return completed;
    }
    return response(410, "host_public_job_expired");
  }

  job_response_t diagnostic_jobs_t::run_host_public_job(const std::string_view job_id, public_speed_provider_t &provider) {
    std::shared_ptr<std::atomic_bool> cancellation;
    std::shared_ptr<const diagnostic_clock_t> clock;
    stream_state_observer_t stream_state;
    {
      std::scoped_lock lock {mutex_};
      expire_jobs_locked();
      if (!clock_ || !host_public_job_ || host_public_job_->job_id != job_id || !host_public_cancel_) {
        return response(410, "host_public_job_expired");
      }
      cancellation = host_public_cancel_;
      clock = clock_;
      stream_state = stream_state_;
    }

    const auto started_at = clock->system_now();
    auto profile = run_public_speed_profile(provider, "host_public", [&] {
      return cancellation->load() || !stream_state || stream_state() != stream_state_e::idle;
    });
    const auto completed_at = clock->system_now();

    result_t result;
    result.schema_version = current_schema_version;
    result.test_id = std::string {job_id};
    result.kind = result_kind_e::host_public;
    result.actor = actor_e::host;
    result.endpoint_label = "cloudflare_edge";
    result.started_at_utc = format_rfc3339(started_at);
    result.completed_at_utc = format_rfc3339(completed_at);
    result.status = profile.status;
    result.throughput = profile.throughput;
    result.error = profile.error;
    if (!stream_state || stream_state() != stream_state_e::idle) {
      result.status = status_e::blocked_stream_active;
      result.error = error_t {"stream_active", "host_public", "Streaming started before the provider job completed."};
    }

    std::scoped_lock lock {mutex_};
    if (!host_public_job_ || host_public_job_->job_id != job_id || host_public_cancel_ != cancellation) {
      return response(410, "host_public_job_expired");
    }
    if (const auto failure = validate_result(result)) {
      return response(500, *failure);
    }
    const auto previous_result = host_public_result_;
    host_public_result_ = stored_result_t {result, completed_at};
    if (!persist_results_locked()) {
      host_public_result_ = previous_result;
      return response(500, "result_persistence_failed");
    }
    host_public_job_.reset();
    host_public_cancel_.reset();
    return response(204);
  }

  job_response_t diagnostic_jobs_t::submit_client_result(const std::string_view paired_client_id, const result_t &result) {
    std::scoped_lock lock {mutex_};
    if (!clock_ || !valid_paired_client_id(paired_client_id)) {
      return response(400, "invalid_paired_client");
    }
    if (result.actor != actor_e::client || (result.kind != result_kind_e::route && result.kind != result_kind_e::client_public && result.kind != result_kind_e::passive_stream)) {
      return response(400, "invalid_client_result_owner");
    }
    if (const auto failure = validate_result(result)) {
      return response(400, *failure);
    }

    const auto client_id = std::string {paired_client_id};
    const auto previous = client_results_.find(client_id);
    const std::optional<stored_result_t> previous_result = previous == client_results_.end() ? std::nullopt : std::optional<stored_result_t> {previous->second};
    client_results_.insert_or_assign(client_id, stored_result_t {result, clock_->system_now()});
    if (!persist_results_locked()) {
      if (previous_result) {
        client_results_.insert_or_assign(client_id, *previous_result);
      } else {
        client_results_.erase(client_id);
      }
      return response(500, "result_persistence_failed");
    }
    return response(204);
  }

  job_response_t diagnostic_jobs_t::submit_host_public_result(const result_t &result) {
    std::scoped_lock lock {mutex_};
    if (!clock_) {
      return response(500, "clock_unavailable");
    }
    if (result.actor != actor_e::host || result.kind != result_kind_e::host_public) {
      return response(400, "invalid_host_public_result_owner");
    }
    if (const auto failure = validate_result(result)) {
      return response(400, *failure);
    }

    const auto previous_result = host_public_result_;
    host_public_result_ = stored_result_t {result, clock_->system_now()};
    if (!persist_results_locked()) {
      host_public_result_ = previous_result;
      return response(500, "result_persistence_failed");
    }
    return response(204);
  }

  latest_results_t diagnostic_jobs_t::latest_results(const std::string_view paired_client_id) const {
    std::scoped_lock lock {mutex_};
    latest_results_t results;
    if (const auto iterator = client_results_.find(std::string {paired_client_id}); iterator != client_results_.end()) {
      results.client_result = iterator->second.result;
    }
    if (host_public_result_) {
      results.host_public_result = host_public_result_->result;
    }
    return results;
  }

  void diagnostic_jobs_t::remove_paired_client(const std::string_view paired_client_id) {
    std::scoped_lock lock {mutex_};
    client_results_.erase(std::string {paired_client_id});
    route_job_creation_attempts_.erase(std::string {paired_client_id});
    if (const auto active = active_route_job_by_client_.find(std::string {paired_client_id}); active != active_route_job_by_client_.end()) {
      route_jobs_.erase(active->second);
      active_route_job_by_client_.erase(active);
    }
    static_cast<void>(persist_results_locked());
  }

  void diagnostic_jobs_t::remove_all_paired_clients() {
    std::scoped_lock lock {mutex_};
    client_results_.clear();
    route_jobs_.clear();
    active_route_job_by_client_.clear();
    route_job_creation_attempts_.clear();
    static_cast<void>(persist_results_locked());
  }

  void diagnostic_jobs_t::prune_expired_results() {
    std::scoped_lock lock {mutex_};
    if (!clock_) {
      return;
    }
    const auto oldest_allowed = clock_->system_now() - default_limits().retention_period;
    bool changed = false;
    for (auto iterator = client_results_.begin(); iterator != client_results_.end();) {
      if (iterator->second.stored_at < oldest_allowed) {
        iterator = client_results_.erase(iterator);
        changed = true;
      } else {
        ++iterator;
      }
    }
    if (host_public_result_ && host_public_result_->stored_at < oldest_allowed) {
      host_public_result_.reset();
      changed = true;
    }
    if (changed) {
      static_cast<void>(persist_results_locked());
    }
  }

  void diagnostic_jobs_t::cancel_for_stream_start() {
    std::scoped_lock lock {mutex_};
    cancel_for_stream_start_locked();
  }

  bool diagnostic_jobs_t::persist_results_locked() const {
    json root {
      {"schema_version", current_schema_version},
      {"client_results", json::array()},
    };
    for (const auto &[client_id, stored] : client_results_) {
      root["client_results"].push_back({
        {"paired_client_id", client_id},
        {"stored_at_epoch_ms", to_epoch_milliseconds(stored.stored_at)},
        {"result", json::parse(serialize_result(stored.result))},
      });
    }
    if (host_public_result_) {
      root["host_public_result"] = {
        {"stored_at_epoch_ms", to_epoch_milliseconds(host_public_result_->stored_at)},
        {"result", json::parse(serialize_result(host_public_result_->result))},
      };
    }

    std::error_code error;
    const auto parent = state_file_.parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent, error);
      if (error) {
        return false;
      }
    }

    const auto temporary = state_file_.string() + ".tmp-" + random_token();
    {
      std::ofstream output {temporary, std::ios::binary | std::ios::trunc};
      if (!output) {
        return false;
      }
      output << root.dump();
      output.flush();
      if (!output) {
        std::filesystem::remove(temporary, error);
        return false;
      }
    }

    std::filesystem::permissions(
      temporary,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace,
      error
    );
    if (error) {
      std::filesystem::remove(temporary, error);
      return false;
    }
    if (!replace_atomically(temporary, state_file_)) {
      std::filesystem::remove(temporary, error);
      return false;
    }
    return true;
  }

  void diagnostic_jobs_t::load_results_locked() {
    std::error_code exists_error;
    if (!std::filesystem::exists(state_file_, exists_error) || exists_error) {
      return;
    }

    try {
      std::ifstream input {state_file_, std::ios::binary};
      const auto root = json::parse(input);
      if (!root.is_object() || root.value("schema_version", 0) != current_schema_version || !root.contains("client_results") || !root.at("client_results").is_array()) {
        return;
      }

      for (const auto &entry : root.at("client_results")) {
        if (!entry.is_object() || !entry.contains("paired_client_id") || !entry.contains("stored_at_epoch_ms") || !entry.contains("result")) {
          continue;
        }
        const auto client_id = entry.at("paired_client_id").get<std::string>();
        if (!valid_paired_client_id(client_id)) {
          continue;
        }
        std::string failure;
        const auto result = parse_result(entry.at("result").dump(), failure);
        if (!result) {
          continue;
        }
        client_results_.insert_or_assign(client_id, stored_result_t {*result, from_epoch_milliseconds(entry.at("stored_at_epoch_ms").get<std::int64_t>())});
      }

      if (root.contains("host_public_result")) {
        const auto &entry = root.at("host_public_result");
        if (entry.is_object() && entry.contains("stored_at_epoch_ms") && entry.contains("result")) {
          std::string failure;
          const auto result = parse_result(entry.at("result").dump(), failure);
          if (result && result->actor == actor_e::host && result->kind == result_kind_e::host_public) {
            host_public_result_ = stored_result_t {*result, from_epoch_milliseconds(entry.at("stored_at_epoch_ms").get<std::int64_t>())};
          }
        }
      }
    } catch (const std::exception &) {
      client_results_.clear();
      host_public_result_.reset();
    }
  }

}  // namespace network_test
