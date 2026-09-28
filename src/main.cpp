/**
 * @file src/main.cpp
 * @brief Definitions for the main entry point for Sunshine.
 */
// standard includes
#include <charconv>
#include <atomic>
#include <chrono>
#include <codecvt>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>

#ifdef __APPLE__
  #include <mach-o/dyld.h>
#endif

// local includes
#include "client_mic.h"
#include "confighttp.h"
#include "display_device.h"
#include "entry_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "main.h"
#include "nvhttp.h"
#include "process.h"
#include "seat.h"
#include "system_tray.h"
#include "upnp.h"
#include "video.h"

#ifdef _WIN32
  #include "platform/windows/admin_broker.h"
  #include "platform/windows/sudovda_control.h"
#endif

extern "C" {
#include "rswrapper.h"
}

using namespace std::literals;

std::map<int, std::function<void()>> signal_handlers;
std::atomic_bool sudovda_holder_running {true};

void on_signal_forwarder(int sig) {
  signal_handlers.at(sig)();
}

template<class FN>
void on_signal(int sig, FN &&fn) {
  signal_handlers.emplace(sig, std::forward<FN>(fn));

  std::signal(sig, on_signal_forwarder);
}

std::map<std::string_view, std::function<int(const char *name, int argc, char **argv)>> cmd_to_func {
  {"creds"sv, [](const char *name, int argc, char **argv) {
     return args::creds(name, argc, argv);
   }},
  {"help"sv, [](const char *name, int argc, char **argv) {
     return args::help(name);
   }},
  {"version"sv, [](const char *name, int argc, char **argv) {
     return args::version();
   }},
#ifdef _WIN32
  {"restore-nvprefs-undo"sv, [](const char *name, int argc, char **argv) {
     return args::restore_nvprefs_undo();
   }},
  {"admin-broker-list"sv, [](const char *name, int argc, char **argv) {
     if (argc != 0) {
       std::cout << "Usage: "sv << name << " --admin-broker-list"sv << std::endl;
       return 2;
     }

     platf::admin_broker::print_task_list(std::cout);
     return 0;
   }},
  {"admin-broker-plan"sv, [](const char *name, int argc, char **argv) {
     if (argc != 1) {
       std::cout << "Usage: "sv << name << " --admin-broker-plan task_id"sv << std::endl;
       return 2;
     }

     const auto plan = platf::admin_broker::plan_request(argv[0]);
     platf::admin_broker::print_plan(std::cout, plan);
     return plan.blockers.empty() ? 0 : 1;
   }},
  {"admin-broker-uac-status"sv, [](const char *name, int argc, char **argv) {
     if (argc != 0) {
       std::cout << "Usage: "sv << name << " --admin-broker-uac-status"sv << std::endl;
       return 2;
     }

     const auto policy = platf::admin_broker::query_uac_policy();
     platf::admin_broker::print_uac_policy(std::cout, policy);
     return policy.blockers.empty() ? 0 : 1;
   }},
  {"admin-broker-current-sid"sv, [](const char *name, int argc, char **argv) {
     if (argc != 0) {
       std::cout << "Usage: "sv << name << " --admin-broker-current-sid"sv << std::endl;
       return 2;
     }

     const auto sid = platf::admin_broker::current_process_user_sid();
     if (sid.empty()) {
       std::cout << "Unable to resolve current process user SID."sv << std::endl;
       return 1;
     }

     std::cout << sid << std::endl;
     return 0;
   }},
  {"admin-broker-serve-once"sv, [](const char *name, int argc, char **argv) {
     std::string required_caller_sid;
     if (argc == 2 && argv[0] == "--require-caller-sid"sv) {
       required_caller_sid = argv[1];
     } else if (argc != 0) {
       std::cout << "Usage: "sv << name << " --admin-broker-serve-once [--require-caller-sid SID]"sv << std::endl;
       return 2;
     }

     const auto result = platf::admin_broker::serve_once(required_caller_sid);
     platf::admin_broker::print_broker_exchange_result(std::cout, result);
     return result.exit_code;
   }},
  {"admin-broker-call"sv, [](const char *name, int argc, char **argv) {
     if (argc < 1) {
       std::cout << "Usage: "sv << name << " --admin-broker-call task_id [--dry-run] --confirm-maintenance [--confirm-secure-desktop-change]"sv << std::endl;
       return 2;
     }

     platf::admin_broker::broker_call_request_t request {
       {
         argv[0],
         false,
         false,
         false,
       },
     };

     for (int i = 1; i < argc; ++i) {
       const std::string_view option {argv[i]};
       if (option == "--dry-run"sv) {
         request.execution.dry_run = true;
       } else if (option == "--confirm-maintenance"sv) {
         request.execution.confirmed = true;
       } else if (option == "--confirm-secure-desktop-change"sv) {
         request.execution.secure_desktop_confirmed = true;
       } else {
         std::cout << "Invalid option: "sv << option << std::endl;
         std::cout << "Usage: "sv << name << " --admin-broker-call task_id [--dry-run] --confirm-maintenance [--confirm-secure-desktop-change]"sv << std::endl;
         return 2;
       }
     }

     const auto result = platf::admin_broker::call_broker(request);
     platf::admin_broker::print_broker_exchange_result(std::cout, result);
     return result.exit_code;
   }},
  {"admin-broker-execute"sv, [](const char *name, int argc, char **argv) {
     if (argc < 1) {
       std::cout << "Usage: "sv << name << " --admin-broker-execute task_id [--dry-run] --confirm-maintenance [--confirm-secure-desktop-change]"sv << std::endl;
       return 2;
     }

     platf::admin_broker::execution_request_t request {
       argv[0],
       false,
       false,
       false,
     };

     for (int i = 1; i < argc; ++i) {
       const std::string_view option {argv[i]};
       if (option == "--dry-run"sv) {
         request.dry_run = true;
       } else if (option == "--confirm-maintenance"sv) {
         request.confirmed = true;
       } else if (option == "--confirm-secure-desktop-change"sv) {
         request.secure_desktop_confirmed = true;
       } else {
         std::cout << "Invalid option: "sv << option << std::endl;
         std::cout << "Usage: "sv << name << " --admin-broker-execute task_id [--dry-run] --confirm-maintenance [--confirm-secure-desktop-change]"sv << std::endl;
         return 2;
       }
     }

     const auto result = platf::admin_broker::execute_request(request);
     platf::admin_broker::print_execution_result(std::cout, result);
     return result.exit_code;
   }},
  {"sudovda-status"sv, [](const char *name, int argc, char **argv) {
     platf::sudovda::print_status(std::cout, platf::sudovda::query_status(true));
     return 0;
   }},
  {"sudovda-dry-run-create"sv, [](const char *name, int argc, char **argv) {
     if (argc != 3) {
       std::cout << "Usage: "sv << name << " --sudovda-dry-run-create width height refresh_rate"sv << std::endl;
       return 2;
     }

     auto parse_uint = [](const char *raw, UINT &out) {
       unsigned long parsed = 0;
       const std::string_view input {raw};
       const auto result = std::from_chars(input.data(), input.data() + input.size(), parsed);
       if (result.ec != std::errc {} || result.ptr != input.data() + input.size() || parsed > std::numeric_limits<UINT>::max() || parsed == 0) {
         return false;
       }

       out = static_cast<UINT>(parsed);
       return true;
     };

     UINT width = 0;
     UINT height = 0;
     UINT refresh_rate = 0;
     if (!parse_uint(argv[0], width) || !parse_uint(argv[1], height) || !parse_uint(argv[2], refresh_rate)) {
       std::cout << "Invalid dimensions. Expected positive integer width, height, and refresh_rate."sv << std::endl;
       return 2;
     }

     const auto request = platf::sudovda::make_default_request(width, height, refresh_rate);
     platf::sudovda::print_plan(std::cout, platf::sudovda::plan_create_virtual_display(request, platf::sudovda::lifecycle_mode_e::dry_run));
     return 0;
   }},
  {"sudovda-test-create"sv, [](const char *name, int argc, char **argv) {
     if (argc != 3 && argc != 4) {
       std::cout << "Usage: "sv << name << " --sudovda-test-create width height refresh_rate [--allow-service-session]"sv << std::endl;
       return 2;
     }

     const bool allow_service_session = argc == 4 && argv[3] == "--allow-service-session"sv;
     if (argc == 4 && !allow_service_session) {
       std::cout << "Invalid option. Expected --allow-service-session."sv << std::endl;
       return 2;
     }

     auto parse_uint = [](const char *raw, UINT &out) {
       unsigned long parsed = 0;
       const std::string_view input {raw};
       const auto result = std::from_chars(input.data(), input.data() + input.size(), parsed);
       if (result.ec != std::errc {} || result.ptr != input.data() + input.size() || parsed > std::numeric_limits<UINT>::max() || parsed == 0) {
         return false;
       }

       out = static_cast<UINT>(parsed);
       return true;
     };

     UINT width = 0;
     UINT height = 0;
     UINT refresh_rate = 0;
     if (!parse_uint(argv[0], width) || !parse_uint(argv[1], height) || !parse_uint(argv[2], refresh_rate)) {
       std::cout << "Invalid dimensions. Expected positive integer width, height, and refresh_rate."sv << std::endl;
       return 2;
     }

     const auto request = platf::sudovda::make_default_request(width, height, refresh_rate);
     const auto result = platf::sudovda::test_create_virtual_display(request, allow_service_session);
     platf::sudovda::print_execution_result(std::cout, result);
     return result.plan.blockers.empty() && result.created && result.provider_capturable && result.removed ? 0 : 1;
   }},
  {"sudovda-hold-default"sv, [](const char *name, int argc, char **argv) {
     if (argc != 3 && argc != 4 && argc != 5) {
       std::cout << "Usage: "sv << name << " --sudovda-hold-default width height refresh_rate [ready_file] [--allow-service-session]"sv << std::endl;
       return 2;
     }

     const bool allow_service_session = argc >= 4 && argv[argc - 1] == "--allow-service-session"sv;
     const int positional_argc = allow_service_session ? argc - 1 : argc;
     if (positional_argc != 3 && positional_argc != 4) {
       std::cout << "Invalid option. Expected optional ready_file followed by optional --allow-service-session."sv << std::endl;
       return 2;
     }

     auto parse_uint = [](const char *raw, UINT &out) {
       unsigned long parsed = 0;
       const std::string_view input {raw};
       const auto result = std::from_chars(input.data(), input.data() + input.size(), parsed);
       if (result.ec != std::errc {} || result.ptr != input.data() + input.size() || parsed > std::numeric_limits<UINT>::max() || parsed == 0) {
         return false;
       }

       out = static_cast<UINT>(parsed);
       return true;
     };

     UINT width = 0;
     UINT height = 0;
     UINT refresh_rate = 0;
     if (!parse_uint(argv[0], width) || !parse_uint(argv[1], height) || !parse_uint(argv[2], refresh_rate)) {
       std::cout << "Invalid dimensions. Expected positive integer width, height, and refresh_rate."sv << std::endl;
       return 2;
     }

     const auto request = platf::sudovda::make_default_request(width, height, refresh_rate);
     auto allocation_result = platf::sudovda::create_virtual_display_allocation(request, allow_service_session);
     platf::sudovda::print_execution_result(std::cout, allocation_result.result);
     if (!allocation_result.allocation || !allocation_result.result.plan.blockers.empty() || !allocation_result.result.display_name) {
       return 1;
     }

     const std::string display_name = *allocation_result.result.display_name;
     if (positional_argc == 4) {
       std::ofstream ready_file {argv[3], std::ios::trunc};
       ready_file << display_name << '\n';
     }

     sudovda_holder_running = true;
     on_signal(SIGINT, []() {
       sudovda_holder_running = false;
     });
     on_signal(SIGTERM, []() {
       sudovda_holder_running = false;
     });

     std::cout << "SudoVDA holder active for " << display_name << ". Stop this process to remove the virtual display." << std::endl;
     while (sudovda_holder_running) {
       std::this_thread::sleep_for(std::chrono::seconds(1));
     }

     std::cout << "SudoVDA holder stopping for " << display_name << "." << std::endl;
     allocation_result.allocation.reset();
     return 0;
   }},
  {"sudovda-remove-default"sv, [](const char *name, int argc, char **argv) {
     if (argc != 0 && argc != 1) {
       std::cout << "Usage: "sv << name << " --sudovda-remove-default [--allow-service-session]"sv << std::endl;
       return 2;
     }

     const bool allow_service_session = argc == 1 && argv[0] == "--allow-service-session"sv;
     if (argc == 1 && !allow_service_session) {
       std::cout << "Invalid option. Expected --allow-service-session."sv << std::endl;
       return 2;
     }

     const auto request = platf::sudovda::make_default_request(1920, 1080, 60);
     const auto result = platf::sudovda::remove_virtual_display_allocation(request, allow_service_session);
     platf::sudovda::print_execution_result(std::cout, result);
     return result.plan.blockers.empty() && result.removed ? 0 : 1;
   }},
  {"sudovda-test-provider-allocation"sv, [](const char *name, int argc, char **argv) {
     if (argc != 3) {
       std::cout << "Usage: "sv << name << " --sudovda-test-provider-allocation width height refresh_rate"sv << std::endl;
       return 2;
     }

     auto parse_uint = [](const char *raw, UINT &out) {
       unsigned long parsed = 0;
       const std::string_view input {raw};
       const auto result = std::from_chars(input.data(), input.data() + input.size(), parsed);
       if (result.ec != std::errc {} || result.ptr != input.data() + input.size() || parsed > std::numeric_limits<UINT>::max() || parsed == 0) {
         return false;
       }

       out = static_cast<UINT>(parsed);
       return true;
     };

     UINT width = 0;
     UINT height = 0;
     UINT refresh_rate = 0;
     if (!parse_uint(argv[0], width) || !parse_uint(argv[1], height) || !parse_uint(argv[2], refresh_rate)) {
       std::cout << "Invalid dimensions. Expected positive integer width, height, and refresh_rate."sv << std::endl;
       return 2;
     }

     config::seat.display_provider = "sudovda";
     video::config_t video_config {};
     video_config.width = static_cast<int>(width);
     video_config.height = static_cast<int>(height);
     video_config.framerate = static_cast<int>(refresh_rate);
     video_config.framerateX100 = static_cast<int>(refresh_rate * 100);
     video_config.client_name = "SudoVDA Provider Test";
     video_config.client_uuid = "sunshineseat-sudovda-provider-test";

     std::cout << "SudoVDA provider allocation test\n";
     auto display = platf::display(platf::mem_type_e::system, {}, video_config);
     if (!display) {
       std::cout << "  display_constructed: no\n";
       platf::release_virtual_display_allocation_if_idle();
       return 1;
     }

     std::cout << "  display_constructed: yes\n";
     std::cout << "  display_size: " << display->width << 'x' << display->height << '\n';

     display.reset();
     platf::release_virtual_display_allocation_if_idle();
     std::cout << "  display_released: yes\n";
     return 0;
   }},
  {"sudovda-test-provider-allocation-for-client"sv, [](const char *name, int argc, char **argv) {
     if (argc != 4 && argc != 5) {
       std::cout << "Usage: "sv << name << " --sudovda-test-provider-allocation-for-client width height refresh_rate client_uuid [client_name]"sv << std::endl;
       return 2;
     }

     auto parse_uint = [](const char *raw, UINT &out) {
       unsigned long parsed = 0;
       const std::string_view input {raw};
       const auto result = std::from_chars(input.data(), input.data() + input.size(), parsed);
       if (result.ec != std::errc {} || result.ptr != input.data() + input.size() || parsed > std::numeric_limits<UINT>::max() || parsed == 0) {
         return false;
       }

       out = static_cast<UINT>(parsed);
       return true;
     };

     UINT width = 0;
     UINT height = 0;
     UINT refresh_rate = 0;
     if (!parse_uint(argv[0], width) || !parse_uint(argv[1], height) || !parse_uint(argv[2], refresh_rate)) {
       std::cout << "Invalid dimensions. Expected positive integer width, height, and refresh_rate."sv << std::endl;
       return 2;
     }

     const std::string client_uuid = argv[3];
     if (client_uuid.empty()) {
       std::cout << "Invalid client_uuid. Expected a non-empty identity string."sv << std::endl;
       return 2;
     }

     const std::string client_name = argc == 5 ? argv[4] : client_uuid;
     const auto request = platf::sudovda::make_persistent_request(width, height, refresh_rate, client_uuid);

     config::seat.display_provider = "sudovda";
     video::config_t video_config {};
     video_config.width = static_cast<int>(width);
     video_config.height = static_cast<int>(height);
     video_config.framerate = static_cast<int>(refresh_rate);
     video_config.framerateX100 = static_cast<int>(refresh_rate * 100);
     video_config.client_name = client_name;
     video_config.client_uuid = client_uuid;

     std::cout << "SudoVDA provider allocation test for explicit client identity\n";
     std::cout << "  client_uuid: " << client_uuid << '\n';
     std::cout << "  client_name: " << client_name << '\n';
     std::cout << "  derived_serial: " << request.serial_number << '\n';
     auto display = platf::display(platf::mem_type_e::system, {}, video_config);
     if (!display) {
       std::cout << "  display_constructed: no\n";
       platf::release_virtual_display_allocation_if_idle();
       return 1;
     }

     std::cout << "  display_constructed: yes\n";
     std::cout << "  display_size: " << display->width << 'x' << display->height << '\n';

     display.reset();
     platf::release_virtual_display_allocation_if_idle();
     std::cout << "  display_released: yes\n";
     return 0;
   }},
#endif
};

