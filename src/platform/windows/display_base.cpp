/**
 * @file src/platform/windows/display_base.cpp
 * @brief Definitions for the Windows display base code.
 */
// standard includes
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <cwctype>
#include <mutex>
#include <iomanip>
#include <sstream>
#include <thread>

// platform includes
#include <initguid.h>

// lib includes
#include <boost/algorithm/string/join.hpp>
#include <boost/process/v1.hpp>
#include <MinHook.h>

// local includes
#include "utf_utils.h"

// We have to include boost/process/v1.hpp before display.h due to WinSock.h,
// but that prevents the definition of NTSTATUS so we must define it ourself.
typedef long NTSTATUS;

// Definition from the WDK's d3dkmthk.h
typedef enum _D3DKMT_GPU_PREFERENCE_QUERY_STATE: DWORD {
  D3DKMT_GPU_PREFERENCE_STATE_UNINITIALIZED,  ///< The GPU preference isn't initialized.
  D3DKMT_GPU_PREFERENCE_STATE_HIGH_PERFORMANCE,  ///< The highest performing GPU is preferred.
  D3DKMT_GPU_PREFERENCE_STATE_MINIMUM_POWER,  ///< The minimum-powered GPU is preferred.
  D3DKMT_GPU_PREFERENCE_STATE_UNSPECIFIED,  ///< A GPU preference isn't specified.
  D3DKMT_GPU_PREFERENCE_STATE_NOT_FOUND,  ///< A GPU preference isn't found.
  D3DKMT_GPU_PREFERENCE_STATE_USER_SPECIFIED_GPU  ///< A specific GPU is preferred.
} D3DKMT_GPU_PREFERENCE_QUERY_STATE;

#include "display.h"
#include "dual_display_compositor.h"
#include "display_reliability.h"
#include "misc.h"
#include "sudovda_control.h"
#include "sudovda_dual_display.h"
#include "src/config.h"
#include "src/display_device.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/seat.h"
#include "src/video.h"

namespace platf {
  using namespace std::literals;
}

namespace platf::dxgi {
  namespace bp = boost::process::v1;

  /**
   * DDAPI-specific initialization goes here.
   */
  int duplication_t::init(display_base_t *display, const ::video::config_t &config) {
    HRESULT status;

    // Capture format will be determined from the first call to AcquireNextFrame()
    display->capture_format = DXGI_FORMAT_UNKNOWN;

    // FIXME: Duplicate output on RX580 in combination with DOOM (2016) --> BSOD
    {
      // IDXGIOutput5 is optional, but can provide improved performance and wide color support
      dxgi::output5_t output5 {};
      status = display->output->QueryInterface(IID_IDXGIOutput5, (void **) &output5);
      if (SUCCEEDED(status)) {
        // Ask the display implementation which formats it supports
        auto supported_formats = display->get_supported_capture_formats();
        if (supported_formats.empty()) {
          BOOST_LOG(warning) << "No compatible capture formats for this encoder"sv;
          return -1;
        }

        // We try this twice, in case we still get an error on reinitialization
        for (int x = 0; x < 2; ++x) {
          // Ensure we can duplicate the current display
          syncThreadDesktop();

          status = output5->DuplicateOutput1((IUnknown *) display->device.get(), 0, supported_formats.size(), supported_formats.data(), &dup);
          if (SUCCEEDED(status)) {
            break;
          }
          std::this_thread::sleep_for(200ms);
        }

        // We don't retry with DuplicateOutput() because we can hit this codepath when we're racing
        // with mode changes and we don't want to accidentally fall back to suboptimal capture if
        // we get unlucky and succeed below.
        if (FAILED(status)) {
          BOOST_LOG(warning) << "DuplicateOutput1 Failed [0x"sv << util::hex(status).to_string_view() << ']';
          return -1;
        }
      } else {
        BOOST_LOG(warning) << "IDXGIOutput5 is not supported by your OS. Capture performance may be reduced."sv;

        dxgi::output1_t output1 {};
        status = display->output->QueryInterface(IID_IDXGIOutput1, (void **) &output1);
        if (FAILED(status)) {
          BOOST_LOG(error) << "Failed to query IDXGIOutput1 from the output"sv;
          return -1;
        }

        for (int x = 0; x < 2; ++x) {
          // Ensure we can duplicate the current display
          syncThreadDesktop();

          status = output1->DuplicateOutput((IUnknown *) display->device.get(), &dup);
          if (SUCCEEDED(status)) {
            break;
          }
          std::this_thread::sleep_for(200ms);
        }

        if (FAILED(status)) {
          BOOST_LOG(error) << "DuplicateOutput Failed [0x"sv << util::hex(status).to_string_view() << ']';
          return -1;
        }
      }
    }

    DXGI_OUTDUPL_DESC dup_desc;
    dup->GetDesc(&dup_desc);

    BOOST_LOG(info) << "Desktop resolution ["sv << dup_desc.ModeDesc.Width << 'x' << dup_desc.ModeDesc.Height << ']';
    BOOST_LOG(info) << "Desktop format ["sv << display->dxgi_format_to_string(dup_desc.ModeDesc.Format) << ']';

    display->display_refresh_rate = dup_desc.ModeDesc.RefreshRate;
    double display_refresh_rate_decimal = (double) display->display_refresh_rate.Numerator / display->display_refresh_rate.Denominator;
    BOOST_LOG(info) << "Display refresh rate [" << display_refresh_rate_decimal << "Hz]";
    if (display_reliability::should_report_capture_topology_instability(
          config::sunshineseat.mode == "sidecar"sv,
          config::seat.display_provider,
          !config.client_uuid.empty() || !config.client_name.empty(),
          dup_desc.ModeDesc.Width,
          dup_desc.ModeDesc.Height,
          display_refresh_rate_decimal
        )) {
      BOOST_LOG(warning) << "SunshineSeat sidecar capture topology is unstable [resolution="sv
                         << dup_desc.ModeDesc.Width << 'x' << dup_desc.ModeDesc.Height
                         << ", refresh_hz="sv << display_refresh_rate_decimal
                         << ", output_name="sv << config::video.output_name
                         << ", display_provider="sv << config::seat.display_provider << ']';
    }
    if (display->client_frame_rate_strict.Numerator > 0) {
      int num = display->client_frame_rate_strict.Numerator;
      int den = display->client_frame_rate_strict.Denominator;
      BOOST_LOG(info) << "Requested frame rate [" << num << "/" << den << " exactly " << av_q2d(AVRational {num, den}) << " fps]";
    } else {
      BOOST_LOG(info) << "Requested frame rate [" << display->client_frame_rate << "fps]";
    }
    display->display_refresh_rate_rounded = lround(display_refresh_rate_decimal);
    return 0;
  }

  capture_e duplication_t::next_frame(DXGI_OUTDUPL_FRAME_INFO &frame_info, std::chrono::milliseconds timeout, resource_t::pointer *res_p) {
    auto capture_status = release_frame();
    if (capture_status != capture_e::ok) {
      return capture_status;
    }

    auto status = dup->AcquireNextFrame(timeout.count(), &frame_info, res_p);

    switch (status) {
      case S_OK:
        // ProtectedContentMaskedOut seems to semi-randomly be TRUE or FALSE even when protected content
        // is on screen the whole time, so we can't just print when it changes. Instead we'll keep track
        // of the last time we printed the warning and print another if we haven't printed one recently.
        if (frame_info.ProtectedContentMaskedOut && std::chrono::steady_clock::now() > last_protected_content_warning_time + 10s) {
          BOOST_LOG(warning) << "Windows is currently blocking DRM-protected content from capture. You may see black regions where this content would be."sv;
          last_protected_content_warning_time = std::chrono::steady_clock::now();
        }

        has_frame = true;
        return capture_e::ok;
      case DXGI_ERROR_WAIT_TIMEOUT:
        return capture_e::timeout;
      case WAIT_ABANDONED:
      case DXGI_ERROR_ACCESS_LOST:
      case DXGI_ERROR_ACCESS_DENIED:
        return capture_e::reinit;
      default:
        BOOST_LOG(error) << "Couldn't acquire next frame [0x"sv << util::hex(status).to_string_view();
        return capture_e::error;
    }
  }

  capture_e duplication_t::reset(dup_t::pointer dup_p) {
    auto capture_status = release_frame();

    dup.reset(dup_p);

    return capture_status;
  }

  capture_e duplication_t::release_frame() {
    if (!has_frame) {
      return capture_e::ok;
    }

    auto status = dup->ReleaseFrame();
    has_frame = false;
    switch (status) {
      case S_OK:
        return capture_e::ok;

      case DXGI_ERROR_INVALID_CALL:
        BOOST_LOG(warning) << "Duplication frame already released";
        return capture_e::ok;

      case DXGI_ERROR_ACCESS_LOST:
        return capture_e::reinit;

      default:
        BOOST_LOG(error) << "Error while releasing duplication frame [0x"sv << util::hex(status).to_string_view();
        return capture_e::error;
    }
  }

  duplication_t::~duplication_t() {
    release_frame();
  }

