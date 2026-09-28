/**
 * @file src/client_mic.cpp
 * @brief Authenticated client-microphone frame validation and host sink.
 */

#include "client_mic.h"

#include "audio.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <opus/opus.h>
#include <utility>

#ifdef _WIN32
  #include "logging.h"
  #include "platform/windows/utf_utils.h"

  #include <audioclient.h>
  #include <mmdeviceapi.h>
  #include <propidl.h>
  #include <wrl/client.h>

  // Must be the last included file because it supplies MinGW GUID definitions.
  // clang-format off
  #include "platform/windows/PolicyConfig.h"
  // clang-format on
#endif

namespace client_mic {
  bool allows_routing(const std::string_view policy) noexcept {
    return audio::allows_audio_routing(policy);
  }

  namespace {
    bool equals_case_insensitive(const std::string_view left, const std::string_view right) {
      if (left.size() != right.size()) {
        return false;
      }
      return std::equal(left.begin(), left.end(), right.begin(), [](const char lhs, const char rhs) {
        return std::tolower(static_cast<unsigned char>(lhs)) == std::tolower(static_cast<unsigned char>(rhs));
      });
    }

    bool contains_case_insensitive(const std::string_view value, const std::string_view needle) {
      if (needle.empty()) {
        return true;
      }
      return std::search(value.begin(), value.end(), needle.begin(), needle.end(), [](const char lhs, const char rhs) {
               return std::tolower(static_cast<unsigned char>(lhs)) == std::tolower(static_cast<unsigned char>(rhs));
             }) != value.end();
    }

    bool looks_like_virtual_audio_endpoint(
      const std::string_view friendly_name,
      const std::string_view device_description,
      const std::string_view adapter_friendly_name = {}
    ) {
      constexpr std::array<std::string_view, 5> virtual_markers {
        "cable",
        "virtual",
        "loopback",
        "moonlight",
        "vb-audio",
      };
      return std::any_of(virtual_markers.begin(), virtual_markers.end(), [&](const auto marker) {
        return contains_case_insensitive(friendly_name, marker) ||
               contains_case_insensitive(device_description, marker) ||
               contains_case_insensitive(adapter_friendly_name, marker);
      });
    }

    std::uint16_t read_u16(const char *data) {
      return static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[0])) |
             static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[1])) << 8U;
    }

    std::uint32_t read_u32(const char *data) {
      return static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[0])) |
             static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[1])) << 8U |
             static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[2])) << 16U |
             static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[3])) << 24U;
    }
  }  // namespace

  bool is_matching_capture_endpoint(
    const std::string_view render_friendly_name,
    const std::string_view render_device_description,
    const std::string_view capture_friendly_name,
    const std::string_view capture_device_description
  ) {
    if (render_device_description.empty() || capture_device_description.empty() || !equals_case_insensitive(render_device_description, capture_device_description)) {
      return false;
    }

    return looks_like_virtual_audio_endpoint(render_friendly_name, render_device_description) &&
           looks_like_virtual_audio_endpoint(capture_friendly_name, capture_device_description);
  }

  bool is_matching_capture_endpoint_with_adapter(
    const std::string_view render_friendly_name,
    const std::string_view render_device_description,
    const std::string_view render_adapter_friendly_name,
    const std::string_view capture_friendly_name,
    const std::string_view capture_device_description,
    const std::string_view capture_adapter_friendly_name
  ) {
    if (render_adapter_friendly_name.empty() || capture_adapter_friendly_name.empty() || !equals_case_insensitive(render_adapter_friendly_name, capture_adapter_friendly_name)) {
      return false;
    }

    return looks_like_virtual_audio_endpoint(render_friendly_name, render_device_description, render_adapter_friendly_name) &&
           looks_like_virtual_audio_endpoint(capture_friendly_name, capture_device_description, capture_adapter_friendly_name);
  }

  bool is_amd_voice_capture_endpoint(
    const std::string_view friendly_name,
    const std::string_view device_description,
    const std::string_view adapter_friendly_name
  ) {
    constexpr std::string_view amd_voice_marker = "amd streaming audio device";
    return contains_case_insensitive(friendly_name, amd_voice_marker) ||
           contains_case_insensitive(device_description, amd_voice_marker) ||
           contains_case_insensitive(adapter_friendly_name, amd_voice_marker);
  }

  bool is_krisp_capture_endpoint(
    const std::string_view friendly_name,
    const std::string_view device_description,
    const std::string_view adapter_friendly_name
  ) {
    constexpr std::string_view krisp_marker = "krisp microphone";
    return contains_case_insensitive(friendly_name, krisp_marker) ||
           contains_case_insensitive(device_description, krisp_marker) ||
           contains_case_insensitive(adapter_friendly_name, krisp_marker);
  }

  bool decode_frame(const std::string_view &payload, frame_t &frame) {
    if (payload.size() < MESSAGE_HEADER_SIZE || payload.size() > MAX_PACKET_BYTES) {
      return false;
    }
    const auto *data = payload.data();
    if (static_cast<std::uint8_t>(data[0]) != MESSAGE_VERSION || static_cast<std::uint8_t>(data[1]) != CODEC_OPUS || static_cast<std::uint8_t>(data[2]) != CHANNELS_MONO || read_u32(data + 12) != SAMPLE_RATE || read_u16(data + 8) != FRAME_SAMPLES) {
      return false;
    }

    const auto encoded_length = read_u16(data + 10);
    if (encoded_length == 0 || encoded_length > MAX_PACKET_BYTES - MESSAGE_HEADER_SIZE || MESSAGE_HEADER_SIZE + encoded_length != payload.size()) {
      return false;
    }

    frame.sequence = read_u32(data + 4);
    frame.sample_count = read_u16(data + 8);
    frame.encoded_length = encoded_length;
    frame.channels = static_cast<std::uint8_t>(data[2]);
    frame.codec = static_cast<std::uint8_t>(data[1]);
    frame.sample_rate = read_u32(data + 12);
    frame.encoded = payload.substr(MESSAGE_HEADER_SIZE, encoded_length);
    return true;
  }