#ifdef _WIN32
LRESULT CALLBACK SessionMonitorWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    case WM_ENDSESSION:
      {
        // Terminate ourselves with a blocking exit call
        std::cout << "Received WM_ENDSESSION"sv << std::endl;
        lifetime::exit_sunshine(0, false);
        return 0;
      }
    default:
      return DefWindowProc(hwnd, uMsg, wParam, lParam);
  }
}

WINAPI BOOL ConsoleCtrlHandler(DWORD type) {
  if (type == CTRL_CLOSE_EVENT) {
    BOOST_LOG(info) << "Console closed handler called";
    lifetime::exit_sunshine(0, false);
  }
  return FALSE;
}
#endif

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
constexpr bool tray_is_enabled = true;
#else
constexpr bool tray_is_enabled = false;
#endif

void mainThreadLoop(const std::shared_ptr<safe::event_t<bool>> &shutdown_event) {
  bool run_loop = false;

  // Conditions that would require the main thread event loop
#ifndef _WIN32
  run_loop = tray_is_enabled && config::sunshine.system_tray;  // On Windows, tray runs in separate thread, so no main loop needed for tray
#endif

  if (!run_loop) {
    BOOST_LOG(info) << "No main thread features enabled, skipping event loop"sv;
    // Wait for shutdown
    shutdown_event->view();
    return;
  }

  // Main thread event loop
  BOOST_LOG(info) << "Starting main loop"sv;
  while (system_tray::process_tray_events() == 0);
  BOOST_LOG(info) << "Main loop has exited"sv;
}