  capture_e display_base_t::capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) {
    auto adjust_client_frame_rate = [&]() -> DXGI_RATIONAL {
      // Use exactly the requested rate if the client sent an X100 value
      if (client_frame_rate_strict.Numerator > 0) {
        return client_frame_rate_strict;
      }
      // Adjust capture frame interval when display refresh rate is not integral but very close to requested fps.
      if (display_refresh_rate.Denominator > 1) {
        DXGI_RATIONAL candidate = display_refresh_rate;
        if (client_frame_rate % display_refresh_rate_rounded == 0) {
          candidate.Numerator *= client_frame_rate / display_refresh_rate_rounded;
        } else if (display_refresh_rate_rounded % client_frame_rate == 0) {
          candidate.Denominator *= display_refresh_rate_rounded / client_frame_rate;
        }
        double candidate_rate = (double) candidate.Numerator / candidate.Denominator;
        // Can only decrease requested fps, otherwise client may start accumulating frames and suffer increased latency.
        if (client_frame_rate > candidate_rate && candidate_rate / client_frame_rate > 0.99) {
          BOOST_LOG(info) << "Adjusted capture rate to " << candidate_rate << "fps to better match display";
          return candidate;
        }
      }

      return {(uint32_t) client_frame_rate, 1};
    };

    DXGI_RATIONAL client_frame_rate_adjusted = adjust_client_frame_rate();
    std::optional<std::chrono::steady_clock::time_point> frame_pacing_group_start;
    uint32_t frame_pacing_group_frames = 0;

    // Keep the display awake during capture. If the display goes to sleep during
    // capture, best case is that capture stops until it powers back on. However,
    // worst case it will trigger us to reinit DD, waking the display back up in
    // a neverending cycle of waking and sleeping the display of an idle machine.
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
    auto clear_display_required = util::fail_guard([]() {
      SetThreadExecutionState(ES_CONTINUOUS);
    });

    sleep_overshoot_logger.reset();

    while (true) {
      // This will return false if the HDR state changes or for any number of other
      // display or GPU changes. We should reinit to examine the updated state of
      // the display subsystem. It is recommended to call this once per frame.
      if (!factory->IsCurrent()) {
        return platf::capture_e::reinit;
      }

      platf::capture_e status = capture_e::ok;
      std::shared_ptr<img_t> img_out;

      // Try to continue frame pacing group, snapshot() is called with zero timeout after waiting for client frame interval
      if (frame_pacing_group_start) {
        const uint32_t seconds = (uint64_t) frame_pacing_group_frames * client_frame_rate_adjusted.Denominator / client_frame_rate_adjusted.Numerator;
        const uint32_t remainder = (uint64_t) frame_pacing_group_frames * client_frame_rate_adjusted.Denominator % client_frame_rate_adjusted.Numerator;
        const auto sleep_target = *frame_pacing_group_start +
                                  std::chrono::nanoseconds(1s) * seconds +
                                  std::chrono::nanoseconds(1s) * remainder / client_frame_rate_adjusted.Numerator;
        const auto sleep_period = sleep_target - std::chrono::steady_clock::now();

        if (sleep_period <= 0ns) {
          // We missed next frame time, invalidating current frame pacing group
          frame_pacing_group_start = std::nullopt;
          frame_pacing_group_frames = 0;
          status = capture_e::timeout;
        } else {
          timer->sleep_for(sleep_period);
          sleep_overshoot_logger.first_point(sleep_target);
          sleep_overshoot_logger.second_point_now_and_log();

          status = snapshot(pull_free_image_cb, img_out, 0ms, *cursor);

          if (status == capture_e::ok && img_out) {
            frame_pacing_group_frames += 1;
          } else {
            frame_pacing_group_start = std::nullopt;
            frame_pacing_group_frames = 0;
          }
        }
      }

      // Start new frame pacing group if necessary, snapshot() is called with non-zero timeout
      if (status == capture_e::timeout || (status == capture_e::ok && !frame_pacing_group_start)) {
        status = snapshot(pull_free_image_cb, img_out, 200ms, *cursor);

        if (status == capture_e::ok && img_out) {
          frame_pacing_group_start = img_out->frame_timestamp;

          if (!frame_pacing_group_start) {
            BOOST_LOG(warning) << "snapshot() provided image without timestamp";
            frame_pacing_group_start = std::chrono::steady_clock::now();
          }

          frame_pacing_group_frames = 1;
        } else if (status == platf::capture_e::timeout) {
          // The D3D11 device is protected by an unfair lock that is held the entire time that
          // IDXGIOutputDuplication::AcquireNextFrame() is running. This is normally harmless,
          // however sometimes the encoding thread needs to interact with our ID3D11Device to
          // create dummy images or initialize the shared state that is used to pass textures
          // between the capture and encoding ID3D11Devices.
          //
          // When we're in a state where we're not actively receiving frames regularly, we will
          // spend almost 100% of our time in AcquireNextFrame() holding that critical lock.
          // Worse still, since it's unfair, we can monopolize it while the encoding thread
          // is starved. The encoding thread may acquire it for a few moments across a few
          // ID3D11Device calls before losing it again to us for another long time waiting in
          // AcquireNextFrame(). The starvation caused by this lock contention causes encoder
          // reinitialization to take several seconds instead of a fraction of a second.
          //
          // To avoid starving the encoding thread, sleep without the lock held for a little
          // while each time we reach our max frame timeout. This will only happen when nothing
          // is updating the display, so no visible stutter should be introduced by the sleep.
          std::this_thread::sleep_for(10ms);
        }
      }

      switch (status) {
        case platf::capture_e::reinit:
        case platf::capture_e::error:
        case platf::capture_e::interrupted:
          return status;
        case platf::capture_e::timeout:
          if (!push_captured_image_cb(std::move(img_out), false)) {
            return capture_e::ok;
          }
          break;
        case platf::capture_e::ok:
          if (!push_captured_image_cb(std::move(img_out), true)) {
            return capture_e::ok;
          }
          break;
        default:
          BOOST_LOG(error) << "Unrecognized capture status ["sv << (int) status << ']';
          return status;
      }

      status = release_snapshot();
      if (status != platf::capture_e::ok) {
        return status;
      }
    }

    return capture_e::ok;
  }

  /**
   * @brief Tests to determine if the Desktop Duplication API can capture the given output.
   * @details When testing for enumeration only, we avoid resyncing the thread desktop.
   * @param adapter The DXGI adapter to use for capture.
   * @param output The DXGI output to capture.
   * @param enumeration_only Specifies whether this test is occurring for display enumeration.
   */
  bool test_dxgi_duplication(adapter_t &adapter, output_t &output, bool enumeration_only) {
    D3D_FEATURE_LEVEL featureLevels[] {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
      D3D_FEATURE_LEVEL_9_3,
      D3D_FEATURE_LEVEL_9_2,
      D3D_FEATURE_LEVEL_9_1
    };

    device_t device;
    auto status = D3D11CreateDevice(
      adapter.get(),
      D3D_DRIVER_TYPE_UNKNOWN,
      nullptr,
      D3D11_CREATE_DEVICE_FLAGS,
      featureLevels,
      sizeof(featureLevels) / sizeof(D3D_FEATURE_LEVEL),
      D3D11_SDK_VERSION,
      &device,
      nullptr,
      nullptr
    );
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create D3D11 device for DD test [0x"sv << util::hex(status).to_string_view() << ']';
      return false;
    }

    output1_t output1;
    status = output->QueryInterface(IID_IDXGIOutput1, (void **) &output1);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to query IDXGIOutput1 from the output"sv;
      return false;
    }

    // Check if we can use the Desktop Duplication API on this output
    for (int x = 0; x < 2; ++x) {
      dup_t dup;

      // Only resynchronize the thread desktop when not enumerating displays.
      // During enumeration, the caller will do this only once to ensure
      // a consistent view of available outputs.
      if (!enumeration_only) {
        syncThreadDesktop();
      }

      status = output1->DuplicateOutput((IUnknown *) device.get(), &dup);
      if (SUCCEEDED(status)) {
        return true;
      }

      // If we're not resyncing the thread desktop and we don't have permission to
      // capture the current desktop, just bail immediately. Retrying won't help.
      if (enumeration_only && status == E_ACCESSDENIED) {
        break;
      } else {
        std::this_thread::sleep_for(200ms);
      }
    }

    BOOST_LOG(error) << "DuplicateOutput() test failed [0x"sv << util::hex(status).to_string_view() << ']';
    return false;
  }

  /**
   * @brief Hook for NtGdiDdDDIGetCachedHybridQueryValue() from win32u.dll.
   * @param gpuPreference A pointer to the location where the preference will be written.
   * @return Always STATUS_SUCCESS if valid arguments are provided.
   */
  NTSTATUS __stdcall NtGdiDdDDIGetCachedHybridQueryValueHook(D3DKMT_GPU_PREFERENCE_QUERY_STATE *gpuPreference) {
    // By faking a cached GPU preference state of D3DKMT_GPU_PREFERENCE_STATE_UNSPECIFIED, this will
    // prevent DXGI from performing the normal GPU preference resolution that looks at the registry,
    // power settings, and the hybrid adapter DDI interface to pick a GPU. Instead, we will not be
    // bound to any specific GPU. This will prevent DXGI from performing output reparenting (moving
    // outputs from their true location to the render GPU), which breaks DDA.
    if (gpuPreference) {
      *gpuPreference = D3DKMT_GPU_PREFERENCE_STATE_UNSPECIFIED;
      return 0;  // STATUS_SUCCESS
    } else {
      return STATUS_INVALID_PARAMETER;
    }
  }

  int display_base_t::init(const ::video::config_t &config, const std::string &display_name) {
    std::once_flag windows_cpp_once_flag;

    std::call_once(windows_cpp_once_flag, []() {
      DECLARE_HANDLE(DPI_AWARENESS_CONTEXT);

      typedef BOOL (*User32_SetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT value);

      {
        auto user32 = LoadLibraryA("user32.dll");
        auto f = (User32_SetProcessDpiAwarenessContext) GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (f) {
          f(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }

        FreeLibrary(user32);
      }

      {
        // We aren't calling MH_Uninitialize(), but that's okay because this hook lasts for the life of the process
        MH_Initialize();
        MH_CreateHookApi(L"win32u.dll", "NtGdiDdDDIGetCachedHybridQueryValue", (void *) NtGdiDdDDIGetCachedHybridQueryValueHook, nullptr);
        MH_EnableHook(MH_ALL_HOOKS);
      }
    });

    // Get rectangle of full desktop for absolute mouse coordinates
    env_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    env_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HRESULT status;

    status = CreateDXGIFactory1(IID_IDXGIFactory1, (void **) &factory);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create DXGIFactory1 [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    auto adapter_name = utf_utils::from_utf8(config::video.adapter_name);
    auto output_name = utf_utils::from_utf8(display_name);

    adapter_t::pointer adapter_p;
    for (int tries = 0; tries < 2; ++tries) {
      for (int x = 0; factory->EnumAdapters1(x, &adapter_p) != DXGI_ERROR_NOT_FOUND; ++x) {
        dxgi::adapter_t adapter_tmp {adapter_p};

        DXGI_ADAPTER_DESC1 adapter_desc;
        adapter_tmp->GetDesc1(&adapter_desc);

        if (!adapter_name.empty() && adapter_desc.Description != adapter_name) {
          continue;
        }

        dxgi::output_t::pointer output_p;
        for (int y = 0; adapter_tmp->EnumOutputs(y, &output_p) != DXGI_ERROR_NOT_FOUND; ++y) {
          dxgi::output_t output_tmp {output_p};

          DXGI_OUTPUT_DESC desc;
          output_tmp->GetDesc(&desc);

          if (!output_name.empty() && desc.DeviceName != output_name) {
            continue;
          }

          if (desc.AttachedToDesktop && test_dxgi_duplication(adapter_tmp, output_tmp, false)) {
            output = std::move(output_tmp);

            offset_x = desc.DesktopCoordinates.left;
            offset_y = desc.DesktopCoordinates.top;
            width = desc.DesktopCoordinates.right - offset_x;
            height = desc.DesktopCoordinates.bottom - offset_y;

            display_rotation = desc.Rotation;
            if (display_rotation == DXGI_MODE_ROTATION_ROTATE90 ||
                display_rotation == DXGI_MODE_ROTATION_ROTATE270) {
              width_before_rotation = height;
              height_before_rotation = width;
            } else {
              width_before_rotation = width;
              height_before_rotation = height;
            }

            // left and bottom may be negative, yet absolute mouse coordinates start at 0x0
            // Ensure offset starts at 0x0
            offset_x -= GetSystemMetrics(SM_XVIRTUALSCREEN);
            offset_y -= GetSystemMetrics(SM_YVIRTUALSCREEN);

            break;
          }
        }

        if (output) {
          adapter = std::move(adapter_tmp);
          break;
        }
      }

      if (output) {
        break;
      }

      // If we made it here without finding an output, try to power on the display and retry.
      if (tries == 0) {
        SetThreadExecutionState(ES_DISPLAY_REQUIRED);
        Sleep(500);
      }
    }

    if (!output) {
      BOOST_LOG(error) << "Failed to locate an output device"sv;
      return -1;
    }

    D3D_FEATURE_LEVEL featureLevels[] {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
      D3D_FEATURE_LEVEL_9_3,
      D3D_FEATURE_LEVEL_9_2,
      D3D_FEATURE_LEVEL_9_1
    };

    status = adapter->QueryInterface(IID_IDXGIAdapter, (void **) &adapter_p);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to query IDXGIAdapter interface"sv;
      return -1;
    }

    status = D3D11CreateDevice(
      adapter_p,
      D3D_DRIVER_TYPE_UNKNOWN,
      nullptr,
      D3D11_CREATE_DEVICE_FLAGS,
      featureLevels,
      sizeof(featureLevels) / sizeof(D3D_FEATURE_LEVEL),
      D3D11_SDK_VERSION,
      &device,
      &feature_level,
      &device_ctx
    );

    adapter_p->Release();

    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create D3D11 device [0x"sv << util::hex(status).to_string_view() << ']';

      return -1;
    }

    DXGI_ADAPTER_DESC adapter_desc;
    adapter->GetDesc(&adapter_desc);

    auto description = utf_utils::to_utf8(adapter_desc.Description);
    BOOST_LOG(info)
      << std::endl
      << "Device Description : " << description << std::endl
      << "Device Vendor ID   : 0x"sv << util::hex(adapter_desc.VendorId).to_string_view() << std::endl
      << "Device Device ID   : 0x"sv << util::hex(adapter_desc.DeviceId).to_string_view() << std::endl
      << "Device Video Mem   : "sv << adapter_desc.DedicatedVideoMemory / 1048576 << " MiB"sv << std::endl
      << "Device Sys Mem     : "sv << adapter_desc.DedicatedSystemMemory / 1048576 << " MiB"sv << std::endl
      << "Share Sys Mem      : "sv << adapter_desc.SharedSystemMemory / 1048576 << " MiB"sv << std::endl
      << "Feature Level      : 0x"sv << util::hex(feature_level).to_string_view() << std::endl
      << "Capture size       : "sv << width << 'x' << height << std::endl
      << "Offset             : "sv << offset_x << 'x' << offset_y << std::endl
      << "Virtual Desktop    : "sv << env_width << 'x' << env_height;

    // Bump up thread priority
    {
      const DWORD flags = TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY;
      TOKEN_PRIVILEGES tp;
      HANDLE token;
      LUID val;

      if (OpenProcessToken(GetCurrentProcess(), flags, &token) &&
          !!LookupPrivilegeValue(nullptr, SE_INC_BASE_PRIORITY_NAME, &val)) {
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = val;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

        if (!AdjustTokenPrivileges(token, false, &tp, sizeof(tp), nullptr, nullptr)) {
          BOOST_LOG(warning) << "Could not set privilege to increase GPU priority";
        }
      }

      CloseHandle(token);

      HMODULE gdi32 = GetModuleHandleA("GDI32");
      if (gdi32) {
        auto check_hags = [&](const LUID &adapter) -> bool {
          auto d3dkmt_open_adapter = (PD3DKMTOpenAdapterFromLuid) GetProcAddress(gdi32, "D3DKMTOpenAdapterFromLuid");
          auto d3dkmt_query_adapter_info = (PD3DKMTQueryAdapterInfo) GetProcAddress(gdi32, "D3DKMTQueryAdapterInfo");
          auto d3dkmt_close_adapter = (PD3DKMTCloseAdapter) GetProcAddress(gdi32, "D3DKMTCloseAdapter");
          if (!d3dkmt_open_adapter || !d3dkmt_query_adapter_info || !d3dkmt_close_adapter) {
            BOOST_LOG(error) << "Couldn't load d3dkmt functions from gdi32.dll to determine GPU HAGS status";
            return false;
          }

          D3DKMT_OPENADAPTERFROMLUID d3dkmt_adapter = {adapter};
          if (FAILED(d3dkmt_open_adapter(&d3dkmt_adapter))) {
            BOOST_LOG(error) << "D3DKMTOpenAdapterFromLuid() failed while trying to determine GPU HAGS status";
            return false;
          }

          bool result;

          D3DKMT_WDDM_2_7_CAPS d3dkmt_adapter_caps = {};
          D3DKMT_QUERYADAPTERINFO d3dkmt_adapter_info = {};
          d3dkmt_adapter_info.hAdapter = d3dkmt_adapter.hAdapter;
          d3dkmt_adapter_info.Type = KMTQAITYPE_WDDM_2_7_CAPS;
          d3dkmt_adapter_info.pPrivateDriverData = &d3dkmt_adapter_caps;
          d3dkmt_adapter_info.PrivateDriverDataSize = sizeof(d3dkmt_adapter_caps);

          if (SUCCEEDED(d3dkmt_query_adapter_info(&d3dkmt_adapter_info))) {
            result = d3dkmt_adapter_caps.HwSchEnabled;
          } else {
            BOOST_LOG(warning) << "D3DKMTQueryAdapterInfo() failed while trying to determine GPU HAGS status";
            result = false;
          }

          D3DKMT_CLOSEADAPTER d3dkmt_close_adapter_wrap = {d3dkmt_adapter.hAdapter};
          if (FAILED(d3dkmt_close_adapter(&d3dkmt_close_adapter_wrap))) {
            BOOST_LOG(error) << "D3DKMTCloseAdapter() failed while trying to determine GPU HAGS status";
          }

          return result;
        };

        auto d3dkmt_set_process_priority = (PD3DKMTSetProcessSchedulingPriorityClass) GetProcAddress(gdi32, "D3DKMTSetProcessSchedulingPriorityClass");
        if (d3dkmt_set_process_priority) {
          auto priority = D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME;
          bool hags_enabled = check_hags(adapter_desc.AdapterLuid);
          if (hags_enabled && adapter_desc.VendorId != 0x10DE) {
            // AMD AMF/D3D11 can intermittently block in avcodec_send_frame when
            // HAGS is active and Sunshine is assigned the realtime GPU class.
            // Keep the encoder on the GPU, but use the bounded high class for
            // non-NVIDIA adapters; realtime remains the NVIDIA default unless
            // its existing compatibility switch opts out.
            priority = D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH;
          } else if (adapter_desc.VendorId == 0x10DE) {
            // As of 2023.07, NVIDIA driver has unfixed bug(s) where "realtime" can cause unrecoverable encoding freeze or outright driver crash
            // This issue happens more frequently with HAGS, in DX12 games or when VRAM is filled close to max capacity
            // Track OBS to see if they find better workaround or NVIDIA fixes it on their end, they seem to be in communication
            if (hags_enabled && !config::video.nv_realtime_hags) {
              priority = D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH;
            }
          }
          BOOST_LOG(info) << "Active GPU has HAGS " << (hags_enabled ? "enabled" : "disabled");
          BOOST_LOG(info) << "Using " << (priority == D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH ? "high" : "realtime") << " GPU priority";
          if (FAILED(d3dkmt_set_process_priority(GetCurrentProcess(), priority))) {
            BOOST_LOG(warning) << "Failed to adjust GPU priority. Please run application as administrator for optimal performance.";
          }
        } else {
          BOOST_LOG(error) << "Couldn't load D3DKMTSetProcessSchedulingPriorityClass function from gdi32.dll to adjust GPU priority";
        }
      }

      dxgi::dxgi_t dxgi;
      status = device->QueryInterface(IID_IDXGIDevice, (void **) &dxgi);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to query DXGI interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      status = dxgi->SetGPUThreadPriority(7);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to increase capture GPU thread priority. Please run application as administrator for optimal performance.";
      }
    }

    // Try to reduce latency
    {
      dxgi::dxgi1_t dxgi {};
      status = device->QueryInterface(IID_IDXGIDevice, (void **) &dxgi);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to query DXGI interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      status = dxgi->SetMaximumFrameLatency(1);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to set maximum frame latency [0x"sv << util::hex(status).to_string_view() << ']';
      }
    }

    client_frame_rate = config.framerate;
    client_frame_rate_strict = {0, 0};
    if (config.framerateX100 > 0) {
      AVRational fps = ::video::framerateX100_to_rational(config.framerateX100);
      client_frame_rate_strict = DXGI_RATIONAL {static_cast<UINT>(fps.num), static_cast<UINT>(fps.den)};
    }

    dxgi::output6_t output6 {};
    status = output->QueryInterface(IID_IDXGIOutput6, (void **) &output6);
    if (SUCCEEDED(status)) {
      DXGI_OUTPUT_DESC1 desc1;
      output6->GetDesc1(&desc1);

      BOOST_LOG(info)
        << std::endl
        << "Colorspace         : "sv << colorspace_to_string(desc1.ColorSpace) << std::endl
        << "Bits Per Color     : "sv << desc1.BitsPerColor << std::endl
        << "Red Primary        : ["sv << desc1.RedPrimary[0] << ',' << desc1.RedPrimary[1] << ']' << std::endl
        << "Green Primary      : ["sv << desc1.GreenPrimary[0] << ',' << desc1.GreenPrimary[1] << ']' << std::endl
        << "Blue Primary       : ["sv << desc1.BluePrimary[0] << ',' << desc1.BluePrimary[1] << ']' << std::endl
        << "White Point        : ["sv << desc1.WhitePoint[0] << ',' << desc1.WhitePoint[1] << ']' << std::endl
        << "Min Luminance      : "sv << desc1.MinLuminance << " nits"sv << std::endl
        << "Max Luminance      : "sv << desc1.MaxLuminance << " nits"sv << std::endl
        << "Max Full Luminance : "sv << desc1.MaxFullFrameLuminance << " nits"sv;
    }

    if (!timer || !*timer) {
      BOOST_LOG(error) << "Uninitialized high precision timer";
      return -1;
    }

    return 0;
  }

  bool display_base_t::is_hdr() {
    dxgi::output6_t output6 {};

    auto status = output->QueryInterface(IID_IDXGIOutput6, (void **) &output6);
    if (FAILED(status)) {
      BOOST_LOG(warning) << "Failed to query IDXGIOutput6 from the output"sv;
      return false;
    }

    DXGI_OUTPUT_DESC1 desc1;
    output6->GetDesc1(&desc1);

    return desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
  }

  bool display_base_t::get_hdr_metadata(SS_HDR_METADATA &metadata) {
    dxgi::output6_t output6 {};

    std::memset(&metadata, 0, sizeof(metadata));

    auto status = output->QueryInterface(IID_IDXGIOutput6, (void **) &output6);
    if (FAILED(status)) {
      BOOST_LOG(warning) << "Failed to query IDXGIOutput6 from the output"sv;
      return false;
    }

    DXGI_OUTPUT_DESC1 desc1;
    output6->GetDesc1(&desc1);

    // The primaries reported here seem to correspond to scRGB (Rec. 709)
    // which we then convert to Rec 2020 in our scRGB FP16 -> PQ shader
    // prior to encoding. It's not clear to me if we're supposed to report
    // the primaries of the original colorspace or the one we've converted
    // it to, but let's just report Rec 2020 primaries and D65 white level
    // to avoid confusing clients by reporting Rec 709 primaries with a
    // Rec 2020 colorspace. It seems like most clients ignore the primaries
    // in the metadata anyway (luminance range is most important).
    desc1.RedPrimary[0] = 0.708f;
    desc1.RedPrimary[1] = 0.292f;
    desc1.GreenPrimary[0] = 0.170f;
    desc1.GreenPrimary[1] = 0.797f;
    desc1.BluePrimary[0] = 0.131f;
    desc1.BluePrimary[1] = 0.046f;
    desc1.WhitePoint[0] = 0.3127f;
    desc1.WhitePoint[1] = 0.3290f;

    metadata.displayPrimaries[0].x = desc1.RedPrimary[0] * 50000;
    metadata.displayPrimaries[0].y = desc1.RedPrimary[1] * 50000;
    metadata.displayPrimaries[1].x = desc1.GreenPrimary[0] * 50000;
    metadata.displayPrimaries[1].y = desc1.GreenPrimary[1] * 50000;
    metadata.displayPrimaries[2].x = desc1.BluePrimary[0] * 50000;
    metadata.displayPrimaries[2].y = desc1.BluePrimary[1] * 50000;

    metadata.whitePoint.x = desc1.WhitePoint[0] * 50000;
    metadata.whitePoint.y = desc1.WhitePoint[1] * 50000;

    metadata.maxDisplayLuminance = desc1.MaxLuminance;
    metadata.minDisplayLuminance = desc1.MinLuminance * 10000;

    // These are content-specific metadata parameters that this interface doesn't give us
    metadata.maxContentLightLevel = 0;
    metadata.maxFrameAverageLightLevel = 0;

    metadata.maxFullFrameLuminance = desc1.MaxFullFrameLuminance;

    return true;
  }

  const char *format_str[] = {
    "DXGI_FORMAT_UNKNOWN",
    "DXGI_FORMAT_R32G32B32A32_TYPELESS",
    "DXGI_FORMAT_R32G32B32A32_FLOAT",
    "DXGI_FORMAT_R32G32B32A32_UINT",
    "DXGI_FORMAT_R32G32B32A32_SINT",
    "DXGI_FORMAT_R32G32B32_TYPELESS",
    "DXGI_FORMAT_R32G32B32_FLOAT",
    "DXGI_FORMAT_R32G32B32_UINT",
    "DXGI_FORMAT_R32G32B32_SINT",
    "DXGI_FORMAT_R16G16B16A16_TYPELESS",
    "DXGI_FORMAT_R16G16B16A16_FLOAT",
    "DXGI_FORMAT_R16G16B16A16_UNORM",
    "DXGI_FORMAT_R16G16B16A16_UINT",
    "DXGI_FORMAT_R16G16B16A16_SNORM",
    "DXGI_FORMAT_R16G16B16A16_SINT",
    "DXGI_FORMAT_R32G32_TYPELESS",
    "DXGI_FORMAT_R32G32_FLOAT",
    "DXGI_FORMAT_R32G32_UINT",
    "DXGI_FORMAT_R32G32_SINT",
    "DXGI_FORMAT_R32G8X24_TYPELESS",
    "DXGI_FORMAT_D32_FLOAT_S8X24_UINT",
    "DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS",
    "DXGI_FORMAT_X32_TYPELESS_G8X24_UINT",
    "DXGI_FORMAT_R10G10B10A2_TYPELESS",
    "DXGI_FORMAT_R10G10B10A2_UNORM",
    "DXGI_FORMAT_R10G10B10A2_UINT",
    "DXGI_FORMAT_R11G11B10_FLOAT",
    "DXGI_FORMAT_R8G8B8A8_TYPELESS",
    "DXGI_FORMAT_R8G8B8A8_UNORM",
    "DXGI_FORMAT_R8G8B8A8_UNORM_SRGB",
    "DXGI_FORMAT_R8G8B8A8_UINT",
    "DXGI_FORMAT_R8G8B8A8_SNORM",
    "DXGI_FORMAT_R8G8B8A8_SINT",
    "DXGI_FORMAT_R16G16_TYPELESS",
    "DXGI_FORMAT_R16G16_FLOAT",
    "DXGI_FORMAT_R16G16_UNORM",
    "DXGI_FORMAT_R16G16_UINT",
    "DXGI_FORMAT_R16G16_SNORM",
    "DXGI_FORMAT_R16G16_SINT",
    "DXGI_FORMAT_R32_TYPELESS",
    "DXGI_FORMAT_D32_FLOAT",
    "DXGI_FORMAT_R32_FLOAT",
    "DXGI_FORMAT_R32_UINT",
    "DXGI_FORMAT_R32_SINT",
    "DXGI_FORMAT_R24G8_TYPELESS",
    "DXGI_FORMAT_D24_UNORM_S8_UINT",
    "DXGI_FORMAT_R24_UNORM_X8_TYPELESS",
    "DXGI_FORMAT_X24_TYPELESS_G8_UINT",
    "DXGI_FORMAT_R8G8_TYPELESS",
    "DXGI_FORMAT_R8G8_UNORM",
    "DXGI_FORMAT_R8G8_UINT",
    "DXGI_FORMAT_R8G8_SNORM",
    "DXGI_FORMAT_R8G8_SINT",
    "DXGI_FORMAT_R16_TYPELESS",
    "DXGI_FORMAT_R16_FLOAT",
    "DXGI_FORMAT_D16_UNORM",
    "DXGI_FORMAT_R16_UNORM",
    "DXGI_FORMAT_R16_UINT",
    "DXGI_FORMAT_R16_SNORM",
    "DXGI_FORMAT_R16_SINT",
    "DXGI_FORMAT_R8_TYPELESS",
    "DXGI_FORMAT_R8_UNORM",
    "DXGI_FORMAT_R8_UINT",
    "DXGI_FORMAT_R8_SNORM",
    "DXGI_FORMAT_R8_SINT",
    "DXGI_FORMAT_A8_UNORM",
    "DXGI_FORMAT_R1_UNORM",
    "DXGI_FORMAT_R9G9B9E5_SHAREDEXP",
    "DXGI_FORMAT_R8G8_B8G8_UNORM",
    "DXGI_FORMAT_G8R8_G8B8_UNORM",
    "DXGI_FORMAT_BC1_TYPELESS",
    "DXGI_FORMAT_BC1_UNORM",
    "DXGI_FORMAT_BC1_UNORM_SRGB",
    "DXGI_FORMAT_BC2_TYPELESS",
    "DXGI_FORMAT_BC2_UNORM",
    "DXGI_FORMAT_BC2_UNORM_SRGB",
    "DXGI_FORMAT_BC3_TYPELESS",
    "DXGI_FORMAT_BC3_UNORM",
    "DXGI_FORMAT_BC3_UNORM_SRGB",
    "DXGI_FORMAT_BC4_TYPELESS",
    "DXGI_FORMAT_BC4_UNORM",
    "DXGI_FORMAT_BC4_SNORM",
    "DXGI_FORMAT_BC5_TYPELESS",
    "DXGI_FORMAT_BC5_UNORM",
    "DXGI_FORMAT_BC5_SNORM",
    "DXGI_FORMAT_B5G6R5_UNORM",
    "DXGI_FORMAT_B5G5R5A1_UNORM",
    "DXGI_FORMAT_B8G8R8A8_UNORM",
    "DXGI_FORMAT_B8G8R8X8_UNORM",
    "DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM",
    "DXGI_FORMAT_B8G8R8A8_TYPELESS",
    "DXGI_FORMAT_B8G8R8A8_UNORM_SRGB",
    "DXGI_FORMAT_B8G8R8X8_TYPELESS",
    "DXGI_FORMAT_B8G8R8X8_UNORM_SRGB",
    "DXGI_FORMAT_BC6H_TYPELESS",
    "DXGI_FORMAT_BC6H_UF16",
    "DXGI_FORMAT_BC6H_SF16",
    "DXGI_FORMAT_BC7_TYPELESS",
    "DXGI_FORMAT_BC7_UNORM",
    "DXGI_FORMAT_BC7_UNORM_SRGB",
    "DXGI_FORMAT_AYUV",
    "DXGI_FORMAT_Y410",
    "DXGI_FORMAT_Y416",
    "DXGI_FORMAT_NV12",
    "DXGI_FORMAT_P010",
    "DXGI_FORMAT_P016",
    "DXGI_FORMAT_420_OPAQUE",
    "DXGI_FORMAT_YUY2",
    "DXGI_FORMAT_Y210",
    "DXGI_FORMAT_Y216",
    "DXGI_FORMAT_NV11",
    "DXGI_FORMAT_AI44",
    "DXGI_FORMAT_IA44",
    "DXGI_FORMAT_P8",
    "DXGI_FORMAT_A8P8",
    "DXGI_FORMAT_B4G4R4A4_UNORM",

    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,

    "DXGI_FORMAT_P208",
    "DXGI_FORMAT_V208",
    "DXGI_FORMAT_V408"
  };

  const char *display_base_t::dxgi_format_to_string(DXGI_FORMAT format) {
    return format_str[format];
  }

  const char *display_base_t::colorspace_to_string(DXGI_COLOR_SPACE_TYPE type) {
    const char *type_str[] = {
      "DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709",
      "DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709",
      "DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709",
      "DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P2020",
      "DXGI_COLOR_SPACE_RESERVED",
      "DXGI_COLOR_SPACE_YCBCR_FULL_G22_NONE_P709_X601",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601",
      "DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709",
      "DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020",
      "DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020",
      "DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020",
      "DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_TOPLEFT_P2020",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020",
      "DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020",
      "DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020",
      "DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P709",
      "DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P2020",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_LEFT_P709",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_LEFT_P2020",
      "DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_TOPLEFT_P2020",
    };

    if (type < ARRAYSIZE(type_str)) {
      return type_str[type];
    } else {
      return "UNKNOWN";
    }
  }

}  // namespace platf::dxgi

