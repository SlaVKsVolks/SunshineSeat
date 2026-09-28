/**
 * @file src/platform/windows/sudovda_control.h
 * @brief Safe SudoVDA lifecycle planning helpers for SunshineSeat.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// display-device includes
#include <display_device/windows/types.h>

// platform includes
#include <windows.h>

namespace platf::sudovda {

  [[nodiscard]] bool should_release_virtual_display_lease(
    bool graceful_disconnect,
    std::chrono::steady_clock::duration elapsed_since_disconnect,
    std::chrono::steady_clock::duration reconnect_grace
  );

  [[nodiscard]] bool should_schedule_virtual_display_release(
    bool has_cached_allocation,
    bool release_worker_active
  );

  [[nodiscard]] std::optional<display_device::ActiveTopology> make_sudovda_stream_topology(
    const display_device::ActiveTopology &physical_topology,
    std::string_view virtual_device_id
  );

  [[nodiscard]] std::optional<display_device::ActiveTopology> make_sudovda_stream_topology(
    const display_device::ActiveTopology &physical_topology,
    const std::vector<std::string> &virtual_device_ids
  );

  [[nodiscard]] std::optional<display_device::ActiveTopology> make_sudovda_stream_topology(
    const display_device::ActiveTopology &physical_topology,
    const display_device::ActiveTopology &active_virtual_topology,
    const std::vector<std::string> &virtual_device_ids
  );

  [[nodiscard]] bool topology_contains_sudovda_devices(
    const display_device::ActiveTopology &current_topology,
    const display_device::ActiveTopology &required_topology
  );

  [[nodiscard]] std::chrono::milliseconds watchdog_keepalive_interval(UINT timeout_seconds);

  struct protocol_version_t {
    std::uint8_t major;
    std::uint8_t minor;
    std::uint8_t incremental;
    bool test_build;
  };

  struct watchdog_t {
    UINT timeout;
    UINT countdown;
  };

  struct status_t {
    std::vector<std::wstring> device_paths;
    bool opened;
    DWORD open_error;
    bool ping_ok;
    DWORD ping_error;
    std::optional<protocol_version_t> protocol_version;
    DWORD protocol_error;
    bool protocol_compatible;
    std::optional<watchdog_t> watchdog;
    DWORD watchdog_error;
  };

  struct display_request_t {
    UINT width;
    UINT height;
    UINT refresh_rate;
    GUID monitor_guid;
    std::string device_name;
    std::string serial_number;
  };

  struct stream_display_t {
    std::string device_id;
    std::string display_name;
    display_request_t request;
  };

  struct stream_topology_result_t {
    bool applied {false};
    std::vector<std::string> device_ids;
  };

  struct display_mode_t {
    UINT width {0};
    UINT height {0};
    UINT refresh_rate {0};

    friend bool operator==(const display_mode_t &, const display_mode_t &) = default;
  };

  struct enumerated_display_t {
    std::string device_id;
    std::string display_name;
    std::string friendly_name;
    bool active;
    std::string stable_identity;
    display_mode_t mode;
  };

  /**
   * Rebind stream-owned virtual displays to a freshly enumerated Windows
   * inventory. A missing, ambiguous, non-virtual, or duplicate match rejects
   * the complete stream topology instead of reusing a stale device ID.
   */
  [[nodiscard]] std::optional<std::vector<enumerated_display_t>> remap_sudovda_stream_inventory(
    const std::vector<enumerated_display_t> &requested,
    const std::vector<enumerated_display_t> &current
  );

  enum class display_transaction_phase_e {
    snapshot,
    creating,
    verifying,
    streaming,
    restoring,
    restored,
    rollback_required,
    rolled_back
  };

  struct display_restore_target_t {
    enumerated_display_t baseline;
    std::string current_device_id;
  };

  struct display_transaction_record_t {
    display_transaction_phase_e phase {display_transaction_phase_e::snapshot};
    std::vector<enumerated_display_t> physical_displays;
    std::optional<enumerated_display_t> virtual_display;
    display_device::ActiveTopology baseline_topology;
    std::vector<std::string> issues;
    bool healthy {true};
    bool rollback_required {false};
  };

  struct display_restore_resolution_t {
    std::vector<display_restore_target_t> targets;
    std::vector<std::string> issues;
    bool healthy {false};
    bool rollback_required {false};
  };

  struct display_restore_verification_t {
    display_transaction_phase_e phase {display_transaction_phase_e::restored};
    std::vector<std::string> issues;
    bool healthy {false};
    bool refresh_restore_healthy {false};
    bool rollback_required {false};
  };

  enum class lifecycle_operation_e {
    create,
    remove
  };

  enum class lifecycle_mode_e {
    dry_run,
    execute
  };

  struct lifecycle_plan_t {
    lifecycle_operation_e operation;
    lifecycle_mode_e mode;
    status_t status;
    std::vector<std::string> actions;
    std::vector<std::string> blockers;
  };

  struct execution_result_t {
    lifecycle_plan_t plan;
    bool created;
    DWORD create_error;
    std::optional<std::string> device_id;
    std::optional<std::string> display_name;
    std::vector<std::string> provider_display_names;
    bool provider_capturable;
    bool removed;
    DWORD remove_error;
  };

  class virtual_display_allocation_t {
  public:
    virtual_display_allocation_t(
      std::wstring device_path,
      GUID monitor_guid,
      std::string display_name,
      std::string device_id,
      display_request_t request,
      display_device::ActiveTopology baseline_topology,
      HANDLE device_handle,
      bool restore_environment
    );
    virtual_display_allocation_t(const virtual_display_allocation_t &) = delete;
    virtual_display_allocation_t &operator=(const virtual_display_allocation_t &) = delete;
    virtual_display_allocation_t(virtual_display_allocation_t &&) = delete;
    virtual_display_allocation_t &operator=(virtual_display_allocation_t &&) = delete;
    ~virtual_display_allocation_t();

    [[nodiscard]] const std::string &display_name() const;
    [[nodiscard]] bool reapply_stream_topology();
    void start_watchdog_keepalive(UINT timeout_seconds);
    [[nodiscard]] bool remove(DWORD &remove_error);

  private:
    void stop_watchdog_keepalive();

    std::wstring device_path;
    GUID monitor_guid;
    std::string display_name_;
    std::string device_id_;
    display_request_t request_;
    display_device::ActiveTopology baseline_topology_;
    HANDLE device_handle;
    std::jthread watchdog_worker;
    bool active;
    bool baseline_restore_pending;
    bool restore_environment;
  };

  struct allocation_result_t {
    execution_result_t result;
    std::shared_ptr<virtual_display_allocation_t> allocation;
  };

  [[nodiscard]] status_t query_status(bool query_driver);
  [[nodiscard]] lifecycle_plan_t plan_create_virtual_display(const display_request_t &request, lifecycle_mode_e mode);
  [[nodiscard]] lifecycle_plan_t plan_remove_virtual_display(const GUID &monitor_guid, lifecycle_mode_e mode);
  [[nodiscard]] allocation_result_t create_virtual_display_allocation(
    const display_request_t &request,
    bool allow_service_session,
    bool apply_exclusive_topology = true,
    bool restore_environment = true,
    bool require_provider_capturability = true
  );
  [[nodiscard]] stream_topology_result_t apply_sudovda_stream_topology(
    const std::vector<stream_display_t> &displays,
    std::string_view reason
  );
  [[nodiscard]] execution_result_t remove_virtual_display_allocation(const display_request_t &request, bool allow_service_session);
  [[nodiscard]] execution_result_t test_create_virtual_display(const display_request_t &request, bool allow_service_session);
  [[nodiscard]] display_request_t make_default_request(UINT width, UINT height, UINT refresh_rate);
  [[nodiscard]] display_request_t make_persistent_request(UINT width, UINT height, UINT refresh_rate, const std::string &identity);
  [[nodiscard]] std::vector<std::string> capturable_display_names(bool log_missing_output = true);
  [[nodiscard]] std::optional<std::string> find_new_display_device_id(
    const std::vector<enumerated_display_t> &before,
    const std::vector<enumerated_display_t> &after,
    const std::string &requested_device_name
  );
  [[nodiscard]] std::optional<std::string> resolve_device_id_after_display_name_resolution(
    const std::optional<std::string> &resolved_device_id,
    const std::vector<enumerated_display_t> &before,
    const std::vector<enumerated_display_t> &after,
    const std::string &requested_device_name
  );
  [[nodiscard]] bool is_successful_remove_result(bool removed, DWORD remove_error);
  [[nodiscard]] bool capture_requires_dxgi_duplication_probe(std::string_view capture);
  [[nodiscard]] bool requires_provider_capturability_blocker(bool provider_capturable, bool require_provider_capturability);
  [[nodiscard]] bool requires_create_rollback(bool created, bool provider_capturable);
  [[nodiscard]] display_transaction_record_t snapshot_display_transaction(const std::vector<enumerated_display_t> &current);
  [[nodiscard]] display_transaction_record_t snapshot_display_transaction(
    const std::vector<enumerated_display_t> &current,
    const display_device::ActiveTopology &baseline_topology
  );
  [[nodiscard]] display_restore_resolution_t resolve_current_display_targets_for_restore(
    const display_transaction_record_t &record,
    const std::vector<enumerated_display_t> &current
  );
  [[nodiscard]] display_restore_verification_t verify_display_transaction_restored(
    const display_transaction_record_t &record,
    const std::vector<enumerated_display_t> &current
  );
  void transition_display_transaction(display_transaction_record_t &record, display_transaction_phase_e phase);
  [[nodiscard]] display_transaction_record_t require_display_transaction_rollback(
    display_transaction_record_t record,
    std::string issue
  );
  [[nodiscard]] display_transaction_record_t complete_display_transaction_rollback(
    display_transaction_record_t record,
    bool successful
  );
  [[nodiscard]] bool is_tracked_display_name(const std::string &display_name);
  void track_display_name(const std::string &display_name);
  void untrack_display_name(const std::string &display_name);

  void print_status(std::ostream &out, const status_t &status);
  void print_plan(std::ostream &out, const lifecycle_plan_t &plan);
  void print_execution_result(std::ostream &out, const execution_result_t &result);

}  // namespace platf::sudovda