int main(int argc, char *argv[]) {
#ifdef __APPLE__
  // Bundle assets are referenced relative to the executable
  // (e.g. ../Resources/assets), so anchor cwd to Contents/MacOS.
  {
    char executable[2048];
    uint32_t size = sizeof(executable);
    if (_NSGetExecutablePath(executable, &size) == 0) {
      std::error_code ec;
      auto exec_dir = std::filesystem::weakly_canonical(std::filesystem::path {executable}, ec).parent_path();
      if (!ec) {
        std::filesystem::current_path(exec_dir, ec);
      }
      if (ec) {
        std::cerr << "Failed to set working directory to executable path: " << ec.message() << '\n';
      }
    }
  }
#endif

  lifetime::argv = argv;

  task_pool_util::TaskPool::task_id_t force_shutdown = nullptr;

#ifdef _WIN32
  // Avoid searching the PATH in case a user has configured their system insecurely
  // by placing a user-writable directory in the system-wide PATH variable.
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);

  setlocale(LC_ALL, "C");
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  // Use UTF-8 conversion for the default C++ locale (used by boost::log)
  std::locale::global(std::locale(std::locale(), new std::codecvt_utf8<wchar_t>));
#pragma GCC diagnostic pop

  mail::man = std::make_shared<safe::mail_raw_t>();

  // parse config file
  if (config::parse(argc, argv)) {
    return 0;
  }

  auto log_deinit_guard = logging::init(config::sunshine.min_log_level, config::sunshine.log_file);
  if (!log_deinit_guard) {
    BOOST_LOG(error) << "Logging failed to initialize"sv;
  }

  // logging can begin at this point
  // if anything is logged prior to this point, it will appear in stdout, but not in the log viewer in the UI
  // the version should be printed to the log before anything else
  BOOST_LOG(info) << PROJECT_NAME << " version: " << PROJECT_VERSION << " commit: " << PROJECT_VERSION_COMMIT;

  // Log publisher metadata
  log_publisher_data();

  // Log modified_config_settings
  config::log_config_settings(config::modified_config_settings, false);
  config::modified_config_settings.clear();

  seat::log_startup_profile();