#ifdef _WIN32
  namespace {
    constexpr PROPERTYKEY pkey_device_friendly_name {
      {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
      14,
    };
    constexpr PROPERTYKEY pkey_device_description {
      {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
      2,
    };
    constexpr PROPERTYKEY pkey_device_interface_friendly_name {
      {0x026e516e, 0xb814, 0x414b, {0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22}},
      2,
    };

    constexpr auto audio_role_count = static_cast<std::size_t>(ERole_enum_count);

    class com_scope_t {
    public:
      com_scope_t():
          result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {
        uninitialize_ = result_ == S_OK || result_ == S_FALSE;
      }

      ~com_scope_t() {
        if (uninitialize_) {
          CoUninitialize();
        }
      }

      bool usable() const {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
      }

    private:
      HRESULT result_;
      bool uninitialize_ = false;
    };

    struct prop_variant_t {
      prop_variant_t() {
        PropVariantInit(&value);
      }

      ~prop_variant_t() {
        PropVariantClear(&value);
      }

      PROPVARIANT value;
    };

    struct endpoint_info_t {
      std::wstring id;
      std::string friendly_name;
      std::string device_description;
      std::string adapter_friendly_name;
    };

    struct default_capture_route_state_t {
      std::size_t active_streams = 0;
      std::wstring target_id;
      std::array<std::optional<std::wstring>, audio_role_count> previous_ids;
    };

    std::mutex default_capture_route_mutex;
    default_capture_route_state_t default_capture_route;

    std::wstring get_device_id(IMMDevice *device) {
      if (device == nullptr) {
        return {};
      }

      LPWSTR raw_id = nullptr;
      if (FAILED(device->GetId(&raw_id)) || raw_id == nullptr) {
        return {};
      }

      std::wstring id(raw_id);
      CoTaskMemFree(raw_id);
      return id;
    }

    std::string get_property_string(IPropertyStore *store, const PROPERTYKEY &key) {
      if (store == nullptr) {
        return {};
      }

      prop_variant_t value;
      if (FAILED(store->GetValue(key, &value.value)) || value.value.vt != VT_LPWSTR || value.value.pwszVal == nullptr) {
        return {};
      }
      return utf_utils::to_utf8(std::wstring(value.value.pwszVal));
    }

    std::optional<endpoint_info_t> describe_endpoint(IMMDevice *device) {
      if (device == nullptr) {
        return std::nullopt;
      }

      endpoint_info_t result;
      result.id = get_device_id(device);
      if (result.id.empty()) {
        return std::nullopt;
      }

      Microsoft::WRL::ComPtr<IPropertyStore> property_store;
      if (FAILED(device->OpenPropertyStore(STGM_READ, property_store.GetAddressOf()))) {
        return std::nullopt;
      }

      result.friendly_name = get_property_string(property_store.Get(), pkey_device_friendly_name);
      result.device_description = get_property_string(property_store.Get(), pkey_device_description);
      result.adapter_friendly_name = get_property_string(property_store.Get(), pkey_device_interface_friendly_name);
      return result;
    }

    std::optional<endpoint_info_t> find_capture_companion(
      IMMDeviceEnumerator *enumerator,
      const std::string &sink_id
    ) {
      if (enumerator == nullptr || sink_id.empty()) {
        return std::nullopt;
      }

      const auto sink_id_utf16 = utf_utils::from_utf8(sink_id);
      if (sink_id_utf16.empty()) {
        return std::nullopt;
      }

      Microsoft::WRL::ComPtr<IMMDevice> render_endpoint;
      if (FAILED(enumerator->GetDevice(sink_id_utf16.c_str(), render_endpoint.GetAddressOf())) || render_endpoint == nullptr) {
        return std::nullopt;
      }

      DWORD render_state = DEVICE_STATE_DISABLED;
      if (FAILED(render_endpoint->GetState(&render_state)) || render_state != DEVICE_STATE_ACTIVE) {
        return std::nullopt;
      }

      const auto render_info = describe_endpoint(render_endpoint.Get());
      if (!render_info) {
        return std::nullopt;
      }

      Microsoft::WRL::ComPtr<IMMDeviceCollection> capture_endpoints;
      if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, capture_endpoints.GetAddressOf())) || capture_endpoints == nullptr) {
        return std::nullopt;
      }

      UINT count = 0;
      if (FAILED(capture_endpoints->GetCount(&count))) {
        return std::nullopt;
      }

      for (UINT index = 0; index < count; ++index) {
        Microsoft::WRL::ComPtr<IMMDevice> capture_endpoint;
        if (FAILED(capture_endpoints->Item(index, capture_endpoint.GetAddressOf())) || capture_endpoint == nullptr) {
          continue;
        }

        const auto capture_info = describe_endpoint(capture_endpoint.Get());
        if (capture_info && is_matching_capture_endpoint_with_adapter(
              render_info->friendly_name,
              render_info->device_description,
              render_info->adapter_friendly_name,
              capture_info->friendly_name,
              capture_info->device_description,
              capture_info->adapter_friendly_name
            )) {
          return capture_info;
        }
      }

      return std::nullopt;
    }

    std::optional<endpoint_info_t> find_unique_physical_microphone(IMMDeviceEnumerator *enumerator) {
      if (enumerator == nullptr) {
        return std::nullopt;
      }

      Microsoft::WRL::ComPtr<IMMDeviceCollection> capture_endpoints;
      if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, capture_endpoints.GetAddressOf())) || capture_endpoints == nullptr) {
        return std::nullopt;
      }

      UINT count = 0;
      if (FAILED(capture_endpoints->GetCount(&count))) {
        return std::nullopt;
      }

      std::optional<endpoint_info_t> result;
      for (UINT index = 0; index < count; ++index) {
        Microsoft::WRL::ComPtr<IMMDevice> capture_endpoint;
        if (FAILED(capture_endpoints->Item(index, capture_endpoint.GetAddressOf())) || capture_endpoint == nullptr) {
          continue;
        }

        const auto capture_info = describe_endpoint(capture_endpoint.Get());
        if (!capture_info || looks_like_virtual_audio_endpoint(
              capture_info->friendly_name,
              capture_info->device_description,
              capture_info->adapter_friendly_name
            )) {
          continue;
        }

        const bool is_microphone = contains_case_insensitive(capture_info->friendly_name, "microphone") ||
                                   contains_case_insensitive(capture_info->device_description, "microphone") ||
                                   contains_case_insensitive(capture_info->adapter_friendly_name, "microphone");
        if (!is_microphone) {
          continue;
        }

        if (result) {
          // Do not guess between multiple physical microphones.
          return std::nullopt;
        }
        result = *capture_info;
      }

      return result;
    }

    std::optional<endpoint_info_t> find_amd_voice_microphone(IMMDeviceEnumerator *enumerator) {
      if (enumerator == nullptr) {
        return std::nullopt;
      }

      Microsoft::WRL::ComPtr<IMMDeviceCollection> capture_endpoints;
      if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, capture_endpoints.GetAddressOf())) || capture_endpoints == nullptr) {
        return std::nullopt;
      }

      UINT count = 0;
      if (FAILED(capture_endpoints->GetCount(&count))) {
        return std::nullopt;
      }

      std::optional<endpoint_info_t> result;
      for (UINT index = 0; index < count; ++index) {
        Microsoft::WRL::ComPtr<IMMDevice> capture_endpoint;
        if (FAILED(capture_endpoints->Item(index, capture_endpoint.GetAddressOf())) || capture_endpoint == nullptr) {
          continue;
        }

        const auto capture_info = describe_endpoint(capture_endpoint.Get());
        if (!capture_info || !is_amd_voice_capture_endpoint(capture_info->friendly_name, capture_info->device_description, capture_info->adapter_friendly_name)) {
          continue;
        }

        if (result) {
          // Do not guess if more than one AMD Voice endpoint is active.
          return std::nullopt;
        }
        result = *capture_info;
      }

      return result;
    }

    std::optional<endpoint_info_t> find_krisp_microphone(IMMDeviceEnumerator *enumerator) {
      if (enumerator == nullptr) {
        return std::nullopt;
      }

      Microsoft::WRL::ComPtr<IMMDeviceCollection> capture_endpoints;
      if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, capture_endpoints.GetAddressOf())) || capture_endpoints == nullptr) {
        return std::nullopt;
      }

      UINT count = 0;
      if (FAILED(capture_endpoints->GetCount(&count))) {
        return std::nullopt;
      }

      std::optional<endpoint_info_t> result;
      for (UINT index = 0; index < count; ++index) {
        Microsoft::WRL::ComPtr<IMMDevice> capture_endpoint;
        if (FAILED(capture_endpoints->Item(index, capture_endpoint.GetAddressOf())) || capture_endpoint == nullptr) {
          continue;
        }

        const auto capture_info = describe_endpoint(capture_endpoint.Get());
        if (!capture_info || !is_krisp_capture_endpoint(capture_info->friendly_name, capture_info->device_description, capture_info->adapter_friendly_name)) {
          continue;
        }

        if (result) {
          // Do not guess if more than one Krisp endpoint is active.
          return std::nullopt;
        }
        result = *capture_info;
      }

      return result;
    }

    std::optional<std::wstring> get_default_capture_id(IMMDeviceEnumerator *enumerator, const ERole role) {
      if (enumerator == nullptr) {
        return std::nullopt;
      }

      Microsoft::WRL::ComPtr<IMMDevice> endpoint;
      if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, role, endpoint.GetAddressOf())) || endpoint == nullptr) {
        return std::nullopt;
      }

      const auto id = get_device_id(endpoint.Get());
      if (id.empty()) {
        return std::nullopt;
      }
      return id;
    }

    bool create_audio_policy(Microsoft::WRL::ComPtr<IPolicyConfig> &policy) {
      const auto status = CoCreateInstance(
        CLSID_CPolicyConfigClient,
        nullptr,
        CLSCTX_ALL,
        IID_IPolicyConfig,
        reinterpret_cast<void **>(policy.GetAddressOf())
      );
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Could not create Windows audio policy configuration: HRESULT=" << static_cast<long>(status);
        return false;
      }
      return true;
    }

    bool repair_stale_default_capture_locked(
      IMMDeviceEnumerator *enumerator,
      IPolicyConfig *policy,
      const std::wstring &target_id
    ) {
      if (enumerator == nullptr || policy == nullptr || target_id.empty()) {
        return false;
      }

      bool stale_default_found = false;
      for (std::size_t role_index = 0; role_index < audio_role_count; ++role_index) {
        const auto current_id = get_default_capture_id(enumerator, static_cast<ERole>(role_index));
        if (current_id && *current_id == target_id) {
          stale_default_found = true;
          break;
        }
      }
      if (!stale_default_found) {
        return false;
      }

      const auto fallback = find_unique_physical_microphone(enumerator);
      if (!fallback) {
        BOOST_LOG(warning) << "Client microphone stale default input repair skipped: no unique physical microphone was found";
        return false;
      }

      bool repaired = false;
      for (std::size_t role_index = 0; role_index < audio_role_count; ++role_index) {
        const auto current_id = get_default_capture_id(enumerator, static_cast<ERole>(role_index));
        if (!current_id || *current_id != target_id) {
          continue;
        }

        const auto status = policy->SetDefaultEndpoint(fallback->id.c_str(), static_cast<ERole>(role_index));
        if (FAILED(status)) {
          BOOST_LOG(warning) << "Could not repair the stale client microphone default input for role "
                             << role_index << ": HRESULT=" << static_cast<long>(status);
          continue;
        }
        repaired = true;
      }

      if (repaired) {
        BOOST_LOG(info) << "Client microphone stale default input repaired while idle: "
                        << fallback->friendly_name;
      }
      return repaired;
    }

    bool acquire_default_capture(const std::string &sink_id) {
      if (sink_id.empty()) {
        BOOST_LOG(debug) << "Client microphone default input routing skipped: no render sink is configured";
        return false;
      }

      com_scope_t com;
      if (!com.usable()) {
        BOOST_LOG(warning) << "Client microphone default input routing skipped: COM initialization failed";
        return false;
      }

      Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
      const auto enum_status = CoCreateInstance(
        CLSID_MMDeviceEnumerator,
        nullptr,
        CLSCTX_ALL,
        IID_IMMDeviceEnumerator,
        reinterpret_cast<void **>(enumerator.GetAddressOf())
      );
      if (FAILED(enum_status) || enumerator == nullptr) {
        BOOST_LOG(warning) << "Client microphone default input routing skipped: could not create endpoint enumerator";
        return false;
      }

      const auto target = find_capture_companion(enumerator.Get(), sink_id);
      if (!target) {
        BOOST_LOG(warning) << "Client microphone default input routing skipped: no virtual capture companion matched the configured render sink";
        return false;
      }

      std::lock_guard route_lock(default_capture_route_mutex);
      if (default_capture_route.active_streams > 0) {
        if (default_capture_route.target_id == target->id) {
          ++default_capture_route.active_streams;
          return true;
        }

        BOOST_LOG(warning) << "Client microphone default input routing skipped: another active stream owns a different capture endpoint";
        return false;
      }

      Microsoft::WRL::ComPtr<IPolicyConfig> policy;
      if (!create_audio_policy(policy)) {
        return false;
      }

      repair_stale_default_capture_locked(enumerator.Get(), policy.Get(), target->id);

      default_capture_route_state_t pending_route;
      pending_route.target_id = target->id;
      for (std::size_t role_index = 0; role_index < audio_role_count; ++role_index) {
        pending_route.previous_ids[role_index] = get_default_capture_id(
          enumerator.Get(),
          static_cast<ERole>(role_index)
        );
      }

      std::array<bool, audio_role_count> changed_roles {};
      for (std::size_t role_index = 0; role_index < audio_role_count; ++role_index) {
        const auto status = policy->SetDefaultEndpoint(target->id.c_str(), static_cast<ERole>(role_index));
        if (FAILED(status)) {
          BOOST_LOG(warning) << "Could not set client microphone capture endpoint as default for role "
                             << role_index << ": HRESULT=" << static_cast<long>(status);
          for (std::size_t restore_index = 0; restore_index < role_index; ++restore_index) {
            if (changed_roles[restore_index] && pending_route.previous_ids[restore_index]) {
              policy->SetDefaultEndpoint(
                pending_route.previous_ids[restore_index]->c_str(),
                static_cast<ERole>(restore_index)
              );
            }
          }
          return false;
        }
        changed_roles[role_index] = true;
      }

      pending_route.active_streams = 1;
      default_capture_route = std::move(pending_route);
      BOOST_LOG(info) << "Client microphone capture endpoint is now the temporary default input: "
                      << target->friendly_name;
      return true;
    }

    void release_default_capture() {
      std::lock_guard route_lock(default_capture_route_mutex);
      if (default_capture_route.active_streams == 0) {
        return;
      }
      --default_capture_route.active_streams;
      if (default_capture_route.active_streams > 0) {
        return;
      }

      com_scope_t com;
      if (!com.usable()) {
        BOOST_LOG(warning) << "Could not restore the previous default input: COM initialization failed";
        default_capture_route = {};
        return;
      }

      Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
      const auto enum_status = CoCreateInstance(
        CLSID_MMDeviceEnumerator,
        nullptr,
        CLSCTX_ALL,
        IID_IMMDeviceEnumerator,
        reinterpret_cast<void **>(enumerator.GetAddressOf())
      );
      Microsoft::WRL::ComPtr<IPolicyConfig> policy;
      if (FAILED(enum_status) || enumerator == nullptr || !create_audio_policy(policy)) {
        BOOST_LOG(warning) << "Could not restore the previous default input: Windows audio policy is unavailable";
        default_capture_route = {};
        return;
      }

      for (std::size_t role_index = 0; role_index < audio_role_count; ++role_index) {
        if (!default_capture_route.previous_ids[role_index]) {
          continue;
        }

        const auto current_id = get_default_capture_id(enumerator.Get(), static_cast<ERole>(role_index));
        if (!current_id || *current_id != default_capture_route.target_id) {
          BOOST_LOG(info) << "Preserving the user's changed default input for role " << role_index;
          continue;
        }

        const auto status = policy->SetDefaultEndpoint(
          default_capture_route.previous_ids[role_index]->c_str(),
          static_cast<ERole>(role_index)
        );
        if (FAILED(status)) {
          BOOST_LOG(warning) << "Could not restore the previous default input for role "
                             << role_index << ": HRESULT=" << static_cast<long>(status);
        }
      }

      // A previous process may have already lost the physical endpoint and
      // recorded the virtual endpoint as the "previous" value. Repair that
      // stale state before declaring the route complete.
      repair_stale_default_capture_locked(
        enumerator.Get(),
        policy.Get(),
        default_capture_route.target_id
      );

      BOOST_LOG(info) << "Client microphone temporary default input routing ended";
      default_capture_route = {};
    }
  }  // namespace
