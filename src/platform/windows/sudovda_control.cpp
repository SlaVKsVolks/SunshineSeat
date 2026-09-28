/**
 * @file src/platform/windows/sudovda_control.cpp
 * @brief Safe SudoVDA lifecycle planning helpers for SunshineSeat.
 */
// standard includes
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <condition_variable>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

// local includes
#include <display_device/noop_audio_context.h>
#include <display_device/windows/persistent_state.h>
#include <display_device/windows/settings_manager.h>
#include <display_device/windows/win_api_layer.h>
#include <display_device/windows/win_display_device.h>
#include "../../logging.h"
#include "display_reliability.h"
#include "misc.h"
#include "sudovda_control.h"

// platform includes
#include <setupapi.h>

namespace platf::sudovda {
  std::optional<display_device::ActiveTopology> make_sudovda_stream_topology(
    const display_device::ActiveTopology &active_virtual_topology,
    const std::string_view virtual_device_id
  ) {
    return make_sudovda_stream_topology(
      active_virtual_topology,
      std::vector<std::string> {std::string {virtual_device_id}}
    );
  }

  std::optional<display_device::ActiveTopology> make_sudovda_stream_topology(
    const display_device::ActiveTopology &active_virtual_topology,
    const std::vector<std::string> &virtual_device_ids
  ) {
    if (virtual_device_ids.empty()) {
      return std::nullopt;
    }

    std::set<std::string> device_ids;
    auto append_topology = [&device_ids](
                             display_device::ActiveTopology &target,
                             const display_device::ActiveTopology &topology
                           ) {
      for (const auto &group : topology) {
        if (group.empty()) {
          return false;
        }

        for (const auto &device_id : group) {
          if (device_id.empty() || !device_ids.insert(device_id).second) {
            return false;
          }
        }

        target.push_back(group);
      }

      return true;
    };

    display_device::ActiveTopology stream_topology;
    if (!append_topology(stream_topology, active_virtual_topology)) {
      return std::nullopt;
    }

    std::set<std::string> requested_device_ids;
    for (const auto &virtual_device_id : virtual_device_ids) {
      if (virtual_device_id.empty() || !requested_device_ids.insert(virtual_device_id).second) {
        return std::nullopt;
      }
      if (!device_ids.contains(virtual_device_id)) {
        device_ids.insert(virtual_device_id);
        stream_topology.push_back({virtual_device_id});
      }
    }

    return stream_topology;
  }

  std::optional<display_device::ActiveTopology> make_sudovda_stream_topology(
    const display_device::ActiveTopology &,
    const display_device::ActiveTopology &,
    const std::vector<std::string> &virtual_device_ids
  ) {
    // Physical and unrequested virtual paths belong to the restore inventory,
    // not to the topology active while a stream owns its virtual displays.
    return make_sudovda_stream_topology(display_device::ActiveTopology {}, virtual_device_ids);
  }

  bool topology_contains_sudovda_devices(
    const display_device::ActiveTopology &current_topology,
    const display_device::ActiveTopology &required_topology
  ) {
    if (current_topology.empty() || required_topology.empty()) {
      return false;
    }

    std::set<std::string> current_device_ids;
    for (const auto &group : current_topology) {
      for (const auto &device_id : group) {
        if (!device_id.empty()) {
          current_device_ids.insert(device_id);
        }
      }
    }

    std::set<std::string> required_device_ids;
    for (const auto &group : required_topology) {
      for (const auto &device_id : group) {
        if (device_id.empty() || !required_device_ids.insert(device_id).second) {
          return false;
        }
      }
    }

    return std::ranges::all_of(required_device_ids, [&current_device_ids](const auto &device_id) {
      return current_device_ids.contains(device_id);
    });
  }

  bool should_release_virtual_display_lease(
    const bool graceful_disconnect,
    const std::chrono::steady_clock::duration elapsed_since_disconnect,
    const std::chrono::steady_clock::duration reconnect_grace
  ) {
    return graceful_disconnect || elapsed_since_disconnect >= reconnect_grace;
  }

  bool should_schedule_virtual_display_release(
    const bool has_cached_allocation,
    const bool release_worker_active
  ) {
    return has_cached_allocation && !release_worker_active;
  }