namespace platf::sudovda {
  namespace {
    bool is_sudovda_adapter(const DXGI_ADAPTER_DESC1 &adapter_desc) {
      const std::wstring description {adapter_desc.Description};

      return description.find(L"SudoMaker") != std::wstring::npos ||
             description.find(L"SudoVDA") != std::wstring::npos;
    }

    std::wstring lowercase(std::wstring value) {
      std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
      });
      return value;
    }

    bool is_sudovda_output_name(const wchar_t *device_name) {
      if (!device_name || !*device_name) {
        return false;
      }

      DISPLAY_DEVICEW device {};
      device.cb = sizeof(device);
      if (!EnumDisplayDevicesW(device_name, 0, &device, 0)) {
        return false;
      }

      const auto device_id = lowercase(device.DeviceID);
      const auto device_string = lowercase(device.DeviceString);
      return device_id.find(L"root\\sudomaker\\sudovda") != std::wstring::npos ||
             device_string.find(L"sudomaker") != std::wstring::npos ||
             device_string.find(L"sudovda") != std::wstring::npos;
    }
  }  // namespace

  std::vector<std::string> capturable_display_names(bool log_missing_output) {
    using namespace std::literals;

    std::vector<std::string> display_names;

    HRESULT status;

    BOOST_LOG(debug) << "Detecting SudoVDA monitors..."sv;

    const auto sudovda_status = query_status(false);
    if (sudovda_status.device_paths.empty()) {
      BOOST_LOG(error) << "SudoVDA device interface is not present."sv;
    } else {
      BOOST_LOG(debug) << "SudoVDA device interfaces present: "sv << sudovda_status.device_paths.size();
    }

    syncThreadDesktop();

    dxgi::factory1_t factory;
    status = CreateDXGIFactory1(IID_IDXGIFactory1, (void **) &factory);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create DXGIFactory1 [0x"sv << util::hex(status).to_string_view() << ']';
      return {};
    }

    dxgi::adapter_t::pointer adapter_p;
    const bool probe_duplication = sudovda::capture_requires_dxgi_duplication_probe(config::video.capture);
    for (int x = 0; factory->EnumAdapters1(x, &adapter_p) != DXGI_ERROR_NOT_FOUND; ++x) {
      dxgi::adapter_t adapter {adapter_p};
      DXGI_ADAPTER_DESC1 adapter_desc;
      adapter->GetDesc1(&adapter_desc);

      const auto adapter_name = utf_utils::to_utf8(adapter_desc.Description);
      const bool adapter_is_sudovda = is_sudovda_adapter(adapter_desc);
      BOOST_LOG(debug) << "Checking adapter for SudoVDA provider: "sv << adapter_name;

      dxgi::output_t::pointer output_p {};
      for (int y = 0; adapter->EnumOutputs(y, &output_p) != DXGI_ERROR_NOT_FOUND; ++y) {
        dxgi::output_t output {output_p};

        DXGI_OUTPUT_DESC desc;
        output->GetDesc(&desc);

        auto device_name = utf_utils::to_utf8(desc.DeviceName);
        auto width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
        auto height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

        BOOST_LOG(debug)
          << "    SudoVDA Output    : "sv << device_name << std::endl
          << "    AttachedToDesktop : "sv << (desc.AttachedToDesktop ? "yes"sv : "no"sv) << std::endl
          << "    Resolution        : "sv << width << 'x' << height << std::endl
          << std::endl;

        const bool output_is_sudovda = is_sudovda_output_name(desc.DeviceName);
        const bool output_is_tracked = is_tracked_display_name(device_name);
        if ((adapter_is_sudovda || output_is_sudovda || output_is_tracked) && desc.AttachedToDesktop &&
            (!probe_duplication || dxgi::test_dxgi_duplication(adapter, output, true))) {
          display_names.emplace_back(std::move(device_name));
        }
      }
    }

    if (display_names.empty() && log_missing_output) {
      BOOST_LOG(error) << "No capturable SudoVDA outputs are attached to the desktop."sv;
    }

    return display_names;
  }
}  // namespace platf::sudovda

