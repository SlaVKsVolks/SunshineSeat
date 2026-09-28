/**
 * @file src/platform/windows/admin_broker.cpp
 * @brief Admin broker planning boundary for SunshineSeat.
 */
// standard includes
#include <algorithm>
#include <iostream>
#include <memory>
#include <sstream>
#include <string_view>
#include <type_traits>

// platform includes
#include <windows.h>
#include <sddl.h>

// local includes
#include "admin_broker.h"

namespace platf::admin_broker {
  using namespace std::literals;

  namespace {
    const std::vector<task_definition_t> tasks {
      {
        "restart-sunshineseat",
        "Restart SunshineSeat",
        "Stop and restart the SunshineSeat process without elevating the streaming process.",
        "broker:restart-current-sunshineseat",
        "",
        true,
        false,
        false,
      },
      {
        "sudovda-status",
        "Collect SudoVDA status",
        "Run the SudoVDA status probe and write the result into the broker audit log.",
        "sunshine.exe --sudovda-status",
        "",
        false,
        false,
        false,
      },
      {
        "sudovda-clean-default",
        "Remove SunshineSeat test display",
        "Remove the fixed SunshineSeat SudoVDA test display GUID if it exists.",
        "sunshine.exe --sudovda-remove-default --allow-service-session",
        "",
        true,
        false,
        false,
      },
      {
        "repair-streaming-audio",
        "Repair streaming audio devices",
        "Run a future broker-owned audio repair routine for Steam Streaming Speakers and virtual audio routing.",
        "broker:repair-streaming-audio",
        "broker:restore-audio-routing",
        true,
        false,
        false,
      },
      {
        "maintenance-shell",
        "Open temporary elevated maintenance shell",
        "Start a short-lived elevated shell for manual repair when allowlisted tasks are insufficient.",
        "broker:open-maintenance-shell",
        "",
        true,
        true,
        false,
      },
      {
        "uac-remote-maintenance-on",
        "Allow remote-clickable UAC prompts",
        "Temporarily move UAC prompts off the secure desktop so the remote stream can capture and click them.",
        R"(reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System" /v PromptOnSecureDesktop /t REG_DWORD /d 0 /f)",
        R"(reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System" /v PromptOnSecureDesktop /t REG_DWORD /d 1 /f)",
        true,
        true,
        true,
      },
      {
        "uac-remote-maintenance-off",
        "Restore secure desktop UAC prompts",
        "Restore the default Secure Desktop UAC prompt behavior after remote maintenance.",
        R"(reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System" /v PromptOnSecureDesktop /t REG_DWORD /d 1 /f)",
        "",
        true,
        false,
        true,
      },
    };

    const task_definition_t *find_task(const std::string &task_id) {
      const auto task = std::find_if(tasks.begin(), tasks.end(), [&](const task_definition_t &candidate) {
        return candidate.id == task_id;
      });

      if (task == tasks.end()) {
        return nullptr;
      }

      return &*task;
    }

    struct handle_deleter_t {
      void operator()(HANDLE handle) const {
        if (handle && handle != INVALID_HANDLE_VALUE) {
          CloseHandle(handle);
        }
      }
    };

    using unique_handle_t = std::unique_ptr<std::remove_pointer_t<HANDLE>, handle_deleter_t>;

    struct client_identity_t {
      bool known;
      std::string sid;
      std::uint32_t session_id;
      std::vector<std::string> blockers;
    };

    std::string sid_from_token(HANDLE token) {
      DWORD needed = 0;
      GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
      if (needed == 0) {
        return {};
      }

      std::vector<unsigned char> buffer(needed);
      if (!GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed)) {
        return {};
      }

      const auto *user = reinterpret_cast<const TOKEN_USER *>(buffer.data());
      char *sid_string = nullptr;
      if (!ConvertSidToStringSidA(user->User.Sid, &sid_string)) {
        return {};
      }

      std::string result {sid_string};
      LocalFree(sid_string);
      return result;
    }

    std::uint32_t session_id_from_token(HANDLE token) {
      DWORD session_id = 0;
      DWORD returned_size = 0;
      if (!GetTokenInformation(token, TokenSessionId, &session_id, sizeof(session_id), &returned_size)) {
        return 0;
      }

      return static_cast<std::uint32_t>(session_id);
    }

    bool process_is_elevated() {
      HANDLE raw_token = nullptr;
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
        return false;
      }

      unique_handle_t token {raw_token};
      TOKEN_ELEVATION elevation {};
      DWORD returned_size = 0;
      if (!GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &returned_size)) {
        return false;
      }

      return elevation.TokenIsElevated != 0;
    }

    client_identity_t capture_pipe_client_identity(HANDLE pipe) {
      client_identity_t identity {
        false,
        "",
        0,
        {},
      };

      if (!ImpersonateNamedPipeClient(pipe)) {
        identity.blockers.emplace_back("Unable to impersonate broker pipe client. Windows error: "s + std::to_string(GetLastError()));
        return identity;
      }

      HANDLE raw_token = nullptr;
      if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw_token)) {
        const auto error_code = GetLastError();
        RevertToSelf();
        identity.blockers.emplace_back("Unable to open impersonated client token. Windows error: "s + std::to_string(error_code));
        return identity;
      }

      unique_handle_t token {raw_token};
      identity.sid = sid_from_token(token.get());
      identity.session_id = session_id_from_token(token.get());
      RevertToSelf();

      if (identity.sid.empty()) {
        identity.blockers.emplace_back("Unable to resolve broker pipe client SID.");
        return identity;
      }

      identity.known = true;
      return identity;
    }

    bool write_prompt_on_secure_desktop(std::uint32_t value, DWORD &error_code) {
      const DWORD reg_value = value;
      error_code = RegSetKeyValueW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        L"PromptOnSecureDesktop",
        REG_DWORD,
        &reg_value,
        sizeof(reg_value)
      );

      return error_code == ERROR_SUCCESS;
    }

    bool task_policy_value(const std::string &task_id, std::uint32_t &value) {
      if (task_id == "uac-remote-maintenance-on"sv) {
        value = 0;
        return true;
      }

      if (task_id == "uac-remote-maintenance-off"sv) {
        value = 1;
        return true;
      }

      return false;
    }

    std::string bool_wire(bool value) {
      return value ? "1"s : "0"s;
    }

    bool parse_bool_wire(const std::string &value) {
      return value == "1"sv || value == "true"sv || value == "yes"sv;
    }

    std::string trim_cr(std::string value) {
      while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
        value.pop_back();
      }

      return value;
    }

    std::string serialize_broker_request(const broker_call_request_t &request) {
      std::ostringstream out;
      out << "SUNSHINESEAT_ADMIN_BROKER_V1\n";
      out << "task_id=" << request.execution.task_id << '\n';
      out << "dry_run=" << bool_wire(request.execution.dry_run) << '\n';
      out << "confirm_maintenance=" << bool_wire(request.execution.confirmed) << '\n';
      out << "confirm_secure_desktop_change=" << bool_wire(request.execution.secure_desktop_confirmed) << '\n';
      return out.str();
    }

    execution_request_t parse_broker_request(const std::string &raw, std::vector<std::string> &blockers) {
      execution_request_t request {
        "",
        false,
        false,
        false,
      };

      std::istringstream input {raw};
      std::string line;
      if (!std::getline(input, line) || trim_cr(line) != "SUNSHINESEAT_ADMIN_BROKER_V1"sv) {
        blockers.emplace_back("Invalid broker request protocol header.");
        return request;
      }

      while (std::getline(input, line)) {
        line = trim_cr(line);
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
          continue;
        }

        const auto key = line.substr(0, separator);
        const auto value = line.substr(separator + 1);
        if (key == "task_id"sv) {
          request.task_id = value;
        } else if (key == "dry_run"sv) {
          request.dry_run = parse_bool_wire(value);
        } else if (key == "confirm_maintenance"sv) {
          request.confirmed = parse_bool_wire(value);
        } else if (key == "confirm_secure_desktop_change"sv) {
          request.secure_desktop_confirmed = parse_bool_wire(value);
        }
      }

      if (request.task_id.empty()) {
        blockers.emplace_back("Broker request did not include task_id.");
      }

      return request;
    }

    bool write_pipe_string(HANDLE pipe, const std::string &value, std::vector<std::string> &blockers) {
      DWORD bytes_written = 0;
      if (!WriteFile(pipe, value.data(), static_cast<DWORD>(value.size()), &bytes_written, nullptr)) {
        blockers.emplace_back("Unable to write broker pipe response. Windows error: "s + std::to_string(GetLastError()));
        return false;
      }

      if (bytes_written != value.size()) {
        blockers.emplace_back("Broker pipe write was incomplete.");
        return false;
      }

      return true;
    }
  }  // namespace

  std::string pipe_name() {
    return R"(\\.\pipe\SunshineSeatAdminBroker)";
  }

  std::string current_process_user_sid() {
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
      return {};
    }

    unique_handle_t token {raw_token};
    return sid_from_token(token.get());
  }

  const std::vector<task_definition_t> &allowed_tasks() {
    return tasks;
  }

  uac_policy_t query_uac_policy() {
    DWORD value = 0;
    DWORD value_size = sizeof(value);
    const auto result = RegGetValueW(
      HKEY_LOCAL_MACHINE,
      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
      L"PromptOnSecureDesktop",
      RRF_RT_REG_DWORD,
      nullptr,
      &value,
      &value_size
    );

    if (result != ERROR_SUCCESS) {
      return {false, 0, {"Unable to read PromptOnSecureDesktop. Windows error: "s + std::to_string(result)}};
    }

    return {true, static_cast<std::uint32_t>(value), {}};
  }

  request_plan_t plan_request(const std::string &task_id) {
    request_plan_t plan {
      pipe_name(),
      task_id,
      {},
      {},
      {
        "timestamp",
        "requesting_user_sid",
        "requesting_session_id",
        "task_id",
        "command",
        "exit_code",
        "stdout_path",
        "stderr_path",
      },
    };

    const auto *task = find_task(task_id);
    if (!task) {
      plan.blockers.emplace_back("Unknown admin broker task id.");
      return plan;
    }

    plan.actions.emplace_back("Connect to the broker named pipe: "s + plan.pipe_name);
    plan.actions.emplace_back("Broker verifies the caller token belongs to the active SunshineSeat target user.");
    plan.actions.emplace_back("Broker verifies task id is allowlisted: "s + task->id);
    if (task->requires_confirmation) {
      plan.actions.emplace_back("Broker requires an explicit per-request confirmation token.");
    }
    if (task->requires_interactive_desktop) {
      plan.actions.emplace_back("Broker marks the task as interactive-maintenance only; it must not run during unattended streaming.");
    }
    if (task->changes_secure_desktop_policy) {
      plan.actions.emplace_back("Broker records a Secure Desktop policy change and exposes the rollback command before execution.");
    }
    plan.actions.emplace_back("Broker executes: "s + task->command);
    if (!task->rollback_command.empty()) {
      plan.actions.emplace_back("Rollback command available: "s + task->rollback_command);
    }
    plan.actions.emplace_back("Broker writes audit fields before returning status to the user process.");

    return plan;
  }

  execution_result_t execute_request(const execution_request_t &request) {
    execution_result_t result {
      request.task_id,
      request.dry_run,
      process_is_elevated(),
      false,
      1,
      "",
      {},
      {},
    };

    const auto *task = find_task(request.task_id);
    if (!task) {
      result.blockers.emplace_back("Unknown admin broker task id.");
      return result;
    }

    result.command = task->command;
    result.actions.emplace_back("Resolved allowlisted task: "s + task->id);

    if (task->requires_confirmation && !request.confirmed) {
      result.blockers.emplace_back("Missing --confirm-maintenance for confirmation-required task.");
    }

    if (task->changes_secure_desktop_policy && !request.secure_desktop_confirmed) {
      result.blockers.emplace_back("Missing --confirm-secure-desktop-change for Secure Desktop policy task.");
    }

    std::uint32_t policy_value = 0;
    if (!task_policy_value(task->id, policy_value)) {
      result.blockers.emplace_back("Execution is not implemented for this broker task yet.");
    }

    if (!request.dry_run && !result.elevated) {
      result.blockers.emplace_back("Live broker execution requires an elevated process.");
    }

    if (!result.blockers.empty()) {
      return result;
    }

    result.actions.emplace_back("Validated maintenance confirmation flags.");
    result.actions.emplace_back("Validated UAC Secure Desktop policy target value: "s + std::to_string(policy_value));

    if (request.dry_run) {
      result.actions.emplace_back("Dry-run only; no registry value was changed.");
      result.exit_code = 0;
      return result;
    }

    DWORD error_code = ERROR_SUCCESS;
    if (!write_prompt_on_secure_desktop(policy_value, error_code)) {
      result.blockers.emplace_back("Unable to write PromptOnSecureDesktop. Windows error: "s + std::to_string(error_code));
      return result;
    }

    result.executed = true;
    result.exit_code = 0;
    result.actions.emplace_back("Wrote PromptOnSecureDesktop="s + std::to_string(policy_value));
    return result;
  }

  broker_exchange_result_t serve_once(const std::string &required_caller_sid) {
    broker_exchange_result_t result {
      false,
      false,
      1,
      "",
      0,
      "",
      {},
      {},
    };

    unique_handle_t pipe {
      CreateNamedPipeA(
        pipe_name().c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1,
        8192,
        8192,
        15000,
        nullptr
      )
    };

    if (pipe.get() == INVALID_HANDLE_VALUE) {
      result.blockers.emplace_back("Unable to create admin broker named pipe. Windows error: "s + std::to_string(GetLastError()));
      return result;
    }

    result.actions.emplace_back("Created broker named pipe: "s + pipe_name());
    const auto connected = ConnectNamedPipe(pipe.get(), nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
    if (!connected) {
      result.blockers.emplace_back("No broker client connected. Windows error: "s + std::to_string(GetLastError()));
      return result;
    }

    result.connected = true;
    result.actions.emplace_back("Accepted one local broker pipe client.");

    const auto identity = capture_pipe_client_identity(pipe.get());
    result.caller_sid = identity.sid;
    result.caller_session_id = identity.session_id;
    result.blockers.insert(result.blockers.end(), identity.blockers.begin(), identity.blockers.end());
    if (!identity.known) {
      return result;
    }

    result.actions.emplace_back("Captured caller SID: "s + identity.sid);
    result.actions.emplace_back("Captured caller session id: "s + std::to_string(identity.session_id));

    if (!required_caller_sid.empty() && identity.sid != required_caller_sid) {
      result.blockers.emplace_back("Broker client SID does not match the required caller SID.");
      return result;
    }

    char buffer[4096] {};
    DWORD bytes_read = 0;
    if (!ReadFile(pipe.get(), buffer, sizeof(buffer) - 1, &bytes_read, nullptr)) {
      result.blockers.emplace_back("Unable to read broker request. Windows error: "s + std::to_string(GetLastError()));
      return result;
    }

    std::vector<std::string> parse_blockers;
    const auto execution_request = parse_broker_request(std::string {buffer, bytes_read}, parse_blockers);
    if (!parse_blockers.empty()) {
      result.blockers.insert(result.blockers.end(), parse_blockers.begin(), parse_blockers.end());
      return result;
    }

    const auto execution = execute_request(execution_request);
    std::ostringstream response;
    response << "SunshineSeat admin broker served request\n";
    response << "  caller_sid: " << result.caller_sid << '\n';
    response << "  caller_session_id: " << result.caller_session_id << '\n';
    print_execution_result(response, execution);
    result.response = response.str();
    result.served = true;
    result.exit_code = execution.exit_code;

    std::vector<std::string> write_blockers;
    write_pipe_string(pipe.get(), result.response, write_blockers);
    result.blockers.insert(result.blockers.end(), write_blockers.begin(), write_blockers.end());
    DisconnectNamedPipe(pipe.get());
    return result;
  }

  broker_exchange_result_t call_broker(const broker_call_request_t &request) {
    broker_exchange_result_t result {
      false,
      false,
      1,
      "",
      0,
      "",
      {},
      {},
    };

    if (!WaitNamedPipeA(pipe_name().c_str(), 15000)) {
      result.blockers.emplace_back("Admin broker pipe is not available. Windows error: "s + std::to_string(GetLastError()));
      return result;
    }

    unique_handle_t pipe {
      CreateFileA(
        pipe_name().c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION,
        nullptr
      )
    };

    if (pipe.get() == INVALID_HANDLE_VALUE) {
      result.blockers.emplace_back("Unable to connect to admin broker pipe. Windows error: "s + std::to_string(GetLastError()));
      return result;
    }

    result.connected = true;
    result.actions.emplace_back("Connected to broker named pipe: "s + pipe_name());

    DWORD pipe_mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe.get(), &pipe_mode, nullptr, nullptr)) {
      result.blockers.emplace_back("Unable to set broker pipe read mode. Windows error: "s + std::to_string(GetLastError()));
      return result;
    }

    std::vector<std::string> write_blockers;
    if (!write_pipe_string(pipe.get(), serialize_broker_request(request), write_blockers)) {
      result.blockers.insert(result.blockers.end(), write_blockers.begin(), write_blockers.end());
      return result;
    }

    result.actions.emplace_back("Sent allowlisted broker request.");

    std::ostringstream response;
    char buffer[4096] {};
    DWORD error_code = ERROR_SUCCESS;
    while (true) {
      DWORD bytes_read = 0;
      if (ReadFile(pipe.get(), buffer, sizeof(buffer), &bytes_read, nullptr)) {
        response.write(buffer, bytes_read);
        if (bytes_read < sizeof(buffer)) {
          break;
        }
        continue;
      }

      error_code = GetLastError();
      if (error_code == ERROR_MORE_DATA) {
        response.write(buffer, bytes_read);
        continue;
      }

      if (error_code == ERROR_BROKEN_PIPE) {
        break;
      }

      break;
    }

    if (response.tellp() == std::streampos(0) && error_code != ERROR_BROKEN_PIPE) {
      result.blockers.emplace_back("Unable to read broker response. Windows error: "s + std::to_string(error_code));
      return result;
    }

    result.response = response.str();
    result.served = true;
    result.exit_code = result.response.find("  exit_code: 0\n") != std::string::npos ? 0 : 1;
    return result;
  }

  void print_task_list(std::ostream &out) {
    out << "SunshineSeat admin broker tasks\n";
    out << "  pipe_name: " << pipe_name() << '\n';
    for (const auto &task : tasks) {
      out << "  - " << task.id << '\n';
      out << "      title: " << task.title << '\n';
      out << "      command: " << task.command << '\n';
      out << "      confirmation_required: " << (task.requires_confirmation ? "yes"sv : "no"sv) << '\n';
      out << "      interactive_desktop_required: " << (task.requires_interactive_desktop ? "yes"sv : "no"sv) << '\n';
      out << "      changes_secure_desktop_policy: " << (task.changes_secure_desktop_policy ? "yes"sv : "no"sv) << '\n';
    }
  }

  void print_plan(std::ostream &out, const request_plan_t &plan) {
    out << "SunshineSeat admin broker request plan\n";
    out << "  task_id: " << plan.task_id << '\n';
    out << "  pipe_name: " << plan.pipe_name << '\n';
    out << "  blocked: " << (plan.blockers.empty() ? "no"sv : "yes"sv) << '\n';
    out << "  actions:\n";
    for (const auto &action : plan.actions) {
      out << "    - " << action << '\n';
    }
    if (plan.actions.empty()) {
      out << "    - none\n";
    }
    out << "  audit_fields:\n";
    for (const auto &field : plan.audit_fields) {
      out << "    - " << field << '\n';
    }
    out << "  blockers:\n";
    if (plan.blockers.empty()) {
      out << "    - none\n";
    } else {
      for (const auto &blocker : plan.blockers) {
        out << "    - " << blocker << '\n';
      }
    }
  }

  void print_uac_policy(std::ostream &out, const uac_policy_t &policy) {
    out << "SunshineSeat UAC remote maintenance status\n";
    out << "  preferred_path: broker-mediated allowlisted elevated tasks\n";
    out << "  remote_clickable_prompt_mode: explicit maintenance only\n";
    if (policy.prompt_on_secure_desktop_known) {
      out << "  PromptOnSecureDesktop: " << policy.prompt_on_secure_desktop << '\n';
      out << "  secure_desktop_uac: " << (policy.prompt_on_secure_desktop == 0 ? "disabled"sv : "enabled"sv) << '\n';
      out << "  remote_clickable_uac_prompts: " << (policy.prompt_on_secure_desktop == 0 ? "possible"sv : "blocked_by_secure_desktop"sv) << '\n';
    } else {
      out << "  PromptOnSecureDesktop: unknown\n";
      out << "  remote_clickable_uac_prompts: unknown\n";
    }
    out << "  broker_task_to_enable_remote_clicks: uac-remote-maintenance-on\n";
    out << "  broker_task_to_restore_secure_desktop: uac-remote-maintenance-off\n";
    out << "  warning: remote-clickable UAC prompts require disabling Secure Desktop until restored.\n";
    out << "  blockers:\n";
    if (policy.blockers.empty()) {
      out << "    - none\n";
    } else {
      for (const auto &blocker : policy.blockers) {
        out << "    - " << blocker << '\n';
      }
    }
  }

  void print_execution_result(std::ostream &out, const execution_result_t &result) {
    out << "SunshineSeat admin broker execution result\n";
    out << "  task_id: " << result.task_id << '\n';
    out << "  dry_run: " << (result.dry_run ? "yes"sv : "no"sv) << '\n';
    out << "  elevated: " << (result.elevated ? "yes"sv : "no"sv) << '\n';
    out << "  executed: " << (result.executed ? "yes"sv : "no"sv) << '\n';
    out << "  exit_code: " << result.exit_code << '\n';
    out << "  command: " << result.command << '\n';
    out << "  actions:\n";
    if (result.actions.empty()) {
      out << "    - none\n";
    } else {
      for (const auto &action : result.actions) {
        out << "    - " << action << '\n';
      }
    }
    out << "  blockers:\n";
    if (result.blockers.empty()) {
      out << "    - none\n";
    } else {
      for (const auto &blocker : result.blockers) {
        out << "    - " << blocker << '\n';
      }
    }
  }

  void print_broker_exchange_result(std::ostream &out, const broker_exchange_result_t &result) {
    out << "SunshineSeat admin broker exchange result\n";
    out << "  connected: " << (result.connected ? "yes"sv : "no"sv) << '\n';
    out << "  served: " << (result.served ? "yes"sv : "no"sv) << '\n';
    out << "  exit_code: " << result.exit_code << '\n';
    if (!result.caller_sid.empty()) {
      out << "  caller_sid: " << result.caller_sid << '\n';
      out << "  caller_session_id: " << result.caller_session_id << '\n';
    }
    out << "  actions:\n";
    if (result.actions.empty()) {
      out << "    - none\n";
    } else {
      for (const auto &action : result.actions) {
        out << "    - " << action << '\n';
      }
    }
    out << "  blockers:\n";
    if (result.blockers.empty()) {
      out << "    - none\n";
    } else {
      for (const auto &blocker : result.blockers) {
        out << "    - " << blocker << '\n';
      }
    }
    if (!result.response.empty()) {
      out << "  response:\n" << result.response;
    }
  }

}  // namespace platf::admin_broker