  std::chrono::milliseconds watchdog_keepalive_interval(const UINT timeout_seconds) {
    if (timeout_seconds == 0) {
      return std::chrono::milliseconds {0};
    }

    const auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::seconds {timeout_seconds});
    return std::max(std::chrono::milliseconds {1}, timeout / 3);
  }

  namespace {
    std::mutex tracked_display_names_mutex;
    std::vector<std::string> tracked_display_names;

    constexpr DWORD ioctl_add_virtual_display = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS);
    constexpr DWORD ioctl_remove_virtual_display = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS);
    constexpr DWORD ioctl_get_watchdog = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS);
    constexpr DWORD ioctl_driver_ping = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x888, METHOD_BUFFERED, FILE_ANY_ACCESS);
    constexpr DWORD ioctl_get_protocol_version = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8FF, METHOD_BUFFERED, FILE_ANY_ACCESS);

    constexpr protocol_version_t expected_protocol_version {
      0,
      2,
      1,
      true
    };

    constexpr auto display_name_resolution_timeout = std::chrono::seconds(20);
    constexpr auto provider_capturable_timeout = std::chrono::seconds(20);
    constexpr auto poll_interval = std::chrono::milliseconds(100);

    constexpr GUID interface_guid {
      0xe5bcc234,
      0x1e0c,
      0x418a,
      {0xa0, 0xd4, 0xef, 0x8b, 0x75, 0x01, 0x41, 0x4d}
    };

    struct device_info_set_t {
      explicit device_info_set_t(HDEVINFO handle):
          handle {handle} {
      }

      device_info_set_t(const device_info_set_t &) = delete;
      device_info_set_t &operator=(const device_info_set_t &) = delete;

      ~device_info_set_t() {
        if (handle != INVALID_HANDLE_VALUE) {
          SetupDiDestroyDeviceInfoList(handle);
        }
      }

      HDEVINFO handle;
    };

    struct handle_t {
      explicit handle_t(HANDLE handle):
          handle {handle} {
      }

      handle_t(const handle_t &) = delete;
      handle_t &operator=(const handle_t &) = delete;

      ~handle_t() {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
          CloseHandle(handle);
        }
      }

      HANDLE release() {
        return std::exchange(handle, INVALID_HANDLE_VALUE);
      }

      HANDLE handle;
    };

    struct driver_protocol_version_t {
      std::uint8_t major;
      std::uint8_t minor;
      std::uint8_t incremental;
      bool test_build;
    };

    struct driver_protocol_out_t {
      driver_protocol_version_t version;
    };

    struct driver_watchdog_out_t {
      UINT timeout;
      UINT countdown;
    };

    struct virtual_display_add_params_t {
      UINT width;
      UINT height;
      UINT refresh_rate;
      GUID monitor_guid;
      CHAR device_name[14];
      CHAR serial_number[14];
    };

    struct virtual_display_remove_params_t {
      GUID monitor_guid;
    };

    struct virtual_display_add_out_t {
      LUID adapter_luid;
      UINT target_id;
    };

    std::string narrow(const std::wstring &input) {
      if (input.empty()) {
        return {};
      }

      const auto size = WideCharToMultiByte(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
      if (size <= 0) {
        return {};
      }

      std::string output(size, '\0');
      WideCharToMultiByte(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), output.data(), size, nullptr, nullptr);
      return output;
    }

    std::string guid_to_string(const GUID &guid) {
      std::ostringstream out;
      out << std::hex << std::setfill('0')
          << std::setw(8) << guid.Data1 << '-'
          << std::setw(4) << guid.Data2 << '-'
          << std::setw(4) << guid.Data3 << '-'
          << std::setw(2) << static_cast<int>(guid.Data4[0])
          << std::setw(2) << static_cast<int>(guid.Data4[1]) << '-';

      for (int i = 2; i < 8; ++i) {
        out << std::setw(2) << static_cast<int>(guid.Data4[i]);
      }

      return out.str();
    }

    std::string protocol_to_string(const protocol_version_t &version) {
      std::ostringstream out;
      out << static_cast<int>(version.major)
          << '.'
          << static_cast<int>(version.minor)
          << '.'
          << static_cast<int>(version.incremental)
          << (version.test_build ? " test" : " release");
      return out.str();
    }

    bool is_protocol_compatible(const protocol_version_t &other) {
      if (expected_protocol_version.major != other.major) {
        return false;
      }

      return expected_protocol_version.minor <= other.minor;
    }

    bool same_luid(const LUID &left, const LUID &right) {
      return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
    }

    std::string upper_copy(std::string value) {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
      });
      return value;
    }

    bool contains_case_insensitive(const std::string &haystack, const std::string &needle) {
      if (needle.empty()) {
        return true;
      }

      return upper_copy(haystack).find(upper_copy(needle)) != std::string::npos;
    }

    std::vector<enumerated_display_t> enumerate_display_devices() {
      const auto display_device_api = std::make_shared<display_device::WinDisplayDevice>(std::make_shared<display_device::WinApiLayer>());
      const auto devices = display_device_api->enumAvailableDevices();

      std::vector<enumerated_display_t> enumerated_devices;
      enumerated_devices.reserve(devices.size());

      for (const auto &device : devices) {
        std::string stable_identity = device.m_device_id;
        if (device.m_edid) {
          stable_identity = device.m_edid->m_manufacturer_id + ":" + device.m_edid->m_product_code + ":" + std::to_string(device.m_edid->m_serial_number);
        }
        display_mode_t mode {};
        if (device.m_info) {
          mode.width = device.m_info->m_resolution.m_width;
          mode.height = device.m_info->m_resolution.m_height;
          const auto refresh_rate = std::visit([](const auto &value) {
            using value_t = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<value_t, double>) {
              return value;
            } else {
              return value.m_denominator == 0 ? 0.0 : static_cast<double>(value.m_numerator) / value.m_denominator;
            }
          }, device.m_info->m_refresh_rate);
          mode.refresh_rate = static_cast<UINT>(std::lround(refresh_rate));
        }
        enumerated_devices.push_back({
          device.m_device_id,
          device.m_display_name,
          device.m_friendly_name,
          device.m_info.has_value(),
          std::move(stable_identity),
          mode
        });
      }

      return enumerated_devices;
    }

    std::string resolve_display_name_from_device_id(const std::string &device_id) {
      if (device_id.empty()) {
        return {};
      }

      const auto display_device_api = std::make_shared<display_device::WinDisplayDevice>(std::make_shared<display_device::WinApiLayer>());
      return display_device_api->getDisplayName(device_id);
    }

    bool activate_display_device(const std::string &device_id, const display_request_t &request) {
      if (device_id.empty()) {
        return false;
      }

      auto display_device_api = std::make_shared<display_device::WinDisplayDevice>(std::make_shared<display_device::WinApiLayer>());
      display_device::SettingsManager settings_manager {
        display_device_api,
        std::make_shared<display_device::NoopAudioContext>(),
        std::make_unique<display_device::PersistentState>(nullptr),
        {}
      };

      display_device::SingleDisplayConfiguration config;
      config.m_device_id = device_id;
      config.m_device_prep = display_device::SingleDisplayConfiguration::DevicePreparation::EnsureActive;
      config.m_resolution = display_device::Resolution {request.width, request.height};
      config.m_refresh_rate = display_device::Rational {request.refresh_rate, 1};

      return settings_manager.applySettings(config) == display_device::SettingsManagerInterface::ApplyResult::Ok;
    }

    std::optional<bool> current_process_is_local_system() {
      HANDLE raw_token = nullptr;
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
        return std::nullopt;
      }

      handle_t token {raw_token};
      DWORD token_size = 0;
      GetTokenInformation(token.handle, TokenUser, nullptr, 0, &token_size);
      if (token_size == 0) {
        return std::nullopt;
      }

      std::vector<std::byte> token_buffer(token_size);
      if (!GetTokenInformation(token.handle, TokenUser, token_buffer.data(), token_size, &token_size)) {
        return std::nullopt;
      }

      auto *token_user = reinterpret_cast<TOKEN_USER *>(token_buffer.data());

      SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
      PSID local_system_sid = nullptr;
      if (!AllocateAndInitializeSid(
            &nt_authority,
            1,
            SECURITY_LOCAL_SYSTEM_RID,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            &local_system_sid
          )) {
        return std::nullopt;
      }

      const bool is_local_system = EqualSid(token_user->User.Sid, local_system_sid);
      FreeSid(local_system_sid);
      return is_local_system;
    }

    DWORD current_session_id() {
      DWORD session_id = 0;
      if (!ProcessIdToSessionId(GetCurrentProcessId(), &session_id)) {
        return 0;
      }

      return session_id;
    }

    std::vector<std::wstring> enumerate_device_paths() {
      std::vector<std::wstring> device_paths;
      device_info_set_t device_info_set {
        SetupDiGetClassDevsW(&interface_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
      };

      if (device_info_set.handle == INVALID_HANDLE_VALUE) {
        return device_paths;
      }

      SP_DEVICE_INTERFACE_DATA interface_data {};
      interface_data.cbSize = sizeof(interface_data);

      for (DWORD index = 0; SetupDiEnumDeviceInterfaces(device_info_set.handle, nullptr, &interface_guid, index, &interface_data); ++index) {
        DWORD detail_size = 0;
        SetupDiGetDeviceInterfaceDetailW(device_info_set.handle, &interface_data, nullptr, 0, &detail_size, nullptr);

        if (detail_size == 0) {
          continue;
        }

        std::vector<std::byte> detail_buffer(detail_size);
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(detail_buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (SetupDiGetDeviceInterfaceDetailW(device_info_set.handle, &interface_data, detail, detail_size, nullptr, nullptr)) {
          device_paths.emplace_back(detail->DevicePath);
        }
      }

      return device_paths;
    }

    HANDLE open_device(const std::wstring &device_path, DWORD &open_error) {
      HANDLE handle = CreateFileW(
        device_path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
      );

      if (handle == INVALID_HANDLE_VALUE) {
        open_error = GetLastError();
      }

      return handle;
    }

    void add_common_blockers(lifecycle_plan_t &plan) {
      if (plan.status.device_paths.empty()) {
        plan.blockers.emplace_back("SudoVDA device interface is not present.");
      }
      if (!plan.status.opened) {
        plan.blockers.emplace_back("SudoVDA device could not be opened.");
      }
      if (!plan.status.ping_ok) {
        plan.blockers.emplace_back("SudoVDA driver did not answer ping.");
      }
      if (!plan.status.protocol_compatible) {
        plan.blockers.emplace_back("SudoVDA protocol is missing or incompatible.");
      }
    }

    void add_execution_blockers(lifecycle_plan_t &plan, bool allow_service_session) {
      add_common_blockers(plan);

      if (allow_service_session) {
        plan.actions.emplace_back("Service/System/session guard override requested.");
        return;
      }

      const auto is_local_system = current_process_is_local_system();
      if (!is_local_system) {
        plan.blockers.emplace_back("Refusing live SudoVDA IOCTLs because current process identity could not be verified.");
      } else if (*is_local_system) {
        plan.blockers.emplace_back("Refusing live SudoVDA IOCTLs from LocalSystem without --allow-service-session.");
      }

      if (current_session_id() == 0) {
        plan.blockers.emplace_back("Refusing live SudoVDA IOCTLs from session 0 without --allow-service-session.");
      }
    }

    void copy_fixed_string(char (&destination)[14], const std::string &source) {
      std::fill(std::begin(destination), std::end(destination), '\0');
      const auto size = std::min<std::size_t>(source.size(), sizeof(destination) - 1);
      std::copy_n(source.data(), size, destination);
    }

    bool resolve_display_name(const virtual_display_add_out_t &added_display, std::string &display_name) {
      platf::syncThreadDesktop();

      UINT path_count = 0;
      UINT mode_count = 0;
      if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
        return false;
      }

      std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
      std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
      if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) != ERROR_SUCCESS) {
        return false;
      }

      const auto path = std::find_if(paths.begin(), paths.begin() + path_count, [&](const DISPLAYCONFIG_PATH_INFO &candidate) {
        return candidate.targetInfo.id == added_display.target_id && same_luid(candidate.targetInfo.adapterId, added_display.adapter_luid);
      });

      if (path == paths.begin() + path_count) {
        return false;
      }

      DISPLAYCONFIG_SOURCE_DEVICE_NAME source_name {};
      source_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
      source_name.header.size = sizeof(source_name);
      source_name.header.adapterId = added_display.adapter_luid;
      source_name.header.id = path->sourceInfo.id;

      if (DisplayConfigGetDeviceInfo(&source_name.header) != ERROR_SUCCESS) {
        return false;
      }

      display_name = narrow(source_name.viewGdiDeviceName);
      return !display_name.empty();
    }

    bool wait_for_display_name(const virtual_display_add_out_t &added_display, const display_request_t &request, const std::vector<enumerated_display_t> &devices_before_add, std::optional<std::string> &device_id, std::string &display_name) {
      const auto deadline = std::chrono::steady_clock::now() + display_name_resolution_timeout;
      std::optional<std::string> added_device_id;
      bool activation_attempted = false;

      while (std::chrono::steady_clock::now() < deadline) {
        if (resolve_display_name(added_display, display_name)) {
          if (!added_device_id) {
            added_device_id = resolve_device_id_after_display_name_resolution(
              std::nullopt,
              devices_before_add,
              enumerate_display_devices(),
              request.device_name
            );
            if (added_device_id) {
              device_id = added_device_id;
            }
          }
          return true;
        }

        if (!added_device_id) {
          added_device_id = find_new_display_device_id(devices_before_add, enumerate_display_devices(), request.device_name);
          if (added_device_id) {
            device_id = added_device_id;
          }
        }

        if (added_device_id) {
          if (!activation_attempted) {
            activation_attempted = true;
            (void) activate_display_device(*added_device_id, request);
          }

          display_name = resolve_display_name_from_device_id(*added_device_id);
          if (!display_name.empty()) {
            return true;
          }
        }

        std::this_thread::sleep_for(poll_interval);
      }

      return false;
    }

    bool wait_for_provider_capturable(const std::string &display_name, std::vector<std::string> &provider_display_names) {
      const auto deadline = std::chrono::steady_clock::now() + provider_capturable_timeout;
      while (std::chrono::steady_clock::now() < deadline) {
        provider_display_names = capturable_display_names(false);
        if (std::find(provider_display_names.begin(), provider_display_names.end(), display_name) != provider_display_names.end()) {
          return true;
        }

        std::this_thread::sleep_for(poll_interval);
      }

      return false;
    }

    bool remove_virtual_display(HANDLE device, const GUID &monitor_guid, DWORD &remove_error) {
      virtual_display_remove_params_t params {
        monitor_guid
      };

      DWORD bytes_returned = 0;
      const bool success = DeviceIoControl(
        device,
        ioctl_remove_virtual_display,
        &params,
        sizeof(params),
        nullptr,
        0,
        &bytes_returned,
        nullptr
      );

      if (!success) {
        remove_error = GetLastError();
      }

      return success;
    }

    void print_error(std::ostream &out, const char *label, DWORD error) {
      if (error != ERROR_SUCCESS) {
        out << label << ": " << error << '\n';
      }
    }
  }  // namespace

  namespace {
    using namespace std::literals::string_view_literals;

    constexpr int topology_operation_attempts = 5;
    constexpr auto topology_operation_retry_delay = std::chrono::milliseconds {250};
    // SudoVDA removal restores the physical topology synchronously, but the
    // driver can still be finishing the device transition when a client
    // retries. Serialize every create/remove transaction so a retry cannot
    // enumerate half-removed virtual displays and build a conflicting pair.
    std::mutex sudovda_allocation_transition_mutex;

    bool is_virtual_display(const enumerated_display_t &display) {
      return contains_case_insensitive(display.friendly_name, "SUNSEAT") ||
             contains_case_insensitive(display.stable_identity, "SUDOVDA");
    }

    std::optional<display_device::ActiveTopology> capture_physical_topology(
      const std::shared_ptr<display_device::WinDisplayDevice> &display_device_api,
      const std::vector<enumerated_display_t> &devices,
      const bool allow_inactive_recovery
    ) {
      if (!display_device_api) {
        return std::nullopt;
      }

      const auto current_topology = display_device_api->getCurrentTopology();
      if (!display_device_api->isTopologyValid(current_topology)) {
        return std::nullopt;
      }

      std::set<std::string> active_physical_ids;
      std::set<std::string> all_physical_ids;
      for (const auto &display : devices) {
        if (is_virtual_display(display)) {
          continue;
        }
        all_physical_ids.insert(display.device_id);
        if (display.active) {
          active_physical_ids.insert(display.device_id);
        }
      }

      display_device::ActiveTopology physical_topology;
      for (const auto &group : current_topology) {
        std::vector<std::string> physical_group;
        for (const auto &device_id : group) {
          if (all_physical_ids.contains(device_id)) {
            physical_group.push_back(device_id);
          }
        }
        if (!physical_group.empty()) {
          physical_topology.push_back(std::move(physical_group));
        }
      }

      std::set<std::string> captured_physical_ids;
      for (const auto &group : physical_topology) {
        captured_physical_ids.insert(group.begin(), group.end());
      }
      if (!active_physical_ids.empty() && captured_physical_ids == active_physical_ids) {
        return physical_topology;
      }

      if (!allow_inactive_recovery || all_physical_ids.empty()) {
        return std::nullopt;
      }

      display_device::ActiveTopology fallback_topology;
      for (const auto &device_id : all_physical_ids) {
        fallback_topology.push_back({device_id});
      }
      if (!display_device_api->isTopologyValid(fallback_topology)) {
        return std::nullopt;
      }

      return fallback_topology;
    }

    bool restore_physical_topology(const display_device::ActiveTopology &baseline_topology, std::string_view reason) {
      if (baseline_topology.empty()) {
        BOOST_LOG(error) << "SudoVDA cannot restore an empty physical topology [reason=" << reason << ']';
        return false;
      }

      const auto display_device_api = std::make_shared<display_device::WinDisplayDevice>(std::make_shared<display_device::WinApiLayer>());
      if (!display_device_api->isTopologyValid(baseline_topology)) {
        BOOST_LOG(error) << "SudoVDA baseline physical topology is invalid [reason=" << reason << ']';
        return false;
      }

      for (int attempt = 0; attempt < topology_operation_attempts; ++attempt) {
        const auto current_topology = display_device_api->getCurrentTopology();
        const bool current_topology_populated = !current_topology.empty();
        const bool current_topology_valid = current_topology_populated &&
                                             display_device_api->isTopologyValid(current_topology);
        if (current_topology_valid &&
            display_device_api->isTopologyTheSame(current_topology, baseline_topology)) {
          return true;
        }

        if (display_reliability::should_attempt_sudovda_topology_set(current_topology_populated) &&
            display_device_api->setTopology(baseline_topology)) {
          const auto updated_topology = display_device_api->getCurrentTopology();
          if (display_device_api->isTopologyValid(updated_topology) &&
              display_device_api->isTopologyTheSame(updated_topology, baseline_topology)) {
            return true;
          }
        } else if (!current_topology_populated) {
          BOOST_LOG(debug) << "SudoVDA deferring physical topology restore until current topology is populated [reason=" << reason << ']';
        }

        if (attempt + 1 < topology_operation_attempts) {
          std::this_thread::sleep_for(topology_operation_retry_delay);
        }
      }

      BOOST_LOG(error) << "SudoVDA failed to restore physical topology [reason=" << reason << ']';
      return false;
    }

    stream_topology_result_t apply_stream_topology_for_displays(
      const std::vector<stream_display_t> &displays,
      std::string_view reason
    ) {
      stream_topology_result_t result;
      if (displays.empty()) {
        return result;
      }

      std::vector<enumerated_display_t> requested_inventory;
      requested_inventory.reserve(displays.size());
      std::set<std::string> requested_display_names;
      bool requested_inventory_valid = true;
      for (const auto &display : displays) {
        if (display.device_id.empty() ||
            display.display_name.empty() ||
            !requested_display_names.insert(display.display_name).second) {
          requested_inventory_valid = false;
        }
        requested_inventory.push_back({
          display.device_id,
          display.display_name,
          {},
          true,
          {},
          {}
        });
      }
      if (!requested_inventory_valid) {
        BOOST_LOG(error) << "SudoVDA refused to build an exclusive virtual stream topology [reason=" << reason
                         << ", cause=invalid_requested_inventory]";
        return result;
      }

      const auto display_device_api = std::make_shared<display_device::WinDisplayDevice>(std::make_shared<display_device::WinApiLayer>());
      for (int attempt = 0; attempt < topology_operation_attempts; ++attempt) {
        const auto current_devices = enumerate_display_devices();
        const auto remapped_inventory = remap_sudovda_stream_inventory(requested_inventory, current_devices);
        if (!remapped_inventory) {
          BOOST_LOG(warning) << "SudoVDA display inventory did not contain a complete exclusive virtual stream topology [reason="
                             << reason << ", attempt=" << attempt + 1 << '/' << topology_operation_attempts << ']';
          if (attempt + 1 < topology_operation_attempts) {
            std::this_thread::sleep_for(topology_operation_retry_delay);
          }
          continue;
        }

        const auto physical_topology = capture_physical_topology(display_device_api, current_devices, true);
        display_device::ActiveTopology active_virtual_topology;
        for (const auto &display : current_devices) {
          if (is_virtual_display(display) &&
              is_tracked_display_name(display.display_name) &&
              !display.device_id.empty()) {
            active_virtual_topology.push_back({display.device_id});
          }
        }

        std::vector<std::string> virtual_device_ids;
        virtual_device_ids.reserve(remapped_inventory->size());
        display_device::DeviceDisplayModeMap requested_modes;
        for (std::size_t index = 0; index < remapped_inventory->size(); ++index) {
          const auto &current_display = (*remapped_inventory)[index];
          virtual_device_ids.push_back(current_display.device_id);
          requested_modes.emplace(
            current_display.device_id,
            display_device::DisplayMode {
              display_device::Resolution {displays[index].request.width, displays[index].request.height},
              display_device::Rational {displays[index].request.refresh_rate, 1}
            }
          );
        }
        const auto stream_topology = physical_topology ?
                                       make_sudovda_stream_topology(
                                         *physical_topology,
                                         active_virtual_topology,
                                         virtual_device_ids
                                       ) :
                                       std::nullopt;
        if (!stream_topology) {
          BOOST_LOG(warning) << "SudoVDA display inventory was not stable while building exclusive virtual stream topology [reason="
                             << reason << ", attempt=" << attempt + 1 << '/'
                             << topology_operation_attempts
                             << ", physical_topology_available=" << (physical_topology ? "yes" : "no")
                             << ", active_virtual_group_count=" << active_virtual_topology.size() << ']';
          if (attempt + 1 < topology_operation_attempts) {
            std::this_thread::sleep_for(topology_operation_retry_delay);
          }
          continue;
        }

        auto current_topology = display_device_api->getCurrentTopology();
        if (!display_device_api->isTopologyValid(current_topology) ||
            !topology_contains_sudovda_devices(current_topology, *stream_topology) ||
            !display_device_api->isTopologyTheSame(current_topology, *stream_topology)) {
          if (!display_device_api->setTopology(*stream_topology)) {
            if (attempt + 1 < topology_operation_attempts) {
              std::this_thread::sleep_for(topology_operation_retry_delay);
            }
            continue;
          }
          current_topology = display_device_api->getCurrentTopology();
        }

        if (display_device_api->isTopologyValid(current_topology) &&
            topology_contains_sudovda_devices(current_topology, *stream_topology) &&
            display_device_api->isTopologyTheSame(current_topology, *stream_topology) &&
            display_device_api->setDisplayModes(requested_modes)) {
          result.applied = true;
          result.device_ids = std::move(virtual_device_ids);
          return result;
        }

        if (attempt + 1 < topology_operation_attempts) {
          std::this_thread::sleep_for(topology_operation_retry_delay);
        }
      }

      BOOST_LOG(error) << "SudoVDA failed to apply exclusive virtual stream topology [reason=" << reason << ']';
      return result;
    }

    stream_topology_result_t apply_stream_topology(
      const std::string &virtual_device_id,
      const std::string &display_name,
      const display_request_t &request,
      std::string_view reason
    ) {
      return apply_stream_topology_for_displays(
        {stream_display_t {virtual_device_id, display_name, request}},
        reason
      );
    }

    const enumerated_display_t *find_unique_restore_match(
      const enumerated_display_t &baseline,
      const std::vector<enumerated_display_t> &current,
      std::vector<std::string> &issues
    ) {
      std::vector<const enumerated_display_t *> candidates;
      if (!baseline.stable_identity.empty()) {
        for (const auto &display : current) {
          if (!display.stable_identity.empty() && display.stable_identity == baseline.stable_identity) {
            candidates.push_back(&display);
          }
        }
      }
      if (candidates.empty() && !baseline.display_name.empty()) {
        for (const auto &display : current) {
          if (display.display_name == baseline.display_name) {
            candidates.push_back(&display);
          }
        }
      }
      if (candidates.size() == 1) {
        return candidates.front();
      }
      if (candidates.empty()) {
        issues.emplace_back("missing_restore_target");
      } else {
        issues.emplace_back("ambiguous_restore_target");
      }
      return nullptr;
    }

    bool is_recorded_virtual_display(const display_transaction_record_t &record, const enumerated_display_t &display) {
      if (!record.virtual_display) {
        return false;
      }
      const auto &virtual_display = *record.virtual_display;
      return (!virtual_display.stable_identity.empty() && display.stable_identity == virtual_display.stable_identity) ||
             (!virtual_display.device_id.empty() && display.device_id == virtual_display.device_id) ||
             (!virtual_display.friendly_name.empty() && display.friendly_name == virtual_display.friendly_name) ||
             (!virtual_display.display_name.empty() && display.display_name == virtual_display.display_name);
    }
  }

  std::optional<std::vector<enumerated_display_t>> remap_sudovda_stream_inventory(
    const std::vector<enumerated_display_t> &requested,
    const std::vector<enumerated_display_t> &current
  ) {
    if (requested.empty()) {
      return std::nullopt;
    }

    std::vector<enumerated_display_t> remapped;
    remapped.reserve(requested.size());
    std::set<std::string> remapped_device_ids;
    std::set<std::string> requested_display_names;
    for (const auto &requested_display : requested) {
      if (requested_display.device_id.empty() ||
          requested_display.display_name.empty() ||
          !requested_display_names.insert(requested_display.display_name).second) {
        return std::nullopt;
      }

      const enumerated_display_t *match = nullptr;
      for (const auto &current_display : current) {
        if (!is_virtual_display(current_display) ||
            current_display.device_id.empty() ||
            current_display.display_name != requested_display.display_name) {
          continue;
        }
        if (match != nullptr) {
          return std::nullopt;
        }
        match = &current_display;
      }
      if (match == nullptr || !remapped_device_ids.insert(match->device_id).second) {
        return std::nullopt;
      }
      remapped.push_back(*match);
    }

    return remapped;
  }

  stream_topology_result_t apply_sudovda_stream_topology(
    const std::vector<stream_display_t> &displays,
    std::string_view reason
  ) {
    return apply_stream_topology_for_displays(displays, reason);
  }

  display_transaction_record_t snapshot_display_transaction(const std::vector<enumerated_display_t> &current) {
    display_transaction_record_t record;
    record.physical_displays.reserve(current.size());
    for (const auto &display : current) {
      if (!display.friendly_name.empty() && display.friendly_name.find("SUNSEAT") != std::string::npos) {
        record.virtual_display = display;
      } else {
        record.physical_displays.push_back(display);
      }
    }
    record.phase = display_transaction_phase_e::snapshot;
    record.healthy = true;
    return record;
  }

  display_transaction_record_t snapshot_display_transaction(
    const std::vector<enumerated_display_t> &current,
    const display_device::ActiveTopology &baseline_topology
  ) {
    auto record = snapshot_display_transaction(current);
    record.baseline_topology = baseline_topology;
    return record;
  }

  display_restore_resolution_t resolve_current_display_targets_for_restore(
    const display_transaction_record_t &record,
    const std::vector<enumerated_display_t> &current
  ) {
    display_restore_resolution_t resolution;
    for (const auto &baseline : record.physical_displays) {
      const auto *match = find_unique_restore_match(baseline, current, resolution.issues);
      if (match) {
        resolution.targets.push_back({baseline, match->device_id});
      }
    }
    resolution.healthy = resolution.issues.empty() && resolution.targets.size() == record.physical_displays.size();
    resolution.rollback_required = !resolution.healthy;
    return resolution;
  }

  display_restore_verification_t verify_display_transaction_restored(
    const display_transaction_record_t &record,
    const std::vector<enumerated_display_t> &current
  ) {
    display_restore_verification_t verification;
    verification.phase = display_transaction_phase_e::restored;
    for (const auto &display : current) {
      if (is_recorded_virtual_display(record, display)) {
        verification.issues.emplace_back("virtual_display_remaining");
      }
    }
    const auto resolution = resolve_current_display_targets_for_restore(record, current);
    verification.issues.insert(verification.issues.end(), resolution.issues.begin(), resolution.issues.end());
    verification.refresh_restore_healthy = resolution.healthy;
    for (const auto &target : resolution.targets) {
      const auto match = std::find_if(current.begin(), current.end(), [&](const auto &display) {
        return display.device_id == target.current_device_id;
      });
      if (match == current.end() || match->mode != target.baseline.mode || match->active != target.baseline.active) {
        verification.refresh_restore_healthy = false;
        verification.issues.emplace_back("physical_display_mode_mismatch");
      }
    }
    verification.healthy = verification.issues.empty() && verification.refresh_restore_healthy;
    verification.rollback_required = !verification.healthy;
    return verification;
  }

  void transition_display_transaction(display_transaction_record_t &record, display_transaction_phase_e phase) {
    record.phase = phase;
    if (phase == display_transaction_phase_e::rollback_required) {
      record.healthy = false;
      record.rollback_required = true;
    }
  }

  display_transaction_record_t require_display_transaction_rollback(display_transaction_record_t record, std::string issue) {
    record.phase = display_transaction_phase_e::rollback_required;
    record.healthy = false;
    record.rollback_required = true;
    if (!issue.empty()) {
      record.issues.push_back(std::move(issue));
    }
    return record;
  }

  display_transaction_record_t complete_display_transaction_rollback(display_transaction_record_t record, bool successful) {
    if (successful) {
      record.phase = display_transaction_phase_e::rolled_back;
      record.healthy = true;
      record.rollback_required = false;
    } else {
      record.phase = display_transaction_phase_e::rollback_required;
      record.healthy = false;
      record.rollback_required = true;
      record.issues.emplace_back("rollback_failed");
    }
    return record;
  }

  status_t query_status(bool query_driver) {
    status_t status {};
    status.device_paths = enumerate_device_paths();

    if (!query_driver || status.device_paths.empty()) {
      return status;
    }

    handle_t device {open_device(status.device_paths.front(), status.open_error)};
    status.opened = device.handle != INVALID_HANDLE_VALUE;
    if (!status.opened) {
      return status;
    }

    DWORD bytes_returned = 0;
    status.ping_ok = DeviceIoControl(device.handle, ioctl_driver_ping, nullptr, 0, nullptr, 0, &bytes_returned, nullptr);
    if (!status.ping_ok) {
      status.ping_error = GetLastError();
    }

    driver_protocol_out_t protocol {};
    if (DeviceIoControl(device.handle, ioctl_get_protocol_version, nullptr, 0, &protocol, sizeof(protocol), &bytes_returned, nullptr)) {
      status.protocol_version = protocol_version_t {
        protocol.version.major,
        protocol.version.minor,
        protocol.version.incremental,
        protocol.version.test_build
      };
      status.protocol_compatible = is_protocol_compatible(*status.protocol_version);
    } else {
      status.protocol_error = GetLastError();
    }

    driver_watchdog_out_t watchdog {};
    if (DeviceIoControl(device.handle, ioctl_get_watchdog, nullptr, 0, &watchdog, sizeof(watchdog), &bytes_returned, nullptr)) {
      status.watchdog = watchdog_t {
        watchdog.timeout,
        watchdog.countdown
      };
    } else {
      status.watchdog_error = GetLastError();
    }

    return status;
  }

  bool is_tracked_display_name(const std::string &display_name) {
    std::scoped_lock lock {tracked_display_names_mutex};
    return std::find(tracked_display_names.begin(), tracked_display_names.end(), display_name) != tracked_display_names.end();
  }

  void track_display_name(const std::string &display_name) {
    std::scoped_lock lock {tracked_display_names_mutex};
    if (std::find(tracked_display_names.begin(), tracked_display_names.end(), display_name) == tracked_display_names.end()) {
      tracked_display_names.push_back(display_name);
    }
  }

  void untrack_display_name(const std::string &display_name) {
    std::scoped_lock lock {tracked_display_names_mutex};
    std::erase(tracked_display_names, display_name);
  }

  virtual_display_allocation_t::virtual_display_allocation_t(
    std::wstring device_path,
    GUID monitor_guid,
    std::string display_name,
    std::string device_id,
    display_request_t request,
    display_device::ActiveTopology baseline_topology,
    HANDLE device_handle,
    bool restore_environment
  ):
      device_path {std::move(device_path)},
      monitor_guid {monitor_guid},
      display_name_ {std::move(display_name)},
      device_id_ {std::move(device_id)},
      request_ {std::move(request)},
      baseline_topology_ {std::move(baseline_topology)},
      device_handle {device_handle},
      active {true},
      baseline_restore_pending {false},
      restore_environment {restore_environment} {
  }

  virtual_display_allocation_t::~virtual_display_allocation_t() {
    DWORD remove_error = ERROR_SUCCESS;
    bool removed = false;
    unsigned int completed_attempts = 0;
    do {
      removed = remove(remove_error);
      ++completed_attempts;
      if (removed || !display_reliability::should_retry_sudovda_release(removed, completed_attempts)) {
        break;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds {250});
    } while (true);

    if (!removed) {
      BOOST_LOG(error) << "SudoVDA allocation teardown remained unresolved after bounded retry [error="
                       << remove_error << ']';
    }

    if (device_handle != nullptr && device_handle != INVALID_HANDLE_VALUE) {
      CloseHandle(device_handle);
      device_handle = INVALID_HANDLE_VALUE;
    }
  }

  const std::string &virtual_display_allocation_t::display_name() const {
    return display_name_;
  }

  bool virtual_display_allocation_t::reapply_stream_topology() {
    if (!active || device_id_.empty()) {
      return false;
    }

    auto topology_result = apply_stream_topology(device_id_, display_name_, request_, "capture_retry");
    if (topology_result.applied && topology_result.device_ids.size() == 1) {
      device_id_ = std::move(topology_result.device_ids.front());
    }
    return topology_result.applied;
  }

  void virtual_display_allocation_t::start_watchdog_keepalive(const UINT timeout_seconds) {
    const auto interval = watchdog_keepalive_interval(timeout_seconds);
    if (interval <= std::chrono::milliseconds {0} ||
        device_handle == nullptr ||
        device_handle == INVALID_HANDLE_VALUE ||
        watchdog_worker.joinable()) {
      return;
    }

    BOOST_LOG(info) << "SunshineSeat SudoVDA watchdog keepalive started [interval_ms=" << interval.count() << ']';
    watchdog_worker = std::jthread([this, interval](const std::stop_token stop_token) {
      std::mutex wait_mutex;
      std::condition_variable_any wait_condition;
      std::unique_lock wait_lock {wait_mutex};
      unsigned int consecutive_failures = 0;

      while (!stop_token.stop_requested()) {
        wait_lock.unlock();
        DWORD bytes_returned = 0;
        const bool ping_ok = DeviceIoControl(
          device_handle,
          ioctl_driver_ping,
          nullptr,
          0,
          nullptr,
          0,
          &bytes_returned,
          nullptr
        );
        if (ping_ok) {
          if (consecutive_failures > 0) {
            BOOST_LOG(info) << "SunshineSeat SudoVDA watchdog keepalive recovered after failed pings";
          }
          consecutive_failures = 0;
        } else {
          ++consecutive_failures;
          if (consecutive_failures == 1 || consecutive_failures % 10 == 0) {
            BOOST_LOG(warning) << "SunshineSeat SudoVDA watchdog keepalive ping failed [error=" << GetLastError()
                               << ", consecutive_failures=" << consecutive_failures << ']';
          }
        }
        wait_lock.lock();
        wait_condition.wait_for(wait_lock, stop_token, interval, []() {
          return false;
        });
      }
    });
  }

  void virtual_display_allocation_t::stop_watchdog_keepalive() {
    if (!watchdog_worker.joinable()) {
      return;
    }

    watchdog_worker.request_stop();
    watchdog_worker = std::jthread {};
  }

  bool is_successful_remove_result(bool removed, DWORD remove_error) {
    return removed || remove_error == ERROR_NOT_FOUND;
  }

  bool capture_requires_dxgi_duplication_probe(std::string_view capture) {
    // WGC creates a GraphicsCaptureItem directly from the monitor. Requiring
    // DuplicateOutput() here makes a transient DDX failure look like a lost
    // SudoVDA display and can trigger an avoidable allocation/reinit loop.
    return capture != std::string_view {"wgc"};
  }

  bool requires_provider_capturability_blocker(bool provider_capturable, bool require_provider_capturability) {
    return require_provider_capturability && !provider_capturable;
  }

  bool requires_create_rollback(bool created, bool provider_capturable) {
    return created && !provider_capturable;
  }

  std::optional<std::string> find_new_display_device_id(
    const std::vector<enumerated_display_t> &before,
    const std::vector<enumerated_display_t> &after,
    const std::string &requested_device_name
  ) {
    const auto candidate_score = [&](const enumerated_display_t &candidate) {
      const bool requested_name_match =
        contains_case_insensitive(candidate.friendly_name, requested_device_name) ||
        contains_case_insensitive(candidate.display_name, requested_device_name) ||
        contains_case_insensitive(candidate.device_id, requested_device_name);
      const bool inactive = !candidate.active;
      return std::pair {requested_name_match, inactive};
    };

    std::vector<const enumerated_display_t *> candidates;
    candidates.reserve(after.size());

    for (const auto &candidate : after) {
      const auto existing = std::find_if(before.begin(), before.end(), [&](const enumerated_display_t &known) {
        return known.device_id == candidate.device_id;
      });
      if (existing == before.end()) {
        candidates.push_back(&candidate);
      }
    }

    if (candidates.empty()) {
      for (const auto &candidate : after) {
        const auto [requested_name_match, inactive] = candidate_score(candidate);
        if (requested_name_match) {
          (void) inactive;
          return candidate.device_id;
        }
      }
      return std::nullopt;
    }

    const auto best_it = std::max_element(candidates.begin(), candidates.end(), [&](const enumerated_display_t *left, const enumerated_display_t *right) {
      return candidate_score(*left) < candidate_score(*right);
    });
    if (best_it == candidates.end()) {
      return std::nullopt;
    }

    const auto [requested_name_match, inactive] = candidate_score(**best_it);
    if (requested_name_match || candidates.size() == 1) {
      (void) inactive;
      return (*best_it)->device_id;
    }

    return std::nullopt;
  }

  std::optional<std::string> resolve_device_id_after_display_name_resolution(
    const std::optional<std::string> &resolved_device_id,
    const std::vector<enumerated_display_t> &before,
    const std::vector<enumerated_display_t> &after,
    const std::string &requested_device_name
  ) {
    if (resolved_device_id && !resolved_device_id->empty()) {
      return resolved_device_id;
    }

    return find_new_display_device_id(before, after, requested_device_name);
  }

  bool virtual_display_allocation_t::remove(DWORD &remove_error) {
    std::lock_guard transition_lock {sudovda_allocation_transition_mutex};
    stop_watchdog_keepalive();
    if (!active) {
      if (restore_environment && baseline_restore_pending) {
        baseline_restore_pending = !restore_physical_topology(baseline_topology_, "deferred_release_retry");
        if (baseline_restore_pending) {
          remove_error = ERROR_GEN_FAILURE;
          return false;
        }
      }
      remove_error = ERROR_SUCCESS;
      return true;
    }

    handle_t fallback_device {INVALID_HANDLE_VALUE};
    HANDLE control_handle = device_handle;
    if (control_handle == nullptr || control_handle == INVALID_HANDLE_VALUE) {
      DWORD open_error = ERROR_SUCCESS;
      fallback_device.handle = open_device(device_path, open_error);
      control_handle = fallback_device.handle;
      if (control_handle == INVALID_HANDLE_VALUE) {
        remove_error = open_error;
        return false;
      }
    }

    const bool removed = remove_virtual_display(control_handle, monitor_guid, remove_error);
    if (!is_successful_remove_result(removed, remove_error)) {
      return false;
    }
    if (!removed && remove_error == ERROR_NOT_FOUND) {
      remove_error = ERROR_SUCCESS;
    }

    untrack_display_name(display_name_);
    active = false;
    if (device_handle != nullptr && device_handle != INVALID_HANDLE_VALUE) {
      CloseHandle(device_handle);
      device_handle = INVALID_HANDLE_VALUE;
    }

    if (restore_environment) {
      baseline_restore_pending = !restore_physical_topology(baseline_topology_, "allocation_release");
      if (baseline_restore_pending) {
        remove_error = ERROR_GEN_FAILURE;
        return false;
      }

    }

    return true;
  }

  lifecycle_plan_t plan_create_virtual_display(const display_request_t &request, lifecycle_mode_e mode) {
    lifecycle_plan_t plan {
      lifecycle_operation_e::create,
      mode,
      query_status(true),
      {},
      {}
    };

    std::ostringstream action;
    action << "Would call IOCTL_ADD_VIRTUAL_DISPLAY"
           << " width=" << request.width
           << " height=" << request.height
           << " refresh_rate=" << request.refresh_rate
           << " monitor_guid=" << guid_to_string(request.monitor_guid)
           << " device_name=" << request.device_name
           << " serial_number=" << request.serial_number
           << " ioctl=0x" << std::hex << ioctl_add_virtual_display;
    plan.actions.emplace_back(action.str());
    plan.actions.emplace_back("Would poll active display paths to resolve the added display name.");

    add_common_blockers(plan);
    return plan;
  }

  lifecycle_plan_t plan_remove_virtual_display(const GUID &monitor_guid, lifecycle_mode_e mode) {
    lifecycle_plan_t plan {
      lifecycle_operation_e::remove,
      mode,
      query_status(true),
      {},
      {}
    };

    std::ostringstream action;
    action << "Would call IOCTL_REMOVE_VIRTUAL_DISPLAY"
           << " monitor_guid=" << guid_to_string(monitor_guid)
           << " ioctl=0x" << std::hex << ioctl_remove_virtual_display;
    plan.actions.emplace_back(action.str());

    add_common_blockers(plan);
    return plan;
  }

  allocation_result_t create_virtual_display_allocation(
    const display_request_t &request,
    bool allow_service_session,
    bool apply_exclusive_topology,
    bool restore_environment,
    bool require_provider_capturability
  ) {
    std::lock_guard transition_lock {sudovda_allocation_transition_mutex};
    lifecycle_plan_t plan {
      lifecycle_operation_e::create,
      lifecycle_mode_e::execute,
      query_status(true),
      {},
      {}
    };

    std::ostringstream action;
    action << "Will call IOCTL_ADD_VIRTUAL_DISPLAY"
           << " width=" << request.width
           << " height=" << request.height
           << " refresh_rate=" << request.refresh_rate
           << " monitor_guid=" << guid_to_string(request.monitor_guid)
           << " device_name=" << request.device_name
           << " serial_number=" << request.serial_number
           << " ioctl=0x" << std::hex << ioctl_add_virtual_display;
    plan.actions.emplace_back(action.str());
    plan.actions.emplace_back("Will poll active display paths to resolve the added display name.");
    plan.actions.emplace_back("Will keep the virtual display allocated until the allocation object is destroyed.");

    add_execution_blockers(plan, allow_service_session);

    allocation_result_t allocation_result {
      execution_result_t {
      std::move(plan),
      false,
      ERROR_SUCCESS,
      std::nullopt,
      std::nullopt,
      {},
      false,
      false,
      ERROR_SUCCESS
      },
      nullptr
    };

    auto &result = allocation_result.result;

    if (!result.plan.blockers.empty()) {
      return allocation_result;
    }

    DWORD open_error = ERROR_SUCCESS;
    handle_t device {open_device(result.plan.status.device_paths.front(), open_error)};
    if (device.handle == INVALID_HANDLE_VALUE) {
      result.plan.blockers.emplace_back("SudoVDA device could not be opened for execute.");
      result.create_error = open_error;
      return allocation_result;
    }

    const auto display_device_api = std::make_shared<display_device::WinDisplayDevice>(std::make_shared<display_device::WinApiLayer>());
    const auto devices_before_add = enumerate_display_devices();
    const auto baseline_topology = capture_physical_topology(display_device_api, devices_before_add, true);
    if (!baseline_topology) {
      result.plan.blockers.emplace_back("Active physical display topology could not be captured before SudoVDA allocation.");
      return allocation_result;
    }
    auto transaction = snapshot_display_transaction(devices_before_add, *baseline_topology);
    transition_display_transaction(transaction, display_transaction_phase_e::creating);

    virtual_display_add_params_t params {
      request.width,
      request.height,
      request.refresh_rate,
      request.monitor_guid,
      {},
      {}
    };
    copy_fixed_string(params.device_name, request.device_name);
    copy_fixed_string(params.serial_number, request.serial_number);

    virtual_display_add_out_t added_display {};
    DWORD bytes_returned = 0;
    result.created = DeviceIoControl(
      device.handle,
      ioctl_add_virtual_display,
      &params,
      sizeof(params),
      &added_display,
      sizeof(added_display),
      &bytes_returned,
      nullptr
    );

    if (!result.created) {
      result.create_error = GetLastError();
      result.plan.blockers.emplace_back("IOCTL_ADD_VIRTUAL_DISPLAY failed.");
      return allocation_result;
    }

    std::string display_name;
    if (wait_for_display_name(added_display, request, devices_before_add, result.device_id, display_name)) {
      result.display_name = display_name;
      track_display_name(display_name);
      if (require_provider_capturability) {
        result.provider_capturable = wait_for_provider_capturable(display_name, result.provider_display_names);
      } else {
        result.provider_display_names = capturable_display_names(false);
        result.provider_capturable = std::find(
                                       result.provider_display_names.begin(),
                                       result.provider_display_names.end(),
                                       display_name
                                     ) != result.provider_display_names.end();
        if (!result.provider_capturable) {
          BOOST_LOG(info) << "SudoVDA provider capturability check deferred until committed dual-display topology for " << display_name;
        }
      }
      if (requires_provider_capturability_blocker(result.provider_capturable, require_provider_capturability)) {
        result.plan.blockers.emplace_back("Created display did not appear in SudoVDA provider capturable display list before timeout.");
      }
    } else {
      result.plan.blockers.emplace_back("Created display did not resolve to a Windows display name before timeout.");
    }

    if (result.plan.blockers.empty() && result.display_name && result.device_id) {
      transition_display_transaction(transaction, display_transaction_phase_e::verifying);
      stream_topology_result_t topology_result;
      if (apply_exclusive_topology) {
        const std::vector<stream_display_t> displays {
          stream_display_t {*result.device_id, *result.display_name, request}
        };
        topology_result = apply_stream_topology_for_displays(displays, "allocation_create");
      }
      if (apply_exclusive_topology && !topology_result.applied) {
        result.plan.blockers.emplace_back("Exclusive virtual topology could not be applied and verified after SudoVDA allocation.");
      } else {
        if (apply_exclusive_topology && topology_result.device_ids.size() == 1) {
          result.device_id = std::move(topology_result.device_ids.front());
        }
        allocation_result.allocation = std::make_shared<virtual_display_allocation_t>(
          result.plan.status.device_paths.front(),
          request.monitor_guid,
          *result.display_name,
          *result.device_id,
          request,
          transaction.baseline_topology,
          device.release(),
          restore_environment
        );
        allocation_result.allocation->start_watchdog_keepalive(
          result.plan.status.watchdog ? result.plan.status.watchdog->timeout : 0
        );
      }
    } else if (result.plan.blockers.empty() && result.display_name && !result.device_id) {
      result.plan.blockers.emplace_back("Created SudoVDA display did not resolve to a device id for topology application.");
    }

    if (!allocation_result.allocation && requires_create_rollback(result.created, result.provider_capturable)) {
      transaction = require_display_transaction_rollback(transaction, "create_verification_failed");
      const bool removed = remove_virtual_display(device.handle, request.monitor_guid, result.remove_error);
      result.removed = is_successful_remove_result(removed, result.remove_error);
      if (result.display_name) {
        untrack_display_name(*result.display_name);
      }
      if (!removed && result.remove_error == ERROR_NOT_FOUND) {
        result.remove_error = ERROR_SUCCESS;
      }
      if (!result.removed) {
        result.plan.blockers.emplace_back("Rollback remove failed after allocation failure.");
      }
      const bool topology_restored = restore_physical_topology(transaction.baseline_topology, "create_rollback");
      if (!topology_restored) {
        result.plan.blockers.emplace_back("Physical display topology restoration failed after allocation rollback.");
      }
      const auto restore_verification = verify_display_transaction_restored(transaction, enumerate_display_devices());
      if (!restore_verification.healthy) {
        result.plan.blockers.emplace_back("Physical display topology verification failed after allocation rollback.");
      }
      transaction = complete_display_transaction_rollback(
        transaction,
        result.removed && topology_restored && restore_verification.healthy
      );
      BOOST_LOG(info) << "SudoVDA create transaction rollback " << (transaction.healthy ? "completed" : "failed");
    }

    return allocation_result;
  }

  execution_result_t remove_virtual_display_allocation(const display_request_t &request, bool allow_service_session) {
    std::lock_guard transition_lock {sudovda_allocation_transition_mutex};
    lifecycle_plan_t plan {
      lifecycle_operation_e::remove,
      lifecycle_mode_e::execute,
      query_status(true),
      {},
      {}
    };

    std::ostringstream action;
    action << "Will call IOCTL_REMOVE_VIRTUAL_DISPLAY"
           << " monitor_guid=" << guid_to_string(request.monitor_guid)
           << " ioctl=0x" << std::hex << ioctl_remove_virtual_display;
    plan.actions.emplace_back(action.str());

    add_execution_blockers(plan, allow_service_session);

    execution_result_t result {
      std::move(plan),
      false,
      ERROR_SUCCESS,
      std::nullopt,
      std::nullopt,
      {},
      false,
      false,
      ERROR_SUCCESS
    };

    if (!result.plan.blockers.empty()) {
      return result;
    }

    DWORD open_error = ERROR_SUCCESS;
    handle_t device {open_device(result.plan.status.device_paths.front(), open_error)};
    if (device.handle == INVALID_HANDLE_VALUE) {
      result.plan.blockers.emplace_back("SudoVDA device could not be opened for remove.");
      result.remove_error = open_error;
      return result;
    }

    const auto display_device_api = std::make_shared<display_device::WinDisplayDevice>(std::make_shared<display_device::WinApiLayer>());
    const auto devices_before_remove = enumerate_display_devices();
    const auto baseline_topology = capture_physical_topology(display_device_api, devices_before_remove, true);
    const bool removed = remove_virtual_display(device.handle, request.monitor_guid, result.remove_error);
    result.removed = is_successful_remove_result(removed, result.remove_error);
    if (!removed && result.remove_error == ERROR_NOT_FOUND) {
      result.remove_error = ERROR_SUCCESS;
    }
    if (!result.removed) {
      result.plan.blockers.emplace_back("IOCTL_REMOVE_VIRTUAL_DISPLAY failed.");
    } else if (!baseline_topology || !restore_physical_topology(*baseline_topology, "orphan_preflight_remove")) {
      result.removed = false;
      result.remove_error = ERROR_GEN_FAILURE;
      result.plan.blockers.emplace_back("Physical display topology could not be restored after SudoVDA removal.");
    }

    return result;
  }

  execution_result_t test_create_virtual_display(const display_request_t &request, bool allow_service_session) {
    auto allocation_result = create_virtual_display_allocation(request, allow_service_session);
    auto &result = allocation_result.result;

    if (allocation_result.allocation) {
      result.removed = allocation_result.allocation->remove(result.remove_error);
      if (!result.removed) {
        result.plan.blockers.emplace_back("Rollback remove failed after test create.");
      }
      allocation_result.allocation.reset();
    }

    return result;
  }

  display_request_t make_default_request(UINT width, UINT height, UINT refresh_rate) {
    return display_request_t {
      width,
      height,
      refresh_rate,
      GUID {0x53734d35, 0x5a53, 0x4456, {0x44, 0x41, 0x2d, 0x44, 0x52, 0x59, 0x52, 0x55}},
      "SUNSEAT",
      "DRYRUN"
    };
  }

  display_request_t make_persistent_request(UINT width, UINT height, UINT refresh_rate, const std::string &identity) {
    auto request = make_default_request(width, height, refresh_rate);
    if (identity.empty()) {
      return request;
    }

    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char ch : identity) {
      hash ^= ch;
      hash *= 1099511628211ULL;
    }

    request.monitor_guid = GUID {
      static_cast<unsigned long>(0x53534d00U ^ static_cast<unsigned long>(hash & 0xffffffffU)),
      static_cast<unsigned short>((hash >> 32) & 0xffffU),
      static_cast<unsigned short>((hash >> 48) & 0xffffU),
      {
        0x53,
        0x53,
        0x44,
        0x41,
        static_cast<unsigned char>((hash >> 0) & 0xffU),
        static_cast<unsigned char>((hash >> 8) & 0xffU),
        static_cast<unsigned char>((hash >> 16) & 0xffU),
        static_cast<unsigned char>((hash >> 24) & 0xffU)
      }
    };

    std::ostringstream serial;
    serial << "SS" << std::hex << std::uppercase << std::setw(10) << std::setfill('0') << (hash & 0xffffffffffULL);
    request.serial_number = serial.str();
    request.device_name = "SUNSEAT";
    return request;
  }

  void print_status(std::ostream &out, const status_t &status) {
    out << "SudoVDA status\n";
    out << "  device_interfaces: " << status.device_paths.size() << '\n';
    for (const auto &path : status.device_paths) {
      out << "  device_path: " << narrow(path) << '\n';
    }

    out << "  opened: " << (status.opened ? "yes" : "no") << '\n';
    print_error(out, "  open_error", status.open_error);
    out << "  ping_ok: " << (status.ping_ok ? "yes" : "no") << '\n';
    print_error(out, "  ping_error", status.ping_error);

    if (status.protocol_version) {
      out << "  protocol_version: " << protocol_to_string(*status.protocol_version) << '\n';
    } else {
      out << "  protocol_version: unavailable\n";
    }
    print_error(out, "  protocol_error", status.protocol_error);
    out << "  protocol_compatible: " << (status.protocol_compatible ? "yes" : "no") << '\n';

    if (status.watchdog) {
      out << "  watchdog_timeout: " << status.watchdog->timeout << '\n';
      out << "  watchdog_countdown: " << status.watchdog->countdown << '\n';
    } else {
      out << "  watchdog: unavailable\n";
    }
    print_error(out, "  watchdog_error", status.watchdog_error);
  }

  void print_plan(std::ostream &out, const lifecycle_plan_t &plan) {
    out << "SudoVDA lifecycle plan\n";
    out << "  operation: " << (plan.operation == lifecycle_operation_e::create ? "create" : "remove") << '\n';
    out << "  mode: " << (plan.mode == lifecycle_mode_e::dry_run ? "dry-run" : "execute") << '\n';
    out << "  blocked: " << (plan.blockers.empty() ? "no" : "yes") << '\n';

    out << "  actions:\n";
    for (const auto &action : plan.actions) {
      out << "    - " << action << '\n';
    }

    out << "  blockers:\n";
    if (plan.blockers.empty()) {
      out << "    - none\n";
    } else {
      for (const auto &blocker : plan.blockers) {
        out << "    - " << blocker << '\n';
      }
    }

    print_status(out, plan.status);
  }

  void print_execution_result(std::ostream &out, const execution_result_t &result) {
    print_plan(out, result.plan);

    out << "SudoVDA execution result\n";
    out << "  created: " << (result.created ? "yes" : "no") << '\n';
    print_error(out, "  create_error", result.create_error);
    if (result.display_name) {
      out << "  display_name: " << *result.display_name << '\n';
    } else {
      out << "  display_name: unresolved\n";
    }
    if (result.device_id) {
      out << "  device_id: " << *result.device_id << '\n';
    } else {
      out << "  device_id: unresolved\n";
    }
    out << "  provider_capturable: " << (result.provider_capturable ? "yes" : "no") << '\n';
    out << "  provider_display_names: ";
    if (result.provider_display_names.empty()) {
      out << "none\n";
    } else {
      for (std::size_t index = 0; index < result.provider_display_names.size(); ++index) {
        if (index > 0) {
          out << ", ";
        }
        out << result.provider_display_names[index];
      }
      out << '\n';
    }
    out << "  removed: " << (result.removed ? "yes" : "no") << '\n';
    print_error(out, "  remove_error", result.remove_error);
  }

}  // namespace platf::sudovda