#endif

  void repair_stale_default_capture(const std::string_view sink_id) {
#ifdef _WIN32
    if (sink_id.empty()) {
      return;
    }

    com_scope_t com;
    if (!com.usable()) {
      BOOST_LOG(warning) << "Client microphone stale default input repair skipped: COM initialization failed";
      return;
    }

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    const auto enum_status = CoCreateInstance(
      CLSID_MMDeviceEnumerator,
      nullptr,
      CLSCTX_ALL,
      IID_IMMDeviceEnumerator,
      reinterpret_cast<void **>(enumerator.GetAddressOf())
    );
    if (FAILED(enum_status) || enumerator == nullptr) {
      BOOST_LOG(warning) << "Client microphone stale default input repair skipped: could not create endpoint enumerator";
      return;
    }

    const auto target = find_capture_companion(enumerator.Get(), std::string(sink_id));
    if (!target) {
      return;
    }

    std::lock_guard route_lock(default_capture_route_mutex);
    if (default_capture_route.active_streams > 0) {
      return;
    }

    Microsoft::WRL::ComPtr<IPolicyConfig> policy;
    if (!create_audio_policy(policy)) {
      return;
    }
    repair_stale_default_capture_locked(enumerator.Get(), policy.Get(), target->id);
#else
    (void) sink_id;
#endif
  }

  void set_preferred_idle_capture(const std::string_view sink_id) {
#ifdef _WIN32
    if (sink_id.empty()) {
      return;
    }

    com_scope_t com;
    if (!com.usable()) {
      BOOST_LOG(warning) << "Client microphone idle default input selection skipped: COM initialization failed";
      return;
    }

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    const auto enum_status = CoCreateInstance(
      CLSID_MMDeviceEnumerator,
      nullptr,
      CLSCTX_ALL,
      IID_IMMDeviceEnumerator,
      reinterpret_cast<void **>(enumerator.GetAddressOf())
    );
    if (FAILED(enum_status) || enumerator == nullptr) {
      BOOST_LOG(warning) << "Client microphone idle default input selection skipped: could not create endpoint enumerator";
      return;
    }

    // Keep this route tied to the configured Sunshine/Moonlight companion.
    // This prevents a generic AMD endpoint from being selected when the
    // client-microphone feature is not configured for this host.
    if (!find_capture_companion(enumerator.Get(), std::string(sink_id))) {
      BOOST_LOG(warning) << "Client microphone idle default input selection skipped: no virtual capture companion matched the configured render sink";
      return;
    }

    // Prefer the stronger call processor when present. AMD Voice remains a
    // useful fallback for machines where Krisp is not installed or active.
    const auto krisp_target = find_krisp_microphone(enumerator.Get());
    const auto target = krisp_target ? krisp_target : find_amd_voice_microphone(enumerator.Get());
    if (!target) {
      BOOST_LOG(debug) << "Client microphone idle default input selection skipped: Krisp and AMD Voice endpoints are unavailable";
      return;
    }

    const bool target_is_krisp = krisp_target.has_value();

    std::lock_guard route_lock(default_capture_route_mutex);
    if (default_capture_route.active_streams > 0) {
      BOOST_LOG(debug) << "Client microphone idle default input selection skipped: a stream owns the temporary capture route";
      return;
    }

    Microsoft::WRL::ComPtr<IPolicyConfig> policy;
    if (!create_audio_policy(policy)) {
      return;
    }

    std::array<std::optional<std::wstring>, audio_role_count> previous_ids;
    std::array<bool, audio_role_count> changed_roles {};
    for (std::size_t role_index = 0; role_index < audio_role_count; ++role_index) {
      previous_ids[role_index] = get_default_capture_id(
        enumerator.Get(),
        static_cast<ERole>(role_index)
      );
    }

    for (std::size_t role_index = 0; role_index < audio_role_count; ++role_index) {
      if (previous_ids[role_index] && *previous_ids[role_index] == target->id) {
        continue;
      }

      const auto status = policy->SetDefaultEndpoint(target->id.c_str(), static_cast<ERole>(role_index));
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Could not set the processed microphone as the idle default input for role "
                           << role_index << ": HRESULT=" << static_cast<long>(status);
        for (std::size_t restore_index = 0; restore_index < role_index; ++restore_index) {
          if (changed_roles[restore_index] && previous_ids[restore_index]) {
            policy->SetDefaultEndpoint(
              previous_ids[restore_index]->c_str(),
              static_cast<ERole>(restore_index)
            );
          }
        }
        return;
      }
      changed_roles[role_index] = true;
    }

    BOOST_LOG(info) << "Client microphone idle default input set to "
                    << (target_is_krisp ? "Krisp" : "AMD Voice") << ": "
                    << target->friendly_name;