#ifdef _WIN32
  if (config::sunshineseat.client_mic_enabled) {
    client_mic::repair_stale_default_capture(config::sunshineseat.client_mic_sink);
    client_mic::set_preferred_idle_capture(config::sunshineseat.client_mic_sink);
  }
#endif

  if (!config::sunshine.cmd.name.empty()) {
    auto fn = cmd_to_func.find(config::sunshine.cmd.name);
    if (fn == std::end(cmd_to_func)) {
      BOOST_LOG(fatal) << "Unknown command: "sv << config::sunshine.cmd.name;

      BOOST_LOG(info) << "Possible commands:"sv;
      for (auto &[key, _] : cmd_to_func) {
        BOOST_LOG(info) << '\t' << key;
      }

      return 7;
    }

    return fn->second(argv[0], config::sunshine.cmd.argc, config::sunshine.cmd.argv);
  }

  // Adding guard here first as it also performs recovery after crash,
  // otherwise people could theoretically end up without display output.
  // It also should be destroyed before forced shutdown to expedite the cleanup.
  auto display_device_deinit_guard = display_device::init(platf::appdata() / "display_device.state", config::video);
  if (!display_device_deinit_guard) {
    BOOST_LOG(error) << "Display device session failed to initialize"sv;
  }

#ifdef _WIN32
  // Modify relevant NVIDIA control panel settings if the system has corresponding gpu
  if (nvprefs_instance.load()) {
    // Restore global settings to the undo file left by improper termination of sunshine.exe
    nvprefs_instance.restore_from_and_delete_undo_file_if_exists();
    // Modify application settings for sunshine.exe
    nvprefs_instance.modify_application_profile();
    // Modify global settings, undo file is produced in the process to restore after improper termination
    nvprefs_instance.modify_global_profile();
    // Unload dynamic library to survive driver re-installation
    nvprefs_instance.unload();
  }

  // Wait as long as possible to terminate Sunshine.exe during logoff/shutdown
  SetProcessShutdownParameters(0x100, SHUTDOWN_NORETRY);

  // We must create a hidden window to receive shutdown notifications since we load gdi32.dll
  std::promise<HWND> session_monitor_hwnd_promise;
  auto session_monitor_hwnd_future = session_monitor_hwnd_promise.get_future();
  std::promise<void> session_monitor_join_thread_promise;
  auto session_monitor_join_thread_future = session_monitor_join_thread_promise.get_future();

  std::thread session_monitor_thread([&]() {
    platf::set_thread_name("session_monitor");
    session_monitor_join_thread_promise.set_value_at_thread_exit();

    WNDCLASSA wnd_class {};
    wnd_class.lpszClassName = "SunshineSessionMonitorClass";
    wnd_class.lpfnWndProc = SessionMonitorWindowProc;
    if (!RegisterClassA(&wnd_class)) {
      session_monitor_hwnd_promise.set_value(nullptr);
      BOOST_LOG(error) << "Failed to register session monitor window class"sv << std::endl;
      return;
    }

    auto wnd = CreateWindowExA(
      0,
      wnd_class.lpszClassName,
      "Sunshine Session Monitor Window",
      0,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      nullptr,
      nullptr,
      nullptr,
      nullptr
    );

    session_monitor_hwnd_promise.set_value(wnd);

    if (!wnd) {
      BOOST_LOG(error) << "Failed to create session monitor window"sv << std::endl;
      return;
    }

    ShowWindow(wnd, SW_HIDE);

    // Run the message loop for our window
    MSG msg {};
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  });

  auto session_monitor_join_thread_guard = util::fail_guard([&]() {
    if (session_monitor_hwnd_future.wait_for(1s) == std::future_status::ready) {
      if (HWND session_monitor_hwnd = session_monitor_hwnd_future.get()) {
        PostMessage(session_monitor_hwnd, WM_CLOSE, 0, 0);
      }

      if (session_monitor_join_thread_future.wait_for(1s) == std::future_status::ready) {
        session_monitor_thread.join();
        return;
      } else {
        BOOST_LOG(warning) << "session_monitor_join_thread_future reached timeout";
      }
    } else {
      BOOST_LOG(warning) << "session_monitor_hwnd_future reached timeout";
    }

    session_monitor_thread.detach();
  });

