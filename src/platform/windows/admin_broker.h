/**
 * @file src/platform/windows/admin_broker.h
 * @brief Admin broker planning boundary for SunshineSeat.
 */
#pragma once

// standard includes
#include <iosfwd>
#include <cstdint>
#include <string>
#include <vector>

namespace platf::admin_broker {

  struct task_definition_t {
    std::string id;
    std::string title;
    std::string description;
    std::string command;
    std::string rollback_command;
    bool requires_confirmation;
    bool requires_interactive_desktop;
    bool changes_secure_desktop_policy;
  };

  struct request_plan_t {
    std::string pipe_name;
    std::string task_id;
    std::vector<std::string> actions;
    std::vector<std::string> blockers;
    std::vector<std::string> audit_fields;
  };

  struct uac_policy_t {
    bool prompt_on_secure_desktop_known;
    std::uint32_t prompt_on_secure_desktop;
    std::vector<std::string> blockers;
  };

  struct execution_request_t {
    std::string task_id;
    bool dry_run;
    bool confirmed;
    bool secure_desktop_confirmed;
  };

  struct execution_result_t {
    std::string task_id;
    bool dry_run;
    bool elevated;
    bool executed;
    int exit_code;
    std::string command;
    std::vector<std::string> actions;
    std::vector<std::string> blockers;
  };

  struct broker_call_request_t {
    execution_request_t execution;
  };

  struct broker_exchange_result_t {
    bool connected;
    bool served;
    int exit_code;
    std::string caller_sid;
    std::uint32_t caller_session_id;
    std::string response;
    std::vector<std::string> actions;
    std::vector<std::string> blockers;
  };

  [[nodiscard]] std::string pipe_name();
  [[nodiscard]] std::string current_process_user_sid();
  [[nodiscard]] const std::vector<task_definition_t> &allowed_tasks();
  [[nodiscard]] request_plan_t plan_request(const std::string &task_id);
  [[nodiscard]] uac_policy_t query_uac_policy();
  [[nodiscard]] execution_result_t execute_request(const execution_request_t &request);
  [[nodiscard]] broker_exchange_result_t serve_once(const std::string &required_caller_sid);
  [[nodiscard]] broker_exchange_result_t call_broker(const broker_call_request_t &request);

  void print_task_list(std::ostream &out);
  void print_plan(std::ostream &out, const request_plan_t &plan);
  void print_uac_policy(std::ostream &out, const uac_policy_t &policy);
  void print_execution_result(std::ostream &out, const execution_result_t &result);
  void print_broker_exchange_result(std::ostream &out, const broker_exchange_result_t &result);

}  // namespace platf::admin_broker