namespace platf {
  namespace {
    class display_provider_t {
    public:
      virtual ~display_provider_t() = default;

      virtual std::string_view id() const = 0;
      virtual std::shared_ptr<display_t> display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) = 0;
      virtual std::vector<std::string> display_names(mem_type_e hwdevice_type) = 0;
    };

    class desktop_display_provider_t: public display_provider_t {
    public:
      std::string_view id() const override {
        return "desktop"sv;
      }

      std::shared_ptr<display_t> display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) override {
        if (config::video.capture == "ddx" || config::video.capture.empty()) {
          if (hwdevice_type == mem_type_e::dxgi) {
            auto disp = std::make_shared<dxgi::display_ddup_vram_t>();

            if (!disp->init(config, display_name)) {
              return disp;
            }
          } else if (hwdevice_type == mem_type_e::system) {
            auto disp = std::make_shared<dxgi::display_ddup_ram_t>();

            if (!disp->init(config, display_name)) {
              return disp;
            }
          }
        }

        if (config::video.capture == "wgc" || config::video.capture.empty()) {
          if (hwdevice_type == mem_type_e::dxgi) {
            auto disp = std::make_shared<dxgi::display_wgc_vram_t>();

            if (!disp->init(config, display_name)) {
              return disp;
            }
          } else if (hwdevice_type == mem_type_e::system) {
            auto disp = std::make_shared<dxgi::display_wgc_ram_t>();

            if (!disp->init(config, display_name)) {
              return disp;
            }
          }
        }

        // ddx and wgc failed
        return nullptr;
      }