#endif

  task_pool.start(1);

  // Create signal handler after logging has been initialized
  auto shutdown_event = mail::man->event<bool>(mail::shutdown);
  on_signal(SIGINT, [&force_shutdown, &display_device_deinit_guard, shutdown_event]() {
    BOOST_LOG(info) << "Interrupt handler called"sv;

    auto task = []() {
      BOOST_LOG(fatal) << "10 seconds passed, yet Sunshine's still running: Forcing shutdown"sv;
      logging::log_flush();
      std::quick_exit(70);
    };
    force_shutdown = task_pool.pushDelayed(task, 10s).task_id;

    // Break out of the main loop
    shutdown_event->raise(true);

    if (tray_is_enabled && config::sunshine.system_tray) {
      system_tray::end_tray();
    }

    display_device_deinit_guard = nullptr;
  });

  on_signal(SIGTERM, [&force_shutdown, &display_device_deinit_guard, shutdown_event]() {
    BOOST_LOG(info) << "Terminate handler called"sv;

    auto task = []() {
      BOOST_LOG(fatal) << "10 seconds passed, yet Sunshine's still running: Forcing shutdown"sv;
      logging::log_flush();
      std::quick_exit(70);
    };
    force_shutdown = task_pool.pushDelayed(task, 10s).task_id;

    // Break out of the main loop
    shutdown_event->raise(true);

    if (tray_is_enabled && config::sunshine.system_tray) {
      system_tray::end_tray();
    }

    display_device_deinit_guard = nullptr;
  });