#else
    (void) sink_id;
#endif
  }

  struct receiver_t::impl_t {
    explicit impl_t(std::string id):
        sink_id(std::move(id)) {}

    std::string sink_id;
    mutable std::mutex mutex;
    metrics_t counters;
    OpusDecoder *decoder = nullptr;
    std::array<opus_int16, FRAME_SAMPLES> pcm {};
    bool have_sequence = false;
    std::uint32_t last_sequence = 0;
    bool stream_started = false;

#ifdef _WIN32
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    Microsoft::WRL::ComPtr<IMMDevice> endpoint;
    Microsoft::WRL::ComPtr<IAudioClient> audio_client;
    Microsoft::WRL::ComPtr<IAudioRenderClient> render_client;
    WAVEFORMATEX *mix_format = nullptr;
    UINT32 buffer_frames = 0;
    bool started = false;
    bool co_initialized = false;
    bool default_capture_acquired = false;
#endif

    ~impl_t() {
      stop_stream();
      reset();
    }

    void start_stream() {
      std::scoped_lock lock(mutex);
      if (stream_started) {
        return;
      }
      stream_started = true;
#ifdef _WIN32
      default_capture_acquired = acquire_default_capture(sink_id);
#endif
    }

    void stop_stream() {
      std::scoped_lock lock(mutex);
      if (!stream_started) {
        return;
      }
#ifdef _WIN32
      if (default_capture_acquired) {
        release_default_capture();
        default_capture_acquired = false;
      }
#endif
      stream_started = false;
    }

    void reset() {
#ifdef _WIN32
      if (audio_client != nullptr && started) {
        audio_client->Stop();
      }
      started = false;
      render_client.Reset();
      audio_client.Reset();
      endpoint.Reset();
      enumerator.Reset();
      if (mix_format != nullptr) {
        CoTaskMemFree(mix_format);
        mix_format = nullptr;
      }
      if (co_initialized) {
        CoUninitialize();
        co_initialized = false;
      }
#endif
      if (decoder != nullptr) {
        opus_decoder_destroy(decoder);
        decoder = nullptr;
      }
    }

#ifdef _WIN32
    bool initialize_sink() {
      const auto co_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      if (FAILED(co_result) && co_result != RPC_E_CHANGED_MODE) {
        return false;
      }
      co_initialized = co_result == S_OK || co_result == S_FALSE;

      if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.GetAddressOf())))) {
        return false;
      }
      HRESULT result = sink_id.empty() ? enumerator->GetDefaultAudioEndpoint(eRender, eConsole, endpoint.GetAddressOf()) : enumerator->GetDevice(std::wstring(sink_id.begin(), sink_id.end()).c_str(), endpoint.GetAddressOf());
      if (FAILED(result) || endpoint == nullptr || FAILED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(audio_client.GetAddressOf()))) || FAILED(audio_client->GetMixFormat(&mix_format)) || mix_format == nullptr) {
        return false;
      }

      result = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 200000, 0, mix_format, nullptr);
      if (FAILED(result) || FAILED(audio_client->GetBufferSize(&buffer_frames)) || FAILED(audio_client->GetService(IID_PPV_ARGS(render_client.GetAddressOf()))) || FAILED(audio_client->Start())) {
        return false;
      }
      started = true;
      return true;
    }

    bool write_pcm(const opus_int16 *samples, int frames) {
      if (!started || render_client == nullptr || audio_client == nullptr || mix_format == nullptr || frames <= 0) {
        return false;
      }
      UINT32 padding = 0;
      if (FAILED(audio_client->GetCurrentPadding(&padding)) || padding >= buffer_frames) {
        return false;
      }
      const auto writable = std::min<UINT32>(buffer_frames - padding, static_cast<UINT32>(frames));
      if (writable == 0) {
        return false;
      }

      BYTE *output = nullptr;
      if (FAILED(render_client->GetBuffer(writable, &output)) || output == nullptr) {
        return false;
      }

      const auto channels = std::max<WORD>(1, mix_format->nChannels);
      const auto bits = mix_format->wBitsPerSample;
      const bool float_format = mix_format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                                (mix_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix_format->cbSize >= 22 &&
                                 reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(mix_format)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
      const auto bytes_per_sample = bits / 8;
      if ((bits != 16 && bits != 32) || bytes_per_sample == 0) {
        render_client->ReleaseBuffer(0, AUDCLNT_BUFFERFLAGS_SILENT);
        return false;
      }

      for (UINT32 frame = 0; frame < writable; ++frame) {
        const float normalized = static_cast<float>(samples[frame]) / 32768.0F;
        for (WORD channel = 0; channel < channels; ++channel) {
          auto *destination = output + (static_cast<std::size_t>(frame) * channels + channel) * bytes_per_sample;
          if (float_format) {
            std::memcpy(destination, &normalized, sizeof(float));
          } else if (bits == 16) {
            std::memcpy(destination, &samples[frame], sizeof(opus_int16));
          } else {
            const auto sample32 = static_cast<std::int32_t>(samples[frame]) << 16;
            std::memcpy(destination, &sample32, sizeof(sample32));
          }
        }
      }
      return SUCCEEDED(render_client->ReleaseBuffer(writable, 0));
    }
#endif

    bool push(const std::string_view &payload) {
      frame_t frame {};
      std::scoped_lock lock(mutex);
      if (!decode_frame(payload, frame)) {
        ++counters.dropped_frames;
        return false;
      }
      if (have_sequence && static_cast<std::int32_t>(frame.sequence - last_sequence) <= 0) {
        ++counters.dropped_frames;
        return false;
      }
      have_sequence = true;
      last_sequence = frame.sequence;
      ++counters.received_frames;

      if (decoder == nullptr) {
        int opus_error = OPUS_OK;
        decoder = opus_decoder_create(SAMPLE_RATE, CHANNELS_MONO, &opus_error);
        if (decoder == nullptr || opus_error != OPUS_OK) {
          ++counters.decode_failures;
          return false;
        }
      }

      const auto decoded = opus_decode(decoder, reinterpret_cast<const unsigned char *>(frame.encoded.data()), static_cast<opus_int32>(frame.encoded.size()), pcm.data(), FRAME_SAMPLES, 0);
      if (decoded != FRAME_SAMPLES) {
        ++counters.decode_failures;
        return false;
      }
      ++counters.decoded_frames;

#ifdef _WIN32
      if (!started && !initialize_sink()) {
        ++counters.sink_failures;
        return false;
      }
      if (!write_pcm(pcm.data(), decoded)) {
        ++counters.sink_failures;
        return false;
      }
      return true;
#else
      ++counters.sink_failures;
      return false;
#endif
    }
  };

  receiver_t::receiver_t(std::string sink_id):
      impl_(std::make_unique<impl_t>(std::move(sink_id))) {}

  receiver_t::~receiver_t() = default;

  void receiver_t::start_stream() {
    impl_->start_stream();
  }

  void receiver_t::stop_stream() {
    impl_->stop_stream();
  }

  bool receiver_t::push_frame(const std::string_view &payload) {
    return impl_->push(payload);
  }

  metrics_t receiver_t::metrics() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->counters;
  }
}  // namespace client_mic