      std::vector<std::string> display_names(mem_type_e) override {
        std::vector<std::string> display_names;

        HRESULT status;

        BOOST_LOG(debug) << "Detecting monitors..."sv;

        // We sync the thread desktop once before we start the enumeration process
        // to ensure test_dxgi_duplication() returns consistent results for all GPUs
        // even if the current desktop changes during our enumeration process.
        // It is critical that we either fully succeed in enumeration or fully fail,
        // otherwise it can lead to the capture code switching monitors unexpectedly.
        syncThreadDesktop();

        dxgi::factory1_t factory;
        status = CreateDXGIFactory1(IID_IDXGIFactory1, (void **) &factory);
        if (FAILED(status)) {
          BOOST_LOG(error) << "Failed to create DXGIFactory1 [0x"sv << util::hex(status).to_string_view() << ']';
          return {};
        }

        dxgi::adapter_t::pointer adapter_p;
        for (int x = 0; factory->EnumAdapters1(x, &adapter_p) != DXGI_ERROR_NOT_FOUND; ++x) {
          dxgi::adapter_t adapter {adapter_p};
          DXGI_ADAPTER_DESC1 adapter_desc;
          adapter->GetDesc1(&adapter_desc);

          BOOST_LOG(debug)
            << std::endl
            << "====== ADAPTER ====="sv << std::endl
            << "Device Name      : "sv << utf_utils::to_utf8(adapter_desc.Description) << std::endl
            << "Device Vendor ID : 0x"sv << util::hex(adapter_desc.VendorId).to_string_view() << std::endl
            << "Device Device ID : 0x"sv << util::hex(adapter_desc.DeviceId).to_string_view() << std::endl
            << "Device Video Mem : "sv << adapter_desc.DedicatedVideoMemory / 1048576 << " MiB"sv << std::endl
            << "Device Sys Mem   : "sv << adapter_desc.DedicatedSystemMemory / 1048576 << " MiB"sv << std::endl
            << "Share Sys Mem    : "sv << adapter_desc.SharedSystemMemory / 1048576 << " MiB"sv << std::endl
            << std::endl
            << "    ====== OUTPUT ======"sv << std::endl;

          dxgi::output_t::pointer output_p {};
          for (int y = 0; adapter->EnumOutputs(y, &output_p) != DXGI_ERROR_NOT_FOUND; ++y) {
            dxgi::output_t output {output_p};

            DXGI_OUTPUT_DESC desc;
            output->GetDesc(&desc);

            auto device_name = utf_utils::to_utf8(desc.DeviceName);

            auto width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
            auto height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

            BOOST_LOG(debug)
              << "    Output Name       : "sv << device_name << std::endl
              << "    AttachedToDesktop : "sv << (desc.AttachedToDesktop ? "yes"sv : "no"sv) << std::endl
              << "    Resolution        : "sv << width << 'x' << height << std::endl
              << std::endl;

            // Don't include the display in the list if we can't actually capture it
            if (desc.AttachedToDesktop && dxgi::test_dxgi_duplication(adapter, output, true)) {
              display_names.emplace_back(std::move(device_name));
            }
          }
        }

        return display_names;
      }
    };

    struct sudovda_cached_allocation_t {
      sudovda::display_request_t request;
      std::shared_ptr<sudovda::virtual_display_allocation_t> allocation;
      std::string device_id;
      std::string display_name;
    };

    struct sudovda_display_owner_t {
      // Declare the allocation first so C++ destroys the capture display
      // before releasing the virtual monitor. Destroying the allocation first
      // hot-unplugs the DXGI output while Desktop Duplication still owns it,
      // which can corrupt the Windows graphics heap during teardown.
      std::shared_ptr<sudovda_cached_allocation_t> allocation;
      std::shared_ptr<display_t> display;
    };

    std::mutex sudovda_cached_allocation_mutex;
    // Serialize allocation inspection, creation, and caching. Without this
    // boundary two reconnect paths can both observe an empty cache and one
    // path can remove the display that the other path is still creating.
    std::mutex sudovda_allocation_transition_mutex;
    std::shared_ptr<sudovda_cached_allocation_t> sudovda_cached_allocation;
    std::mutex sudovda_release_worker_mutex;
    std::jthread sudovda_release_worker;
    constexpr auto sudovda_reconnect_grace_duration = display_reliability::sudovda_reconnect_grace();

    void cancel_deferred_sudovda_release_locked() {
      if (!sudovda_release_worker.joinable()) {
        return;
      }

      sudovda_release_worker.request_stop();
      sudovda_release_worker = std::jthread {};
      BOOST_LOG(debug) << "SunshineSeat SudoVDA reconnect grace release cancelled"sv;
    }

    std::unique_lock<std::mutex> acquire_sudovda_release_barrier() {
      std::unique_lock worker_lock {sudovda_release_worker_mutex, std::defer_lock};
      const bool release_transition_in_progress = !worker_lock.try_lock();
      if (display_reliability::should_defer_sudovda_allocation(release_transition_in_progress)) {
        worker_lock.lock();
      }
      return worker_lock;
    }

    void remove_sudovda_allocation(const std::shared_ptr<sudovda_cached_allocation_t> &allocation, std::string_view reason) {
      if (!allocation || !allocation->allocation) {
        return;
      }

      DWORD remove_error = ERROR_SUCCESS;
      BOOST_LOG(info) << "SunshineSeat SudoVDA provider removing virtual display allocation reason="sv << reason
                      << " display="sv << allocation->display_name;
      if (allocation->allocation->remove(remove_error)) {
        BOOST_LOG(info) << "sudovda_release_success display="sv << allocation->display_name;
      } else {
        BOOST_LOG(error) << "sudovda_release_failed display="sv << allocation->display_name
                         << " error="sv << remove_error;
      }
    }

    bool same_sudovda_request(const sudovda::display_request_t &lhs, const sudovda::display_request_t &rhs) {
      return lhs.width == rhs.width &&
             lhs.height == rhs.height &&
             lhs.refresh_rate == rhs.refresh_rate &&
             IsEqualGUID(lhs.monitor_guid, rhs.monitor_guid) &&
             lhs.device_name == rhs.device_name &&
             lhs.serial_number == rhs.serial_number;
    }

    bool contains_display_name(const std::vector<std::string> &display_names, const std::string &display_name) {
      return std::find(std::begin(display_names), std::end(display_names), display_name) != std::end(display_names);
    }

    bool wait_for_stable_sudovda_display(
      const std::string &expected_display_name,
      std::vector<std::string> &display_names,
      const std::chrono::steady_clock::duration timeout = std::chrono::seconds(20),
      const std::shared_ptr<std::atomic_bool> &abort_request = {}
    ) {
      constexpr auto stable_display_poll_interval = std::chrono::milliseconds(250);
      int consecutive_hits = 0;
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      while (std::chrono::steady_clock::now() < deadline) {
        if (abort_request && abort_request->load(std::memory_order_acquire)) {
          return false;
        }
        display_names = sudovda::capturable_display_names(false);
        const bool matched = expected_display_name.empty() ? !display_names.empty() : contains_display_name(display_names, expected_display_name);
        if (matched) {
          ++consecutive_hits;
          if (consecutive_hits >= 2) {
            return true;
          }
        } else {
          consecutive_hits = 0;
        }

        std::this_thread::sleep_for(stable_display_poll_interval);
      }

      return false;
    }

    bool has_sudovda_stream_identity(const video::config_t &config) {
      return !config.client_uuid.empty() || !config.client_name.empty();
    }

    std::string dual_display_handshake_expiry_utc() {
      const auto expires_at = std::chrono::system_clock::now() + config::stream.ping_timeout;
      const auto expires_time = std::chrono::system_clock::to_time_t(expires_at);
      std::tm expires_utc {};
      if (gmtime_s(&expires_utc, &expires_time) != 0) {
        return {};
      }

      char encoded[sizeof "2030-01-01T00:00:00Z"] {};
      if (std::strftime(encoded, sizeof(encoded), "%Y-%m-%dT%H:%M:%SZ", &expires_utc) == 0) {
        return {};
      }
      return encoded;
    }

    sudovda::display_request_t make_sudovda_stream_request(const video::config_t &config) {
      const UINT width = config.width > 0 ? static_cast<UINT>(config.width) : 1920;
      const UINT height = config.height > 0 ? static_cast<UINT>(config.height) : 1080;
      const UINT refresh_rate = config.framerate > 0 ? static_cast<UINT>(config.framerate) : 60;

      const auto identity = !config.client_uuid.empty() ? config.client_uuid : config.client_name;
      if (identity.empty()) {
        return sudovda::make_default_request(width, height, refresh_rate);
      }

      auto request = sudovda::make_persistent_request(width, height, refresh_rate, identity);
      BOOST_LOG(info) << "SunshineSeat SudoVDA using persistent client virtual display identity for "sv
                      << (!config.client_name.empty() ? config.client_name : config.client_uuid)
                      << " serial="sv << request.serial_number;
      return request;
    }

    std::shared_ptr<sudovda_cached_allocation_t> get_sudovda_cached_allocation_reference(const std::string &display_name) {
      std::lock_guard lock {sudovda_cached_allocation_mutex};
      if (!sudovda_cached_allocation || !sudovda_cached_allocation->allocation) {
        return nullptr;
      }

      if (!display_name.empty() && sudovda_cached_allocation->display_name != display_name) {
        return nullptr;
      }

      return sudovda_cached_allocation;
    }

    std::vector<std::string> ensure_sudovda_allocation(
      const sudovda::display_request_t &request,
      const std::shared_ptr<std::atomic_bool> &abort_request = {}
    ) {
      std::unique_lock allocation_transition_lock {sudovda_allocation_transition_mutex};
      if (abort_request && abort_request->load(std::memory_order_acquire)) {
        return {};
      }
      // This barrier covers the complete release/reconnect transaction. In
      // particular, a new preflight cannot observe an allocation after it has
      // been detached from the cache but before its topology restore/remove
      // operation has finished.
      auto release_barrier = acquire_sudovda_release_barrier();
      cancel_deferred_sudovda_release_locked();
      std::vector<std::string> display_names;
      std::shared_ptr<sudovda_cached_allocation_t> stale_allocation;
      {
        std::lock_guard lock {sudovda_cached_allocation_mutex};
        if (sudovda_cached_allocation && sudovda_cached_allocation->allocation) {
          if (wait_for_stable_sudovda_display(
                sudovda_cached_allocation->display_name,
                display_names,
                display_reliability::sudovda_cached_display_probe_timeout(),
                abort_request
              )) {
            if (same_sudovda_request(sudovda_cached_allocation->request, request)) {
              BOOST_LOG(debug) << "SunshineSeat SudoVDA provider reusing stream virtual display: "sv << sudovda_cached_allocation->display_name;
            } else {
              BOOST_LOG(warning) << "SunshineSeat SudoVDA provider keeping existing stream virtual display until disconnect: "sv
                                 << sudovda_cached_allocation->display_name;
            }
            return display_names;
          }

          const auto available_devices = display_device::enumerate_devices();
          const bool cached_device_present = std::any_of(
            available_devices.begin(),
            available_devices.end(),
            [&](const auto &device) {
              // QueryType::All can retain an inactive SudoVDA device identity
              // after its output has disappeared from the active topology.
              // That record cannot back Desktop Duplication capture and must
              // not prevent the cached allocation from being recreated.
              return device.m_device_id == sudovda_cached_allocation->device_id &&
                     device.m_info.has_value();
            }
          );
          if (display_reliability::should_recreate_missing_sudovda_allocation(
                true,
                !sudovda_cached_allocation->device_id.empty(),
                !available_devices.empty(),
                !display_names.empty(),
                cached_device_present
              )) {
            BOOST_LOG(warning) << "SunshineSeat SudoVDA cached virtual display device disappeared; recreating allocation: "sv
                               << sudovda_cached_allocation->display_name;
            stale_allocation = std::move(sudovda_cached_allocation);
            sudovda_cached_allocation.reset();
          }

          if (!stale_allocation) {
            // DXGI duplication can report no capturable outputs for a short
            // interval while the encoder/display is being reinitialized. The
            // allocation is still the stream's authoritative lease; releasing
            // it here causes a second topology hotplug during recovery and
            // turns a recoverable encoder stall into a visible freeze.
            BOOST_LOG(warning) << "SunshineSeat SudoVDA cached virtual display is temporarily not capturable; keeping allocation for capture retry: "sv
                             << sudovda_cached_allocation->display_name;
            if (!sudovda_cached_allocation->display_name.empty() &&
                !contains_display_name(display_names, sudovda_cached_allocation->display_name)) {
              display_names.emplace_back(sudovda_cached_allocation->display_name);
            }

            // The allocation can survive a DXGI reset while its output is no
            // longer attached to the desktop. Re-apply the stream display topology
            // in place before capture retries; releasing the lease would create
            // another hotplug and invalidate the AMF surface a second time.
            if (!sudovda_cached_allocation->device_id.empty()) {
              BOOST_LOG(info) << "SunshineSeat SudoVDA reapplying cached stream display topology for capture retry: "sv
                              << sudovda_cached_allocation->display_name;
              if (!sudovda_cached_allocation->allocation->reapply_stream_topology()) {
                BOOST_LOG(warning) << "SunshineSeat SudoVDA cached stream topology reapply could not be verified: "sv
                                   << sudovda_cached_allocation->display_name;
              }
            }
            return display_names;
          }
        }
      }

      if (stale_allocation) {
        remove_sudovda_allocation(stale_allocation, "cached_device_missing"sv);
      }

      if (abort_request && abort_request->load(std::memory_order_acquire)) {
        return {};
      }

      for (unsigned int attempt = 1; ; ++attempt) {
        auto allocation_result = sudovda::create_virtual_display_allocation(request, false);
        if (!allocation_result.allocation) {
          BOOST_LOG(error) << "SunshineSeat SudoVDA provider could not allocate a virtual display for streaming."sv;
          for (const auto &blocker : allocation_result.result.plan.blockers) {
            BOOST_LOG(error) << "SunshineSeat SudoVDA allocation blocker: "sv << blocker;
          }
          return {};
        }

        display_names = std::move(allocation_result.result.provider_display_names);
        auto allocated_display_name = allocation_result.result.display_name.value_or(allocation_result.allocation->display_name());
        if (!allocated_display_name.empty() && !contains_display_name(display_names, allocated_display_name)) {
          display_names.emplace_back(allocated_display_name);
        }

        const bool stable = wait_for_stable_sudovda_display(allocated_display_name, display_names, std::chrono::seconds(20), abort_request);
        if (!stable) {
          BOOST_LOG(error) << "SunshineSeat SudoVDA allocated display did not become stable; releasing partial allocation: "sv
                           << allocated_display_name;
          DWORD remove_error = ERROR_SUCCESS;
          if (allocation_result.allocation->remove(remove_error)) {
            BOOST_LOG(info) << "sudovda_release_success display="sv << allocated_display_name;
          } else {
            BOOST_LOG(error) << "sudovda_release_failed display="sv << allocated_display_name
                             << " error="sv << remove_error;
          }
          allocation_result.allocation.reset();

          const bool aborted = abort_request && abort_request->load(std::memory_order_acquire);
          if (!display_reliability::should_retry_sudovda_allocation(stable, aborted, attempt)) {
            return {};
          }
          BOOST_LOG(warning) << "SunshineSeat SudoVDA retrying one transient allocation stabilization failure"sv;
          continue;
        }

        if (allocation_result.result.device_id) {
          BOOST_LOG(info) << "SunshineSeat SudoVDA provider using verified stream topology for device: "sv
                          << *allocation_result.result.device_id;
          const auto remapped_display_name = display_device::map_output_name(*allocation_result.result.device_id);
          if (!remapped_display_name.empty() && remapped_display_name != allocated_display_name) {
            BOOST_LOG(info) << "SunshineSeat SudoVDA provider remapped selected display after stream topology transaction from "sv
                            << allocated_display_name << " to "sv << remapped_display_name;
            allocated_display_name = remapped_display_name;
          }

          display_names = sudovda::capturable_display_names(false);
          if (!allocated_display_name.empty() && !contains_display_name(display_names, allocated_display_name)) {
            display_names.emplace_back(allocated_display_name);
          }
        } else {
          BOOST_LOG(warning) << "SunshineSeat SudoVDA provider could not resolve a device id for the allocated virtual display."sv;
        }

        {
          std::lock_guard lock {sudovda_cached_allocation_mutex};
          if (sudovda_cached_allocation && sudovda_cached_allocation->allocation) {
            BOOST_LOG(warning) << "SunshineSeat SudoVDA provider replacing unexpected cached allocation: "sv
                               << sudovda_cached_allocation->display_name;
          }
          sudovda_cached_allocation = std::make_shared<sudovda_cached_allocation_t>(sudovda_cached_allocation_t {
            request,
            std::move(allocation_result.allocation),
            allocation_result.result.device_id.value_or(std::string {}),
            allocated_display_name
          });
        }

        BOOST_LOG(info) << "SunshineSeat SudoVDA provider retained stream virtual display allocation: "sv << allocated_display_name;
        return display_names;
      }
    }


    class sudovda_display_provider_t: public display_provider_t {
    public:
      std::string_view id() const override {
        return "sudovda"sv;
      }

      std::shared_ptr<display_t> display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) override {
        if (video::requires_dual_display_compositor(config)) {
          const bool pair_valid = config.dual_display_pair &&
                                  !dual_display_launch::validate_prepared_pair(*config.dual_display_pair);
          switch (dual_display::select_capture_route(true, hwdevice_type == mem_type_e::dxgi, pair_valid)) {
            case dual_display::capture_route_e::paired_d3d11:
              {
                auto paired_display = std::make_shared<dxgi::display_ddup_pair_vram_t>();
                if (!paired_display->init(config, *config.dual_display_pair)) {
                  return paired_display;
                }
                BOOST_LOG(error) << "dual_display_pair_capture_initialization_failed: refusing single-display fallback"sv;
                return nullptr;
              }
            case dual_display::capture_route_e::rejected:
              BOOST_LOG(error) << "dual_display_pair_capture_unsupported: prepared pairs require validated D3D11 capture"sv;
              return nullptr;
            case dual_display::capture_route_e::ordinary:
              break;
          }
        }
        if (!has_sudovda_stream_identity(config)) {
          BOOST_LOG(debug) << "SunshineSeat SudoVDA provider using desktop capture for startup encoder validation; virtual display allocation requires a client identity."sv;
          return desktop_display_provider_t {}.display(hwdevice_type, display_name, config);
        }

        const auto request = make_sudovda_stream_request(config);
        auto virtual_display_names = ensure_sudovda_allocation(request, config.display_initialization_abort);

        if (virtual_display_names.empty()) {
          return nullptr;
        }

        std::string selected_display_name;
        if (display_name.empty()) {
          selected_display_name = virtual_display_names.front();
        } else if (std::find(std::begin(virtual_display_names), std::end(virtual_display_names), display_name) != std::end(virtual_display_names)) {
          selected_display_name = display_name;
        } else {
          BOOST_LOG(error) << "Requested display is not a SudoVDA output; refusing to capture physical console display: "sv << display_name;
          return nullptr;
        }

        auto capturable_virtual_display_names = sudovda::capturable_display_names(false);
        if (!contains_display_name(capturable_virtual_display_names, selected_display_name)) {
          BOOST_LOG(info) << "SunshineSeat SudoVDA provider waiting for selected display to become capturable: "sv << selected_display_name;
          if (!wait_for_stable_sudovda_display(
                selected_display_name,
                capturable_virtual_display_names,
                std::chrono::seconds(3),
                config.display_initialization_abort
              )) {
            // DuplicateOutput() enumeration checks can transiently fail during virtual-display
            // reinitialization even though opening the selected output directly succeeds a
            // moment later. Treat the timeout as a soft signal and fall through to the normal
            // display init path so reset_display() can perform the real retry loop.
            BOOST_LOG(warning) << "SunshineSeat SudoVDA selected display did not become capturable before timeout; attempting direct init anyway: "sv
                               << selected_display_name;
          }
        }

        BOOST_LOG(info) << "SunshineSeat SudoVDA provider selected display: "sv << selected_display_name;
        auto display = desktop_display_provider_t {}.display(hwdevice_type, selected_display_name, config);
        if (!display) {
          // Capture initialization can race DXGI/SudoVDA reenumeration. The
          // stream lease remains authoritative until disconnect; releasing it
          // here would trigger another topology hotplug and amplify a
          // recoverable AMF stall into a visible reconnect loop.
          BOOST_LOG(error) << "SunshineSeat SudoVDA provider failed to initialize capture for selected display; keeping allocation for retry: "sv
                           << selected_display_name;
          return nullptr;
        }

        if (auto allocation_ref = get_sudovda_cached_allocation_reference(selected_display_name)) {
          auto owner = std::make_shared<sudovda_display_owner_t>(sudovda_display_owner_t {
            std::move(allocation_ref),
            std::move(display)
          });
          return std::shared_ptr<display_t>(owner, owner->display.get());
        }

        return display;
      }

      std::vector<std::string> display_names(mem_type_e) override {
        return sudovda::capturable_display_names(false);
      }

    };

    class unsupported_display_provider_t: public display_provider_t {
    public:
      explicit unsupported_display_provider_t(seat::profile_t profile):
          profile {std::move(profile)} {
      }

      std::string_view id() const override {
        return profile.display_provider;
      }

      std::shared_ptr<display_t> display(mem_type_e, const std::string &, const video::config_t &) override {
        BOOST_LOG(error) << "SunshineSeat display provider is not implemented yet: "sv << seat::describe(profile);
        return nullptr;
      }

      std::vector<std::string> display_names(mem_type_e) override {
        BOOST_LOG(error) << "SunshineSeat display provider cannot enumerate displays yet: "sv << seat::describe(profile);
        return {};
      }

    private:
      seat::profile_t profile;
    };

    std::unique_ptr<display_provider_t> make_display_provider(const seat::profile_t &profile) {
      if (profile.display_provider == "desktop") {
        return std::make_unique<desktop_display_provider_t>();
      }

      if (profile.display_provider == "sudovda") {
        return std::make_unique<sudovda_display_provider_t>();
      }

      return std::make_unique<unsupported_display_provider_t>(profile);
    }
  }  // namespace

  /**
   * Pick a display adapter and capture method.
   * @param hwdevice_type enables possible use of hardware encoder
   */
  std::shared_ptr<display_t> display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    const auto profile = seat::active_profile();
    const auto provider = make_display_provider(profile);
    BOOST_LOG(debug) << "SunshineSeat display provider selected for capture: "sv << provider->id();

    return provider->display(hwdevice_type, display_name, config);
  }

  std::vector<std::string> display_names(mem_type_e hwdevice_type) {
    const auto profile = seat::active_profile();
    const auto provider = make_display_provider(profile);
    BOOST_LOG(debug) << "SunshineSeat display provider selected for enumeration: "sv << provider->id();

    return provider->display_names(hwdevice_type);
  }

  dual_display_launch::prepared_pair_ptr prepare_dual_virtual_display_pair(
    const dual_display_launch::request_t &request,
    const std::string_view authenticated_client_identity,
    std::string &failure
  ) {
    const auto profile = seat::active_profile();
    if (profile.display_provider != "sudovda") {
      failure = "dual_display_provider_unsupported";
      return {};
    }

    const auto lease_expires_utc = dual_display_handshake_expiry_utc();
    if (lease_expires_utc.empty()) {
      failure = "dual_display_handshake_expiry_unavailable";
      return {};
    }
    return sudovda::dual::prepare_sudovda_pair(
      request,
      authenticated_client_identity,
      lease_expires_utc,
      failure
    );
  }

  bool prepare_virtual_display_for_stream(const video::config_t &config) {
    const auto profile = seat::active_profile();
    if (video::requires_dual_display_compositor(config)) {
      if (profile.display_provider != "sudovda") {
        BOOST_LOG(error) << "dual_display_capture_provider_unsupported: refusing prepared pair before transport startup"sv;
        return false;
      }

      std::string failure;
      if (!sudovda::dual::prepared_pair_is_capturable(*config.dual_display_pair, failure)) {
        BOOST_LOG(error) << "dual_display_pair_not_capturable: refusing prepared pair before transport startup [reason="sv
                         << failure << ']';
        return false;
      }

      BOOST_LOG(info) << "dual_display_pair_capture_ready: retaining exact prepared pair for D3D11 composite capture"sv;
      return true;
    }
    if (profile.display_provider != "sudovda" || !has_sudovda_stream_identity(config)) {
      return true;
    }

    const auto request = make_sudovda_stream_request(config);
    BOOST_LOG(info) << "SunshineSeat SudoVDA preflight started before stream transport [client="sv
                    << (!config.client_name.empty() ? config.client_name : config.client_uuid) << ']';
    const auto prepared_displays = ensure_sudovda_allocation(request);
    if (prepared_displays.empty()) {
      BOOST_LOG(error) << "SunshineSeat SudoVDA preflight failed; refusing to start stream transport before display is ready"sv;
      return false;
    }

    BOOST_LOG(info) << "SunshineSeat SudoVDA preflight complete before stream transport [display="sv
                    << prepared_displays.front() << ']';
    return true;
  }

  void release_virtual_display_allocation_if_idle() {
    const auto profile = seat::active_profile();
    std::lock_guard worker_lock {sudovda_release_worker_mutex};
    if (!display_reliability::should_retain_sudovda_display_for_reconnect(
          config::sunshineseat.mode == "sidecar",
          profile.display_provider
        )) {
      std::shared_ptr<sudovda_cached_allocation_t> released_allocation;
      {
        std::lock_guard allocation_lock {sudovda_cached_allocation_mutex};
        released_allocation = std::move(sudovda_cached_allocation);
        sudovda_cached_allocation.reset();
      }
      if (released_allocation) {
        BOOST_LOG(info) << "SunshineSeat SudoVDA releasing stream-managed virtual display immediately after last session: "sv
                        << released_allocation->display_name;
        remove_sudovda_allocation(released_allocation, "last_session_cleanup"sv);
      }
      return;
    }

    {
      std::lock_guard allocation_lock {sudovda_cached_allocation_mutex};
      if (!sudovda::should_schedule_virtual_display_release(
            static_cast<bool>(sudovda_cached_allocation),
            sudovda_release_worker.joinable()
          )) {
        return;
      }
      BOOST_LOG(info) << "SunshineSeat SudoVDA provider retaining cached virtual display for reconnect grace [display="sv
                      << sudovda_cached_allocation->display_name
                      << ", grace_seconds="sv << std::chrono::duration_cast<std::chrono::seconds>(sudovda_reconnect_grace_duration).count() << ']';
    }

    sudovda_release_worker = std::jthread([](const std::stop_token stop_token) {
      std::mutex wait_mutex;
      std::condition_variable_any wait_condition;
      std::unique_lock wait_lock {wait_mutex};
      wait_condition.wait_for(wait_lock, stop_token, sudovda_reconnect_grace_duration, []() {
        return false;
      });
      wait_lock.unlock();
      if (stop_token.stop_requested() ||
          !sudovda::should_release_virtual_display_lease(false, sudovda_reconnect_grace_duration, sudovda_reconnect_grace_duration)) {
        return;
      }

      std::shared_ptr<sudovda_cached_allocation_t> released_allocation;
      {
        std::lock_guard allocation_lock {sudovda_cached_allocation_mutex};
        released_allocation = std::move(sudovda_cached_allocation);
        sudovda_cached_allocation.reset();
      }
      if (released_allocation) {
        BOOST_LOG(info) << "SunshineSeat SudoVDA reconnect grace expired; releasing cached virtual display: "sv
                        << released_allocation->display_name;
        remove_sudovda_allocation(released_allocation, "reconnect_grace_expired"sv);
      }
    });
  }

  /**
   * @brief Returns if GPUs/drivers have changed since the last call to this function.
   * @return `true` if a change has occurred or if it is unknown whether a change occurred.
   */
  bool needs_encoder_reenumeration() {
    // Serialize access to the static DXGI factory
    static std::mutex reenumeration_state_lock;
    auto lg = std::lock_guard(reenumeration_state_lock);

    // Keep a reference to the DXGI factory, which will keep track of changes internally.
    static dxgi::factory1_t factory;
    if (!factory || !factory->IsCurrent()) {
      const bool first_dxgi_observation = !factory;
      factory.reset();

      auto status = CreateDXGIFactory1(IID_IDXGIFactory1, (void **) &factory);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create DXGIFactory1 [0x"sv << util::hex(status).to_string_view() << ']';
        factory.release();
      }

      if (display_reliability::should_defer_initial_sudovda_encoder_reenumeration(
            config::sunshineseat.mode == "sidecar",
            config::seat.display_provider,
            first_dxgi_observation,
            SUCCEEDED(status)
          )) {
        BOOST_LOG(info) << "SunshineSeat deferring initial encoder reenumeration until the stream-managed SudoVDA display is ready"sv;
        return false;
      }

      // Always request reenumeration on the first streaming session just to ensure we
      // can deal with any initialization races that may occur when the system is booting.
      BOOST_LOG(info) << "Encoder reenumeration is required"sv;
      return true;
    } else {
      // The DXGI factory from last time is still current, so no encoder changes have occurred.
      return false;
    }
  }
}  // namespace platf