#ifdef _WIN32
  // Terminate gracefully on Windows when console window is closed
  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#endif

  proc::refresh(config::stream.file_apps);

  // If any of the following fail, we log an error and continue event though sunshine will not function correctly.
  // This allows access to the UI to fix configuration problems or view the logs.

  auto platf_deinit_guard = platf::init();
  if (!platf_deinit_guard) {
    BOOST_LOG(error) << "Platform failed to initialize"sv;
  }

  auto proc_deinit_guard = proc::init();
  if (!proc_deinit_guard) {
    BOOST_LOG(error) << "Proc failed to initialize"sv;
  }

  reed_solomon_init();
  auto input_deinit_guard = input::init();

  if (input::probe_gamepads()) {
    BOOST_LOG(warning) << "No gamepad input is available"sv;
  }

  if (video::probe_encoders()) {
    BOOST_LOG(error) << "Video failed to find working encoder"sv;
  }

  if (http::init()) {
    BOOST_LOG(fatal) << "HTTP interface failed to initialize"sv;

#ifdef _WIN32
    BOOST_LOG(fatal) << "To relaunch Sunshine successfully, use the shortcut in the Start Menu. Do not run Sunshine.exe manually."sv;
    std::this_thread::sleep_for(10s);
#endif

    return -1;
  }

  std::unique_ptr<platf::deinit_t> mDNS;
  auto sync_mDNS = std::async(std::launch::async, [&mDNS]() {
    mDNS = platf::publish::start();
  });

  std::unique_ptr<platf::deinit_t> upnp_unmap;
  auto sync_upnp = std::async(std::launch::async, [&upnp_unmap]() {
    upnp_unmap = upnp::start();
  });

  // FIXME: Temporary workaround: Simple-Web_server needs to be updated or replaced
  if (shutdown_event->peek()) {
    return lifetime::desired_exit_code;
  }

  std::thread httpThread {nvhttp::start};
  std::thread configThread {confighttp::start};
  std::thread rtspThread {rtsp_stream::start};

#ifdef _WIN32
  // If we're using the default port and GameStream is enabled, warn the user
  if (config::sunshine.port == 47989 && is_gamestream_enabled()) {
    BOOST_LOG(fatal) << "GameStream is still enabled in GeForce Experience! This *will* cause streaming problems with Sunshine!"sv;
    BOOST_LOG(fatal) << "Disable GameStream on the SHIELD tab in GeForce Experience or change the Port setting on the Advanced tab in the Sunshine Web UI."sv;
  }
#endif

  if (tray_is_enabled && config::sunshine.system_tray) {
    BOOST_LOG(info) << "Starting system tray"sv;
#ifdef _WIN32
    // TODO: Windows has a weird bug where when running as a service and on the first Windows boot,
    // the tray icon would not appear even though Sunshine is running correctly otherwise.
    // Restarting the service would allow the icon to appear normally.
    // For now we will keep the Windows tray icon on a separate thread.
    // Ideally, we would run the system tray on the main thread for all platforms.
    system_tray::init_tray_threaded();
#else
    system_tray::init_tray();
#endif
  }

  mainThreadLoop(shutdown_event);

  httpThread.join();
  configThread.join();
  rtspThread.join();

  task_pool.stop();
  task_pool.join();

#ifdef _WIN32
  // Restore global NVIDIA control panel settings
  if (nvprefs_instance.owning_undo_file() && nvprefs_instance.load()) {
    nvprefs_instance.restore_global_profile();
    nvprefs_instance.unload();
  }
#endif

  return lifetime::desired_exit_code;
}
