/**
 * @file src/nvhttp.cpp
 * @brief Definitions for the nvhttp (GameStream) server.
 */
// macros
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <format>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

// lib includes
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/context_base.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <Simple-Web-Server/server_http.hpp>

// local includes
#include "config.h"
#include "display_device.h"
#include "file_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "network_test_jobs.h"
#include "network_test_provider.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "process.h"
#include "rtsp.h"
#include "utility.h"
#include "uuid.h"
#include "video.h"

using namespace std::literals;

namespace nvhttp {

  static constexpr std::string_view EMPTY_PROPERTY_TREE_ERROR_MSG = "Property tree is empty. Probably, control flow got interrupted by an unexpected C++ exception. This is a bug in Sunshine. Moonlight-qt will report Malformed XML (missing root element)."sv;

  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  crypto::cert_chain_t cert_chain;
  struct named_cert_t;

  class SunshineHTTPSServer: public SimpleWeb::ServerBase<SunshineHTTPS> {
  public:
    SunshineHTTPSServer(const std::string &certification_file, const std::string &private_key_file):
        ServerBase<SunshineHTTPS>::ServerBase(443),
        context(boost::asio::ssl::context::tls_server) {
      // Disabling TLS 1.0 and 1.1 (see RFC 8996)
      context.set_options(boost::asio::ssl::context::no_tlsv1);
      context.set_options(boost::asio::ssl::context::no_tlsv1_1);
      context.use_certificate_chain_file(certification_file);
      context.use_private_key_file(private_key_file, boost::asio::ssl::context::pem);
    }

    std::function<std::shared_ptr<named_cert_t>(SSL *)> verify;
    std::function<void(std::shared_ptr<Response>, std::shared_ptr<Request>)> on_verify_failed;

  protected:
    boost::asio::ssl::context context;

    void after_bind() override {
      if (verify) {
        context.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_fail_if_no_peer_cert | boost::asio::ssl::verify_client_once);
        context.set_verify_callback([](int verified, boost::asio::ssl::verify_context &ctx) {
          // To respond with an error message, a connection must be established
          return 1;
        });
      }
    }

    // This is Server<HTTPS>::accept() with SSL validation support added
    void accept() override {
      auto connection = create_connection(*io_service, context);

      acceptor->async_accept(connection->socket->lowest_layer(), [this, connection](const SimpleWeb::error_code &ec) {
        auto lock = connection->handler_runner->continue_lock();
        if (!lock) {
          return;
        }

        if (ec != SimpleWeb::error::operation_aborted) {
          this->accept();
        }

        auto session = std::make_shared<Session>(config.max_request_streambuf_size, connection);

        if (!ec) {
          boost::asio::ip::tcp::no_delay option(true);
          SimpleWeb::error_code ec;
          session->connection->socket->lowest_layer().set_option(option, ec);

          session->connection->set_timeout(config.timeout_request);
          session->connection->socket->async_handshake(boost::asio::ssl::stream_base::server, [this, session](const SimpleWeb::error_code &ec) {
            session->connection->cancel_timeout();
            auto lock = session->connection->handler_runner->continue_lock();
            if (!lock) {
              return;
            }
            if (!ec) {
              auto verified_client = verify ? verify(session->connection->socket->native_handle()) : nullptr;
              if (verify && !verified_client) {
                this->write(session, on_verify_failed);
              } else {
                if (verified_client) {
                  session->request->userp = std::move(verified_client);
                }
                this->read(session);
              }
            } else if (this->on_error) {
              this->on_error(session->request, ec);
            }
          });
        } else if (this->on_error) {
          this->on_error(session->request, ec);
        }
      });
    }
  };

  using https_server_t = SunshineHTTPSServer;
  using http_server_t = SimpleWeb::Server<SimpleWeb::HTTP>;

  struct conf_intern_t {
    std::string servercert;
    std::string pkey;
  } conf_intern;

  struct named_cert_t {
    std::string name;
    std::string uuid;
    std::string cert;
    bool enabled = true;
    std::uint64_t apollo_perm = static_cast<std::uint64_t>(crypto::PERM::_all);
    bool allow_client_commands = true;
    bool always_use_virtual_display = false;
    std::string display_mode;
    std::list<crypto::command_entry_t> do_cmds;
    std::list<crypto::command_entry_t> undo_cmds;
  };

  struct client_t {
    std::vector<named_cert_t> named_devices;
  };

  using p_named_cert_t = std::shared_ptr<named_cert_t>;

  // uniqueID, session
  std::unordered_map<std::string, pair_session_t> map_id_sess;
  client_t client_root;
  std::atomic<uint32_t> session_id_counter;

  // Set by TLS verify callback, read by launch/resume handler (single-threaded HTTPS server)
  std::string last_verified_client_cert;  // NOSONAR(cpp:S5421) - intentionally mutable global

  std::mutex dual_display_prepared_pair_provider_mutex;
  dual_display_launch::prepared_pair_provider_t dual_display_prepared_pair_provider;

  using args_t = SimpleWeb::CaseInsensitiveMultimap;
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Request>;
  using resp_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>;
  using req_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Request>;

  std::string rtsp_session_host(const req_https_t &request) {
    if (!config::nvhttp.external_ip.empty()) {
      boost::system::error_code ec;
      const auto external_address = boost::asio::ip::make_address(config::nvhttp.external_ip, ec);
      if (!ec) {
        return net::addr_to_url_escaped_string(external_address);
      }

      BOOST_LOG(warning)
        << "Invalid configured external_ip for RTSP session URL [external_ip="
        << config::nvhttp.external_ip << ", error=" << ec.message() << ']';
    }

    return net::addr_to_url_escaped_string(request->local_endpoint().address());
  }

  void set_dual_display_prepared_pair_provider(dual_display_launch::prepared_pair_provider_t provider) {
    std::lock_guard lock {dual_display_prepared_pair_provider_mutex};
    dual_display_prepared_pair_provider = std::move(provider);
  }

  dual_display_launch::prepared_pair_provider_t get_dual_display_prepared_pair_provider() {
    std::lock_guard lock {dual_display_prepared_pair_provider_mutex};
    return dual_display_prepared_pair_provider;
  }

  std::string select_launch_client_identity(
    const std::string_view requested_client_identity,
    const std::string_view verified_client_identity,
    const bool dual_display_requested
  ) {
    return dual_display_requested ? std::string {verified_client_identity} : std::string {requested_client_identity};
  }

  bool should_apply_legacy_display_configuration(const dual_display_launch::prepared_pair_ptr &prepared_pair) {
    return !prepared_pair;
  }

  bool should_probe_encoders_after_display_preparation(const bool prepared_dual_display_pair) {
    return !prepared_dual_display_pair;
  }

  bool should_reset_running_app_state_for_launch(int current_appid, int active_session_count, bool is_input_only);

  enum class op_e {
    ADD,  ///< Add certificate
    REMOVE  ///< Remove certificate
  };

  std::string get_arg(const args_t &args, const char *name, const char *default_value = nullptr) {
    auto it = args.find(name);
    if (it == std::end(args)) {
      if (default_value != nullptr) {
        return std::string(default_value);
      }

      throw std::out_of_range(name);
    }
    return it->second;
  }

  bool valid_hex_string(std::string_view value, std::size_t expected_bytes = 0) {
    if (value.empty() || value.size() % 2 != 0 || (expected_bytes != 0 && value.size() != expected_bytes * 2)) {
      return false;
    }

    return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
      return std::isxdigit(character) != 0;
    });
  }

  bool valid_pairing_device_name(std::string_view name) {
    if (name.empty() || name.size() > 128) {
      return false;
    }

    return std::all_of(name.begin(), name.end(), [](const unsigned char character) {
      return character != '\0' && (character >= 0x20 || character == '\t');
    });
  }

  bool pairing_password_configured() {
    return config::sunshine.pairing_password_iterations == crypto::PAIRING_PASSWORD_ITERATIONS &&
           valid_hex_string(config::sunshine.pairing_password_salt, 16) &&
           valid_hex_string(config::sunshine.pairing_password_verifier, 32);
  }

  pt::ptree command_list_to_ptree(const std::list<crypto::command_entry_t> &commands) {
    pt::ptree nodes;
    for (const auto &command : commands) {
      pt::ptree node;
      node.put("cmd"s, command.cmd);
      node.put("elevated"s, command.elevated);
      nodes.push_back(std::make_pair(""s, std::move(node)));
    }
    return nodes;
  }

  std::list<crypto::command_entry_t> command_list_from_ptree(const pt::ptree &node, const std::string &key) {
    std::list<crypto::command_entry_t> commands;
    const auto child = node.get_child_optional(key);
    if (!child) {
      return commands;
    }

    for (const auto &[_, command_node] : *child) {
      crypto::command_entry_t command;
      command.cmd = command_node.get<std::string>("cmd", ""s);
      command.elevated = command_node.get<bool>("elevated", false);
      if (!command.cmd.empty()) {
        commands.emplace_back(std::move(command));
      }
    }

    return commands;
  }

  const named_cert_t *find_named_device(std::string_view uuid) {
    if (uuid.empty() || uuid == "unknown"sv) {
      return nullptr;
    }

    for (const auto &named_device : client_root.named_devices) {
      if (named_device.enabled && named_device.uuid == uuid) {
        return &named_device;
      }
    }

    return nullptr;
  }

  p_named_cert_t find_named_device_by_cert(std::string_view cert_pem) {
    for (auto &named_device : client_root.named_devices) {
      if (named_device.cert == cert_pem) {
        return p_named_cert_t {&named_device, [](named_cert_t *) {
                               }};
      }
    }

    return nullptr;
  }

  named_cert_t *get_verified_cert(req_https_t request) {
    return static_cast<named_cert_t *>(request->userp.get());
  }

  namespace {

    network_test::stream_state_e diagnostic_stream_state() {
      const auto sessions = rtsp_stream::session_count();
      if (sessions < 0) {
        return network_test::stream_state_e::unknown;
      }
      return sessions == 0 ? network_test::stream_state_e::idle : network_test::stream_state_e::active;
    }

    nlohmann::json serialize_network_limits(const network_test::limits_t &limits) {
      return {
        {"latency_request_count", limits.latency_request_count},
        {"minimum_latency_samples", limits.minimum_latency_samples},
        {"latency_timeout_ms", limits.latency_timeout.count() * 1000},
        {"transfer_ramp_bytes", limits.transfer_ramp_bytes},
        {"direction_byte_cap", limits.direction_byte_cap},
        {"direction_timeout_ms", limits.direction_timeout.count() * 1000},
        {"workflow_timeout_ms", limits.workflow_timeout.count() * 1000},
      };
    }

    nlohmann::json serialize_network_job(const network_test::route_job_t &job) {
      return {
        {"job_id", job.job_id},
        {"nonce", job.nonce},
        {"expires_in_ms", job.expires_in_ms},
        {"limits", serialize_network_limits(job.limits)},
      };
    }

    nlohmann::json serialize_network_result(const network_test::result_t &result) {
      return nlohmann::json::parse(network_test::serialize_result(result));
    }

    void write_network_json(const resp_https_t &response, const int status, nlohmann::json output) {
      output["status_code"] = status;
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      headers.emplace("Cache-Control", "no-store");
      response->write(static_cast<SimpleWeb::StatusCode>(status), output.dump(), headers);
    }

    void write_network_job_response(const resp_https_t &response, const network_test::job_response_t &result) {
      if (result.http_status == 204) {
        response->write(SimpleWeb::StatusCode::success_no_content);
        return;
      }

      nlohmann::json output;
      if (result.job) {
        output["job"] = serialize_network_job(*result.job);
      }
      if (result.result) {
        output["result"] = serialize_network_result(*result.result);
      }
      if (!result.error_code.empty()) {
        output["error"] = {
          {"code", result.error_code},
        };
      }
      write_network_json(response, result.http_status, std::move(output));
    }

    std::optional<std::string> verified_diagnostic_client(const resp_https_t &response, const req_https_t &request) {
      const auto *const named_cert = get_verified_cert(request);
      if (!named_cert || !named_cert->enabled || named_cert->uuid.empty()) {
        write_network_json(response, 401, {{"error", {{"code", "client_certificate_required"}}}});
        return std::nullopt;
      }
      return named_cert->uuid;
    }

    std::optional<std::uint64_t> parse_bounded_bytes(const std::string_view value) {
      if (value.empty()) {
        return std::nullopt;
      }
      std::uint64_t bytes = 0;
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), bytes);
      if (error != std::errc {} || end != value.data() + value.size()) {
        return std::nullopt;
      }
      return bytes;
    }

    std::string final_path_component(const std::string_view path) {
      const auto slash = path.find_last_of('/');
      return slash == std::string_view::npos ? std::string {} : std::string {path.substr(slash + 1)};
    }

    bool has_too_large_body(const req_https_t &request, const std::uint64_t limit) {
      const auto header = request->header.find("content-length");
      if (header != request->header.end()) {
        const auto bytes = parse_bounded_bytes(header->second);
        return !bytes || *bytes > limit;
      }
      return false;
    }

    std::optional<std::uint64_t> declared_body_length(const req_https_t &request) {
      const auto header = request->header.find("content-length");
      if (header == request->header.end()) {
        return std::nullopt;
      }
      return parse_bounded_bytes(header->second);
    }

    bool has_octet_stream_content_type(const req_https_t &request) {
      const auto header = request->header.find("content-type");
      if (header == request->header.end()) {
        return false;
      }
      const auto separator = header->second.find(';');
      return std::string_view {header->second}.substr(0, separator) == "application/octet-stream";
    }

    bool has_json_content_type(const req_https_t &request) {
      const auto header = request->header.find("content-type");
      if (header == request->header.end()) {
        return false;
      }
      const auto separator = header->second.find(';');
      return std::string_view {header->second}.substr(0, separator) == "application/json";
    }

    void networktest_capabilities(resp_https_t response, req_https_t request) {
      if (!verified_diagnostic_client(response, request)) {
        return;
      }
      const auto state = diagnostic_stream_state();
      write_network_json(response, 200, {
                                          {"schema_version", network_test::current_schema_version},
                                          {"limits", serialize_network_limits(network_test::default_limits())},
                                          {"stream_state", state == network_test::stream_state_e::idle ? "idle" : (state == network_test::stream_state_e::active ? "active" : "unknown")},
                                          {"public_provider_available", true},
                                          {"supported_stages", {"route_latency", "route_download", "route_upload", "client_public", "host_public", "passive_stream"}},
                                        });
    }

    void networktest_create_route_job(resp_https_t response, req_https_t request) {
      const auto client_id = verified_diagnostic_client(response, request);
      if (!client_id) {
        return;
      }
      write_network_job_response(response, network_test_jobs().create_route_job(*client_id));
    }

    void networktest_ping_route_job(resp_https_t response, req_https_t request) {
      const auto client_id = verified_diagnostic_client(response, request);
      if (!client_id) {
        return;
      }
      const auto query = request->parse_query_string();
      write_network_job_response(response, network_test_jobs().ping_route_job(*client_id, final_path_component(request->path), get_arg(query, "nonce", "")));
    }

    void networktest_download_route_job(resp_https_t response, req_https_t request) {
      const auto client_id = verified_diagnostic_client(response, request);
      if (!client_id) {
        return;
      }
      const auto query = request->parse_query_string();
      const auto bytes = parse_bounded_bytes(get_arg(query, "bytes", ""));
      if (!bytes) {
        write_network_json(response, 400, {{"error", {{"code", "invalid_download_bytes"}}}});
        return;
      }
      const auto job_result = network_test_jobs().read_download(*client_id, final_path_component(request->path), get_arg(query, "nonce", ""), *bytes);
      if (!job_result.payload) {
        write_network_job_response(response, job_result);
        return;
      }

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/octet-stream");
      headers.emplace("Content-Length", std::to_string(job_result.payload->size()));
      headers.emplace("Cache-Control", "no-store");
      const std::string body {reinterpret_cast<const char *>(job_result.payload->data()), job_result.payload->size()};
      response->write(SimpleWeb::StatusCode::success_ok, body, headers);
    }

    void networktest_upload_route_job(resp_https_t response, req_https_t request) {
      const auto client_id = verified_diagnostic_client(response, request);
      if (!client_id) {
        return;
      }
      if (!has_octet_stream_content_type(request)) {
        write_network_json(response, 400, {{"error", {{"code", "invalid_upload_content_type"}}}});
        return;
      }
      const auto content_length = declared_body_length(request);
      if (!content_length) {
        write_network_json(response, 400, {{"error", {{"code", "upload_content_length_required"}}}});
        return;
      }
      if (has_too_large_body(request, network_test::default_limits().direction_byte_cap)) {
        write_network_json(response, 413, {{"error", {{"code", "upload_bytes_exceeds_cap"}}}});
        return;
      }
      const auto content = request->content.string();
      if (content.empty() || content.size() != *content_length || content.size() > network_test::default_limits().direction_byte_cap) {
        write_network_json(response, 413, {{"error", {{"code", "upload_bytes_exceeds_cap"}}}});
        return;
      }
      const auto query = request->parse_query_string();
      const std::vector<std::uint8_t> body {content.begin(), content.end()};
      write_network_job_response(response, network_test_jobs().consume_upload(*client_id, final_path_component(request->path), get_arg(query, "nonce", ""), body));
    }

    void networktest_cancel_route_job(resp_https_t response, req_https_t request) {
      const auto client_id = verified_diagnostic_client(response, request);
      if (!client_id) {
        return;
      }
      const auto query = request->parse_query_string();
      write_network_job_response(response, network_test_jobs().cancel_route_job(*client_id, final_path_component(request->path), get_arg(query, "nonce", "")));
    }

    void networktest_create_host_public_job(resp_https_t response, req_https_t request) {
      if (!verified_diagnostic_client(response, request)) {
        return;
      }
      const auto result = network_test_jobs().create_host_public_job();
      write_network_job_response(response, result);
      if (!result.job) {
        return;
      }
      const auto job_id = result.job->job_id;
      std::thread {[job_id] {
        network_test::curl_public_speed_provider_t provider;
        static_cast<void>(network_test_jobs().run_host_public_job(job_id, provider));
      }}.detach();
    }

    void networktest_host_public_job_status(resp_https_t response, req_https_t request) {
      if (!verified_diagnostic_client(response, request)) {
        return;
      }
      write_network_job_response(response, network_test_jobs().host_public_job_status(final_path_component(request->path)));
    }

    void networktest_cancel_host_public_job(resp_https_t response, req_https_t request) {
      if (!verified_diagnostic_client(response, request)) {
        return;
      }
      write_network_job_response(response, network_test_jobs().cancel_host_public_job(final_path_component(request->path)));
    }

    void networktest_submit_client_result(resp_https_t response, req_https_t request) {
      const auto client_id = verified_diagnostic_client(response, request);
      if (!client_id) {
        return;
      }
      constexpr auto max_result_body_bytes = std::uint64_t {64} * 1024;
      if (!has_json_content_type(request)) {
        write_network_json(response, 400, {{"error", {{"code", "invalid_result_content_type"}}}});
        return;
      }
      if (has_too_large_body(request, max_result_body_bytes)) {
        write_network_json(response, 413, {{"error", {{"code", "result_body_too_large"}}}});
        return;
      }
      const auto content = request->content.string();
      if (content.size() > max_result_body_bytes) {
        write_network_json(response, 413, {{"error", {{"code", "result_body_too_large"}}}});
        return;
      }
      std::string failure;
      const auto result = network_test::parse_result(content, failure);
      if (!result) {
        write_network_json(response, 400, {{"error", {{"code", failure.empty() ? "invalid_result" : failure}}}});
        return;
      }
      write_network_job_response(response, network_test_jobs().submit_client_result(*client_id, *result));
    }

    void networktest_latest_results(resp_https_t response, req_https_t request) {
      const auto client_id = verified_diagnostic_client(response, request);
      if (!client_id) {
        return;
      }
      auto results = network_test_jobs().latest_results(*client_id);
      nlohmann::json output;
      if (results.client_result) {
        output["client_result"] = serialize_network_result(*results.client_result);
      }
      if (results.host_public_result) {
        output["host_public_result"] = serialize_network_result(*results.host_public_result);
      }
      write_network_json(response, 200, std::move(output));
    }

  }  // namespace

  network_test::diagnostic_jobs_t &network_test_jobs() {
    static network_test::diagnostic_jobs_t jobs {
      platf::appdata() / "network-test-results.json",
      network_test::make_system_diagnostic_clock(),
      diagnostic_stream_state,
    };
    return jobs;
  }

  void parse_mode_string(std::string_view mode_string, int &width, int &height, int &fps) {
    std::stringstream mode {std::string {mode_string}};
    int index = 0;
    std::string segment;
    while (std::getline(mode, segment, 'x')) {
      if (index == 0) {
        width = atoi(segment.c_str());
      } else if (index == 1) {
        height = atoi(segment.c_str());
      } else if (index == 2) {
        fps = atoi(segment.c_str());
      }
      ++index;
    }
  }

  void save_state() {
    pt::ptree root;

    if (fs::exists(config::nvhttp.file_state)) {
      try {
        pt::read_json(config::nvhttp.file_state, root);
      } catch (std::exception &e) {
        BOOST_LOG(error) << "Couldn't read "sv << config::nvhttp.file_state << ": "sv << e.what();
        return;
      }
    }

    root.erase("root"s);

    root.put("root.uniqueid", http::unique_id);
    client_t &client = client_root;
    pt::ptree node;

    pt::ptree named_cert_nodes;
    for (auto &named_cert : client.named_devices) {
      pt::ptree named_cert_node;
      named_cert_node.put("name"s, named_cert.name);
      named_cert_node.put("cert"s, named_cert.cert);
      named_cert_node.put("uuid"s, named_cert.uuid);
      named_cert_node.put("enabled"s, named_cert.enabled);
      named_cert_node.put("perm"s, named_cert.apollo_perm);
      named_cert_node.put("allow_client_commands"s, named_cert.allow_client_commands);
      named_cert_node.put("always_use_virtual_display"s, named_cert.always_use_virtual_display);
      named_cert_node.put("display_mode"s, named_cert.display_mode);
      named_cert_node.add_child("do"s, command_list_to_ptree(named_cert.do_cmds));
      named_cert_node.add_child("undo"s, command_list_to_ptree(named_cert.undo_cmds));
      named_cert_nodes.push_back(std::make_pair(""s, named_cert_node));
    }
    root.add_child("root.named_devices"s, named_cert_nodes);

    try {
      pt::write_json(config::nvhttp.file_state, root);
    } catch (std::exception &e) {
      BOOST_LOG(error) << "Couldn't write "sv << config::nvhttp.file_state << ": "sv << e.what();
      return;
    }
  }

  void load_state() {
    if (!fs::exists(config::nvhttp.file_state)) {
      BOOST_LOG(info) << "File "sv << config::nvhttp.file_state << " doesn't exist"sv;
      http::unique_id = uuid_util::uuid_t::generate().string();
      return;
    }

    pt::ptree tree;
    try {
      pt::read_json(config::nvhttp.file_state, tree);
    } catch (std::exception &e) {
      BOOST_LOG(error) << "Couldn't read "sv << config::nvhttp.file_state << ": "sv << e.what();

      return;
    }

    auto unique_id_p = tree.get_optional<std::string>("root.uniqueid");
    if (!unique_id_p) {
      // This file doesn't contain moonlight credentials
      http::unique_id = uuid_util::uuid_t::generate().string();
      return;
    }
    http::unique_id = std::move(*unique_id_p);

    auto root = tree.get_child("root");
    client_t client;

    // Import from old format
    if (root.get_child_optional("devices")) {
      auto device_nodes = root.get_child("devices");
      for (auto &[_, device_node] : device_nodes) {
        auto uniqID = device_node.get<std::string>("uniqueid");

        if (device_node.count("certs")) {
          for (auto &[_, el] : device_node.get_child("certs")) {
            named_cert_t named_cert;
            named_cert.name = ""s;
            named_cert.cert = el.get_value<std::string>();
            named_cert.uuid = uuid_util::uuid_t::generate().string();
            client.named_devices.emplace_back(named_cert);
          }
        }
      }
    }

    if (root.count("named_devices")) {
      for (auto &[_, el] : root.get_child("named_devices")) {
        named_cert_t named_cert;
        named_cert.name = el.get_child("name").get_value<std::string>();
        named_cert.cert = el.get_child("cert").get_value<std::string>();
        named_cert.uuid = el.get_child("uuid").get_value<std::string>();
        named_cert.enabled = el.get<bool>("enabled", true);
        named_cert.apollo_perm = el.get<std::uint64_t>("perm", static_cast<std::uint64_t>(crypto::PERM::_all));
        named_cert.allow_client_commands = el.get<bool>("allow_client_commands", true);
        named_cert.always_use_virtual_display = el.get<bool>("always_use_virtual_display", false);
        named_cert.display_mode = el.get<std::string>("display_mode", ""s);
        named_cert.do_cmds = command_list_from_ptree(el, "do"s);
        named_cert.undo_cmds = command_list_from_ptree(el, "undo"s);
        client.named_devices.emplace_back(named_cert);
      }
    }

    // Empty certificate chain and import certs from file
    cert_chain.clear();
    for (auto &named_cert : client.named_devices) {
      cert_chain.add(crypto::x509(named_cert.cert));
    }

    client_root = client;
  }

  void add_authorized_client(const std::string &name, std::string &&cert) {
    client_t &client = client_root;
    named_cert_t named_cert;
    named_cert.name = name;
    named_cert.cert = std::move(cert);
    named_cert.uuid = uuid_util::uuid_t::generate().string();
    named_cert.apollo_perm = static_cast<std::uint64_t>(crypto::PERM::_all);
    client.named_devices.emplace_back(named_cert);

    if (!config::sunshine.flags[config::flag::FRESH_STATE]) {
      save_state();
    }
  }

  std::shared_ptr<rtsp_stream::launch_session_t> make_launch_session(
    bool host_audio,
    const args_t &args,
    const named_cert_t *verified_device = nullptr,
    const bool dual_display_requested = false
  ) {
    auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();

    launch_session->id = ++session_id_counter;

    auto rikey = util::from_hex_vec(get_arg(args, "rikey"), true);
    std::copy(rikey.cbegin(), rikey.cend(), std::back_inserter(launch_session->gcm_key));

    const auto requested_unique_id = get_arg(args, "uniqueid", "unknown");
    launch_session->unique_id = select_launch_client_identity(
      requested_unique_id,
      verified_device ? std::string_view {verified_device->uuid} : std::string_view {},
      dual_display_requested
    );
    const auto *named_device = dual_display_requested ? verified_device : find_named_device(requested_unique_id);
    if (dual_display_requested && verified_device) {
      launch_session->unique_id = verified_device->uuid;
      if (requested_unique_id != verified_device->uuid) {
        BOOST_LOG(info) << "SunshineSeat resolved paired client by verified certificate [name="sv
                        << (verified_device->name.empty() ? "unknown"sv : std::string_view {verified_device->name})
                        << ", requested_uniqueid="sv << requested_unique_id
                        << ", paired_uuid="sv << verified_device->uuid << ']';
      }
    } else if (!named_device && verified_device) {
      // Preserve the existing single-display fallback: it only replaces an
      // unknown requested UUID with the certificate-verified device.
      named_device = verified_device;
      launch_session->unique_id = verified_device->uuid;
      BOOST_LOG(info) << "SunshineSeat resolved paired client by verified certificate [name="sv
                      << (verified_device->name.empty() ? "unknown"sv : std::string_view {verified_device->name})
                      << ", requested_uniqueid="sv << requested_unique_id
                      << ", paired_uuid="sv << verified_device->uuid << ']';
    }
    if (verified_device) {
      // This is server-known paired-client state from certificate verification.
      // Do not substitute requested_unique_id: it is launch-request input.
      launch_session->authenticated_client_identity = verified_device->uuid;
    }

    launch_session->host_audio = host_audio;
    const auto requested_mode = get_arg(args, "mode", "0x0x0");
    if (named_device && !named_device->display_mode.empty()) {
      parse_mode_string(named_device->display_mode, launch_session->width, launch_session->height, launch_session->fps);
      BOOST_LOG(info) << "SunshineSeat using Apollo display mode override for client ["sv << named_device->name << "]: "sv << named_device->display_mode;
    } else {
      parse_mode_string(requested_mode, launch_session->width, launch_session->height, launch_session->fps);
      if (named_device) {
        BOOST_LOG(info) << "SunshineSeat using requested display mode for client ["sv << named_device->name << "]: "sv << requested_mode;
      }
    }

    if (named_device) {
      launch_session->device_name = named_device->name.empty() ? "ApolloDisplay"s : named_device->name;
      launch_session->perm = config::sunshineseat.apollo_permissions_enabled ? static_cast<crypto::PERM>(named_device->apollo_perm) & crypto::PERM::_all : crypto::PERM::_all;
      launch_session->virtual_display = util::from_view(get_arg(args, "virtualDisplay", "0")) || named_device->always_use_virtual_display;
      launch_session->client_do_cmds = config::sunshineseat.apollo_client_commands_enabled ? named_device->do_cmds : std::list<crypto::command_entry_t> {};
      launch_session->client_undo_cmds = config::sunshineseat.apollo_client_commands_enabled ? named_device->undo_cmds : std::list<crypto::command_entry_t> {};
    } else {
      launch_session->device_name = "SunshineSeat Client"s;
      launch_session->perm = crypto::PERM::_all;
      launch_session->virtual_display = util::from_view(get_arg(args, "virtualDisplay", "0"));
    }

    launch_session->scale_factor = (std::uint32_t) util::from_view(get_arg(args, "scaleFactor", "100"));
    launch_session->input_only = config::sunshineseat.apollo_input_only_enabled && util::from_view(get_arg(args, "inputOnly", "0"));
    launch_session->appid = (int) util::from_view(get_arg(args, "appid", "unknown"));
    launch_session->enable_sops = util::from_view(get_arg(args, "sops", "0"));
    launch_session->surround_info = (int) util::from_view(get_arg(args, "surroundAudioInfo", "196610"));
    launch_session->surround_params = (get_arg(args, "surroundParams", ""));
    launch_session->continuous_audio = util::from_view(get_arg(args, "continuousAudio", "0"));
    launch_session->gcmap = (int) util::from_view(get_arg(args, "gcmap", "0"));
    launch_session->enable_hdr = util::from_view(get_arg(args, "hdrMode", "0"));
    // Encrypted RTSP is enabled with client reported corever >= 1
    auto corever = util::from_view(get_arg(args, "corever", "0"));
    if (corever >= 1) {
      launch_session->rtsp_cipher = crypto::cipher::gcm_t {
        launch_session->gcm_key,
        false
      };
      launch_session->rtsp_iv_counter = 0;
    }
    launch_session->rtsp_url_scheme = launch_session->rtsp_cipher ? "rtspenc://"s : "rtsp://"s;
    launch_session->client_cert = last_verified_client_cert;

    // Generate the unique identifiers for this connection that we will send later during RTSP handshake
    unsigned char raw_payload[8];
    RAND_bytes(raw_payload, sizeof(raw_payload));
    launch_session->av_ping_payload = util::hex_vec(raw_payload);
    RAND_bytes((unsigned char *) &launch_session->control_connect_data, sizeof(launch_session->control_connect_data));

    launch_session->iv.resize(16);
    uint32_t prepend_iv = util::endian::big<uint32_t>((int) util::from_view(get_arg(args, "rikeyid")));
    auto prepend_iv_p = (uint8_t *) &prepend_iv;
    std::copy(prepend_iv_p, prepend_iv_p + sizeof(prepend_iv), std::begin(launch_session->iv));
    return launch_session;
  }

  std::optional<dual_display_launch::request_t> parse_dual_display_request(
    const args_t &args,
    std::string &failure
  ) {
    failure.clear();
    const auto range = args.equal_range("dualDisplayRequest");
    if (range.first == range.second) {
      return std::nullopt;
    }
    if (std::distance(range.first, range.second) != 1) {
      failure = "duplicate_dual_display_request";
      return std::nullopt;
    }

    const auto request = dual_display_launch::parse_optional_request(true, range.first->second, failure);
    if (!request) {
      return std::nullopt;
    }
    return request;
  }

  dual_display_launch::prepared_pair_ptr prepare_dual_display_pair_after_gates(
    const std::optional<dual_display_launch::request_t> &request,
    const dual_display_allocation_context_t &context,
    const dual_display_launch::prepared_pair_provider_t &provider,
    std::string &failure
  ) {
    failure.clear();
    if (!request) {
      return {};
    }
    if (const auto request_failure = dual_display_launch::validate_request(*request)) {
      failure = *request_failure;
      return {};
    }
    if (!context.no_active_sessions) {
      failure = "dual_display_active_session_conflict";
      return {};
    }
    if (!context.no_pending_launch_sessions) {
      failure = "dual_display_pending_session_conflict";
      return {};
    }
    if (context.input_only) {
      failure = "dual_display_input_only_unsupported";
      return {};
    }
    if (!context.permission_granted) {
      failure = "dual_display_permission_denied";
      return {};
    }
    if (context.authenticated_client_identity.empty()) {
      failure = "dual_display_authenticated_client_identity_unavailable";
      return {};
    }
    if (!context.encoder_supports_dual_display_capture) {
      // Do not call the provider: a software/non-D3D11 profile cannot consume
      // an exclusive dual virtual pair, and falling back to physical capture
      // would violate the authenticated paired-display contract.
      failure = "dual_display_requires_d3d11_encoder";
      return {};
    }
    if (!provider) {
      failure = "dual_display_provider_unavailable";
      return {};
    }

    auto pair = provider(*request, context.authenticated_client_identity, failure);
    if (!pair) {
      if (failure.empty()) {
        failure = "dual_display_provider_unavailable";
      }
      return {};
    }
    if (pair->owner_identity != context.authenticated_client_identity) {
      failure = "dual_display_owner_identity_mismatch";
      return {};
    }
    if (const auto pair_failure = dual_display_launch::validate_prepared_pair(*pair)) {
      failure = *pair_failure;
      return {};
    }
    if (!dual_display_launch::prepared_pair_matches_request(*pair, *request)) {
      failure = "prepared_pair_request_mismatch";
      return {};
    }
    return pair;
  }

  void set_dual_display_failure(pt::ptree &tree, std::string_view failure) {
    tree.put("root.resume", 0);
    tree.put("root.gamesession", 0);
    const auto status_code = failure == "dual_display_permission_denied" ? 403 : failure == "dual_display_provider_unavailable" || failure == "dual_display_platform_unsupported" || failure == "dual_display_pending_session_conflict" ? 503 :
                                                                                                                                                                                                                                          400;
    const auto failure_code = std::string {failure};
    BOOST_LOG(warning)
      << "Dual-display launch rejected [status_code=" << status_code
      << ", failure=" << failure_code << ']';
    tree.put("root.<xmlattr>.status_code", status_code);
    tree.put(
      "root.<xmlattr>.status_message",
      status_code == 503 ? "Dual-display host provider is unavailable" : status_code == 403 ? "This client is not permitted to launch a dual-display stream" :
                                                                                              "Malformed or unsupported dual-display launch request (" + failure_code + ')'
    );
  }

  void remove_session(const pair_session_t &sess) {
    map_id_sess.erase(sess.client.uniqueID);
  }

  void fail_pair(pair_session_t &sess, pt::ptree &tree, const std::string status_msg) {
    tree.put("root.paired", 0);
    tree.put("root.<xmlattr>.status_code", 400);
    tree.put("root.<xmlattr>.status_message", status_msg);
    remove_session(sess);  // Security measure, delete the session when something went wrong and force a re-pair
  }

  void getservercert(pair_session_t &sess, pt::ptree &tree) {
    if (sess.last_phase != PAIR_PHASE::NONE) {
      fail_pair(sess, tree, "Out of order call to getservercert");
      return;
    }
    sess.last_phase = PAIR_PHASE::GETSERVERCERT;

    if (!sess.password_mode) {
      fail_pair(sess, tree, "Password pairing is required");
      return;
    }

    if (sess.session_salt.size() != 16) {
      fail_pair(sess, tree, "Invalid pairing session salt");
      return;
    }

    if (!pairing_password_configured()) {
      fail_pair(sess, tree, "Pairing password is not configured");
      return;
    }

    const auto verifier = util::from_hex_vec(config::sunshine.pairing_password_verifier, true);
    const auto key = crypto::derive_pairing_session_key(sess.session_salt, verifier);
    if (key.empty()) {
      fail_pair(sess, tree, "Unable to derive pairing session key");
      return;
    }
    sess.cipher_key = std::make_unique<crypto::aes_t>(key);

    tree.put("root.paired", 1);
    tree.put("root.plaincert", util::hex_vec(conf_intern.servercert, true));
    tree.put("root.pairingsalt", config::sunshine.pairing_password_salt);
    tree.put("root.pairingiterations", config::sunshine.pairing_password_iterations);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void clientchallenge(pair_session_t &sess, pt::ptree &tree, const std::string &challenge) {
    if (sess.last_phase != PAIR_PHASE::GETSERVERCERT) {
      fail_pair(sess, tree, "Out of order call to clientchallenge");
      return;
    }
    sess.last_phase = PAIR_PHASE::CLIENTCHALLENGE;

    if (!sess.cipher_key) {
      fail_pair(sess, tree, "Cipher key not set");
      return;
    }
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    std::vector<uint8_t> decrypted;
    cipher.decrypt(challenge, decrypted);

    auto x509 = crypto::x509(conf_intern.servercert);
    auto sign = crypto::signature(x509);
    auto serversecret = crypto::rand(16);

    decrypted.insert(std::end(decrypted), std::begin(sign), std::end(sign));
    decrypted.insert(std::end(decrypted), std::begin(serversecret), std::end(serversecret));

    auto hash = crypto::hash({(char *) decrypted.data(), decrypted.size()});
    auto serverchallenge = crypto::rand(16);

    std::string plaintext;
    plaintext.reserve(hash.size() + serverchallenge.size());

    plaintext.insert(std::end(plaintext), std::begin(hash), std::end(hash));
    plaintext.insert(std::end(plaintext), std::begin(serverchallenge), std::end(serverchallenge));

    std::vector<uint8_t> encrypted;
    cipher.encrypt(plaintext, encrypted);

    sess.serversecret = std::move(serversecret);
    sess.serverchallenge = std::move(serverchallenge);

    tree.put("root.paired", 1);
    tree.put("root.challengeresponse", util::hex_vec(encrypted, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void serverchallengeresp(pair_session_t &sess, pt::ptree &tree, const std::string &encrypted_response) {
    if (sess.last_phase != PAIR_PHASE::CLIENTCHALLENGE) {
      fail_pair(sess, tree, "Out of order call to serverchallengeresp");
      return;
    }
    sess.last_phase = PAIR_PHASE::SERVERCHALLENGERESP;

    if (!sess.cipher_key || sess.serversecret.empty()) {
      fail_pair(sess, tree, "Cipher key or serversecret not set");
      return;
    }

    std::vector<uint8_t> decrypted;
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    cipher.decrypt(encrypted_response, decrypted);

    sess.clienthash = std::move(decrypted);

    auto serversecret = sess.serversecret;
    auto sign = crypto::sign256(crypto::pkey(conf_intern.pkey), serversecret);

    serversecret.insert(std::end(serversecret), std::begin(sign), std::end(sign));

    tree.put("root.pairingsecret", util::hex_vec(serversecret, true));
    tree.put("root.paired", 1);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void clientpairingsecret(pair_session_t &sess, std::shared_ptr<safe::queue_t<crypto::x509_t>> &add_cert, pt::ptree &tree, const std::string &client_pairing_secret) {
    if (sess.last_phase != PAIR_PHASE::SERVERCHALLENGERESP) {
      fail_pair(sess, tree, "Out of order call to clientpairingsecret");
      return;
    }
    sess.last_phase = PAIR_PHASE::CLIENTPAIRINGSECRET;

    auto &client = sess.client;

    if (client_pairing_secret.size() <= 16) {
      fail_pair(sess, tree, "Client pairing secret too short");
      return;
    }

    std::string_view secret {client_pairing_secret.data(), 16};
    std::string_view sign {client_pairing_secret.data() + secret.size(), client_pairing_secret.size() - secret.size()};

    auto x509 = crypto::x509(client.cert);
    if (!x509) {
      fail_pair(sess, tree, "Invalid client certificate");
      return;
    }
    auto x509_sign = crypto::signature(x509);

    std::string data;
    data.reserve(sess.serverchallenge.size() + x509_sign.size() + secret.size());

    data.insert(std::end(data), std::begin(sess.serverchallenge), std::end(sess.serverchallenge));
    data.insert(std::end(data), std::begin(x509_sign), std::end(x509_sign));
    data.insert(std::end(data), std::begin(secret), std::end(secret));

    auto hash = crypto::hash(data);

    // if hash not correct, probably MITM
    bool same_hash = hash.size() == sess.clienthash.size() && std::equal(hash.begin(), hash.end(), sess.clienthash.begin());
    auto verify = crypto::verify256(crypto::x509(client.cert), secret, sign);
    if (same_hash && verify) {
      tree.put("root.paired", 1);
      add_cert->raise(crypto::x509(client.cert));

      // The client is now successfully paired and will be authorized to connect
      add_authorized_client(client.name, std::move(client.cert));
    } else {
      tree.put("root.paired", 0);
    }

    remove_session(sess);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  template<class T>
  struct tunnel;

  template<>
  struct tunnel<SunshineHTTPS> {
    static auto constexpr to_string = "HTTPS"sv;
  };

  template<>
  struct tunnel<SimpleWeb::HTTP> {
    static auto constexpr to_string = "NONE"sv;
  };

  template<class T>
  void print_req(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    BOOST_LOG(debug) << "TUNNEL :: "sv << tunnel<T>::to_string;

    BOOST_LOG(debug) << "METHOD :: "sv << request->method;
    BOOST_LOG(debug) << "DESTINATION :: "sv << request->path;

    for (auto &[name, val] : request->header) {
      BOOST_LOG(debug) << name << " -- " << val;
    }

    BOOST_LOG(debug) << " [--] "sv;

    for (auto &[name, val] : request->parse_query_string()) {
      BOOST_LOG(debug) << name << " -- " << val;
    }

    BOOST_LOG(debug) << " [--] "sv;
  }

  template<class T>
  void not_found(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;
    tree.put("root.<xmlattr>.status_code", 404);

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());

    *response
      << "HTTP/1.1 404 NOT FOUND\r\n"
      << data.str();

    response->close_connection_after_response = true;
  }

  template<class T>
  void pair(std::shared_ptr<safe::queue_t<crypto::x509_t>> &add_cert, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;

    auto fg = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto args = request->parse_query_string();
    if (args.find("uniqueid"s) == std::end(args)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing uniqueid parameter");

      return;
    }

    auto uniqID {get_arg(args, "uniqueid")};

    args_t::const_iterator it;
    if (it = args.find("phrase"); it != std::end(args)) {
      if (it->second == "getservercert"sv) {
        if (get_arg(args, "pairingmode", "") != "password") {
          tree.put("root.paired", 0);
          tree.put("root.<xmlattr>.status_code", 400);
          tree.put("root.<xmlattr>.status_message", "Password pairing is required");
          return;
        }
        if (!pairing_password_configured()) {
          tree.put("root.paired", 0);
          tree.put("root.<xmlattr>.status_code", 400);
          tree.put("root.<xmlattr>.status_message", "Pairing password is not configured");
          return;
        }

        const auto session_salt_hex = get_arg(args, "salt", "");
        if (!valid_hex_string(session_salt_hex, 16)) {
          tree.put("root.paired", 0);
          tree.put("root.<xmlattr>.status_code", 400);
          tree.put("root.<xmlattr>.status_message", "Invalid pairing session salt");
          return;
        }

        const auto client_cert_hex = get_arg(args, "clientcert", "");
        if (!valid_hex_string(client_cert_hex)) {
          tree.put("root.paired", 0);
          tree.put("root.<xmlattr>.status_code", 400);
          tree.put("root.<xmlattr>.status_message", "Invalid client certificate");
          return;
        }

        const auto device_name = get_arg(args, "devicename", "");
        if (!valid_pairing_device_name(device_name)) {
          tree.put("root.paired", 0);
          tree.put("root.<xmlattr>.status_code", 400);
          tree.put("root.<xmlattr>.status_message", "Invalid device name");
          return;
        }

        pair_session_t sess;

        sess.client.uniqueID = std::move(uniqID);
        sess.client.cert = util::from_hex_vec(client_cert_hex, true);
        sess.client.name = device_name;
        sess.session_salt = util::from_hex_vec(session_salt_hex, true);
        sess.password_mode = true;

        BOOST_LOG(debug) << sess.client.cert;
        map_id_sess.erase(sess.client.uniqueID);
        auto ptr = map_id_sess.emplace(sess.client.uniqueID, std::move(sess)).first;
        getservercert(ptr->second, tree);
        return;
      } else if (it->second == "pairchallenge"sv) {
        tree.put("root.paired", 1);
        tree.put("root.<xmlattr>.status_code", 200);
        return;
      }
    }

    auto sess_it = map_id_sess.find(uniqID);
    if (sess_it == std::end(map_id_sess)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Invalid uniqueid");

      return;
    }

    if (it = args.find("clientchallenge"); it != std::end(args)) {
      auto challenge = util::from_hex_vec(it->second, true);
      clientchallenge(sess_it->second, tree, challenge);
    } else if (it = args.find("serverchallengeresp"); it != std::end(args)) {
      auto encrypted_response = util::from_hex_vec(it->second, true);
      serverchallengeresp(sess_it->second, tree, encrypted_response);
    } else if (it = args.find("clientpairingsecret"); it != std::end(args)) {
      auto pairingsecret = util::from_hex_vec(it->second, true);
      clientpairingsecret(sess_it->second, add_cert, tree, pairingsecret);
    } else {
      tree.put("root.<xmlattr>.status_code", 404);
      tree.put("root.<xmlattr>.status_message", "Invalid pairing request");
    }
  }

  template<class T>
  void serverinfo(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    int pair_status = 0;
    if constexpr (std::is_same_v<SunshineHTTPS, T>) {
      auto args = request->parse_query_string();
      auto clientID = args.find("uniqueid"s);

      if (clientID != std::end(args)) {
        pair_status = 1;
      }
    }

    auto local_endpoint = request->local_endpoint();

    pt::ptree tree;

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put("root.hostname", config::nvhttp.sunshine_name);

    tree.put("root.appversion", VERSION);
    tree.put("root.GfeVersion", GFE_VERSION);
    tree.put("root.uniqueid", http::unique_id);
    tree.put("root.HttpsPort", net::map_port(PORT_HTTPS));
    tree.put("root.ExternalPort", net::map_port(PORT_HTTP));
    tree.put("root.MaxLumaPixelsHEVC", video::active_hevc_mode > 1 ? "1869449984" : "0");

    // Only include the MAC address for requests sent from paired clients over HTTPS.
    // For HTTP requests, use a placeholder MAC address that Moonlight knows to ignore.
    if constexpr (std::is_same_v<SunshineHTTPS, T>) {
      tree.put("root.mac", platf::get_mac_address(net::addr_to_normalized_string(local_endpoint.address())));
      if (auto named_cert = get_verified_cert(request)) {
        tree.put("root.Permission", std::to_string(named_cert->apollo_perm));
        const auto perm = static_cast<crypto::PERM>(named_cert->apollo_perm) & crypto::PERM::_all;
        if (config::sunshineseat.apollo_client_commands_enabled && !!(perm & crypto::PERM::server_cmd)) {
          auto &root_node = tree.get_child("root");
          for (const auto &cmd : config::sunshine.server_cmds) {
            pt::ptree cmd_node;
            cmd_node.put_value(cmd.cmd_name);
            root_node.push_back(std::make_pair("ServerCommand", std::move(cmd_node)));
          }
        }
      }
    } else {
      tree.put("root.mac", "00:00:00:00:00:00");
      tree.put("root.Permission", "0");
    }

    // Moonlight clients track LAN IPv6 addresses separately from LocalIP which is expected to
    // always be an IPv4 address. If we return that same IPv6 address here, it will clobber the
    // stored LAN IPv4 address. To avoid this, we need to return an IPv4 address in this field
    // when we get a request over IPv6.
    //
    // HACK: We should return the IPv4 address of local interface here, but we don't currently
    // have that implemented. For now, we will emulate the behavior of GFE+GS-IPv6-Forwarder,
    // which returns 127.0.0.1 as LocalIP for IPv6 connections. Moonlight clients with IPv6
    // support know to ignore this bogus address.
    if (local_endpoint.address().is_v6() && !local_endpoint.address().to_v6().is_v4_mapped()) {
      tree.put("root.LocalIP", "127.0.0.1");
    } else {
      tree.put("root.LocalIP", net::addr_to_normalized_string(local_endpoint.address()));
    }

    uint32_t codec_mode_flags = SCM_H264;
    if (video::last_encoder_probe_supported_yuv444_for_codec[0]) {
      codec_mode_flags |= SCM_H264_HIGH8_444;
    }
    if (video::active_hevc_mode >= 2) {
      codec_mode_flags |= SCM_HEVC;
      if (video::last_encoder_probe_supported_yuv444_for_codec[1]) {
        codec_mode_flags |= SCM_HEVC_REXT8_444;
      }
    }
    if (video::active_hevc_mode >= 3) {
      codec_mode_flags |= SCM_HEVC_MAIN10;
      if (video::last_encoder_probe_supported_yuv444_for_codec[1]) {
        codec_mode_flags |= SCM_HEVC_REXT10_444;
      }
    }
    if (video::active_av1_mode >= 2) {
      codec_mode_flags |= SCM_AV1_MAIN8;
      if (video::last_encoder_probe_supported_yuv444_for_codec[2]) {
        codec_mode_flags |= SCM_AV1_HIGH8_444;
      }
    }
    if (video::active_av1_mode >= 3) {
      codec_mode_flags |= SCM_AV1_MAIN10;
      if (video::last_encoder_probe_supported_yuv444_for_codec[2]) {
        codec_mode_flags |= SCM_AV1_HIGH10_444;
      }
    }
    tree.put("root.ServerCodecModeSupport", codec_mode_flags);

    if (!config::nvhttp.external_ip.empty()) {
      tree.put("root.ExternalIP", config::nvhttp.external_ip);
    }

    auto current_appid = proc::proc.running();
    const bool actively_streaming = rtsp_stream::session_count() > 0;
    if (!actively_streaming) {
      current_appid = 0;
    }
    tree.put("root.PairStatus", pair_status);
    tree.put("root.currentgame", current_appid);
    tree.put("root.state", actively_streaming ? "SUNSHINE_SERVER_BUSY" : "SUNSHINE_SERVER_FREE");

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());
    response->close_connection_after_response = true;
  }

  nlohmann::json get_all_clients() {
    nlohmann::json named_cert_nodes = nlohmann::json::array();
    client_t &client = client_root;
    for (auto &named_cert : client.named_devices) {
      nlohmann::json named_cert_node;
      named_cert_node["name"] = named_cert.name;
      named_cert_node["uuid"] = named_cert.uuid;
      named_cert_node["enabled"] = named_cert.enabled;
      named_cert_node["perm"] = named_cert.apollo_perm;
      named_cert_node["allow_client_commands"] = named_cert.allow_client_commands;
      named_cert_node["always_use_virtual_display"] = named_cert.always_use_virtual_display;
      named_cert_node["display_mode"] = named_cert.display_mode;
      named_cert_nodes.push_back(named_cert_node);
    }

    return named_cert_nodes;
  }

  void applist(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto &apps = tree.add_child("root", pt::ptree {});

    apps.put("<xmlattr>.status_code", 200);

    for (auto &proc : proc::proc.get_apps()) {
      pt::ptree app;

      app.put("IsHdrSupported"s, video::active_hevc_mode == 3 ? 1 : 0);
      app.put("AppTitle"s, proc.name);
      app.put("ID", proc.id);

      apps.push_back(std::make_pair("App", std::move(app)));
    }
  }

  void launch(bool &host_audio, resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
    bool revert_display_configuration {false};
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      if (tree.empty()) {
        BOOST_LOG(error) << EMPTY_PROPERTY_TREE_ERROR_MSG;
      }

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;

      if (revert_display_configuration) {
        display_device::revert_configuration();
      }
    });

    auto args = request->parse_query_string();
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args) ||
      args.find("localAudioPlayMode"s) == std::end(args) ||
      args.find("appid"s) == std::end(args)
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required launch parameter");

      return;
    }

    std::string dual_display_failure;
    const auto dual_display_request = parse_dual_display_request(args, dual_display_failure);
    if (!dual_display_failure.empty()) {
      set_dual_display_failure(tree, dual_display_failure);
      return;
    }

    auto appid = util::from_view(get_arg(args, "appid"));
    const bool is_input_only = config::input.enable_input_only_mode && appid == proc::input_only_app_id;

    const auto active_session_count = rtsp_stream::session_count();
    auto current_appid = proc::proc.running();
    if (should_reset_running_app_state_for_launch(current_appid, active_session_count, is_input_only)) {
      BOOST_LOG(warning) << "Resetting stale running app state before launch [appid="sv << current_appid << ']';
      proc::proc.terminate();
      current_appid = proc::proc.running();
    }

    if (current_appid > 0 && !is_input_only) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "An app is already running on this host");

      return;
    }

    host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    auto launch_session = make_launch_session(
      host_audio,
      args,
      get_verified_cert(request),
      dual_display_request.has_value()
    );
    launch_session->remote_address = net::addr_to_normalized_string(request->remote_endpoint().address());
    launch_session->input_only = launch_session->input_only || is_input_only;
    const bool launch_permission_granted = !config::sunshineseat.apollo_permissions_enabled ||
                                           !(!(launch_session->perm & crypto::PERM::launch));
    if (!launch_permission_granted) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "This client is not permitted to launch apps");
      tree.put("root.gamesession", 0);
      return;
    }

    auto dual_display_pair = prepare_dual_display_pair_after_gates(
      dual_display_request,
      {
        .no_active_sessions = active_session_count == 0,
        .no_pending_launch_sessions = rtsp_stream::pending_launch_session_count() == 0,
        .input_only = launch_session->input_only,
        .permission_granted = launch_permission_granted,
        .encoder_supports_dual_display_capture = video::active_encoder_supports_dual_display_capture(),
        .authenticated_client_identity = launch_session->authenticated_client_identity,
      },
      get_dual_display_prepared_pair_provider(),
      dual_display_failure
    );
    if (!dual_display_failure.empty()) {
      set_dual_display_failure(tree, dual_display_failure);
      return;
    }
    launch_session->dual_display_pair = std::move(dual_display_pair);

    if (!is_input_only && rtsp_stream::session_count() == 0) {
      if (should_apply_legacy_display_configuration(launch_session->dual_display_pair)) {
        // The display should be restored in case something fails as there are no other sessions.
        revert_display_configuration = true;

        // We want to prepare display only if there are no active sessions at
        // the moment. This should be done before probing encoders as it could
        // change the active displays.
        display_device::configure_display(config::video, *launch_session);
      }

      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
      if (should_probe_encoders_after_display_preparation(static_cast<bool>(launch_session->dual_display_pair)) && video::probe_encoders()) {
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Failed to initialize video capture/encoding. Is a display connected and turned on?");
        tree.put("root.gamesession", 0);

        return;
      }
    }

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    if (appid > 0) {
      int err = 0;
      if (is_input_only) {
        proc::proc.launch_input_only();
      } else {
        err = proc::proc.execute((int) appid, launch_session);
      }
      if (err) {
        tree.put("root.<xmlattr>.status_code", err);
        tree.put("root.<xmlattr>.status_message", "Failed to start the specified application");
        tree.put("root.gamesession", 0);

        return;
      }
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put(
      "root.sessionUrl0",
      std::format(
        "{}{}:{}",
        launch_session->rtsp_url_scheme,
        rtsp_session_host(request),
        static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
      )
    );
    tree.put("root.gamesession", 1);
    if (launch_session->dual_display_pair) {
      tree.put("root.dualDisplayManifest", dual_display_launch::serialize_manifest(launch_session->dual_display_pair->manifest));
    }

    rtsp_stream::launch_session_raise(launch_session);

    // Stream was started successfully, we will revert the config when the app or session terminates
    revert_display_configuration = false;
  }

  void resume(bool &host_audio, resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      if (tree.empty()) {
        BOOST_LOG(error) << EMPTY_PROPERTY_TREE_ERROR_MSG;
      }

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto current_appid = proc::proc.running();
    if (current_appid == 0) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 503);
      tree.put("root.<xmlattr>.status_message", "No running app to resume");

      return;
    }

    auto args = request->parse_query_string();
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args)
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required resume parameter");

      return;
    }

    std::string dual_display_failure;
    const auto dual_display_request = parse_dual_display_request(args, dual_display_failure);
    if (!dual_display_failure.empty()) {
      set_dual_display_failure(tree, dual_display_failure);
      return;
    }

    // Newer Moonlight clients send localAudioPlayMode on /resume too,
    // so we should use it if it's present in the args and there are
    // no active sessions we could be interfering with.
    const bool no_active_sessions {rtsp_stream::session_count() == 0};
    if (no_active_sessions && args.find("localAudioPlayMode"s) != std::end(args)) {
      host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    }
    const auto launch_session = make_launch_session(
      host_audio,
      args,
      get_verified_cert(request),
      dual_display_request.has_value()
    );
    launch_session->remote_address = net::addr_to_normalized_string(request->remote_endpoint().address());
    const bool view_permission_granted = !config::sunshineseat.apollo_permissions_enabled ||
                                         !(!(launch_session->perm & crypto::PERM::view));
    if (!view_permission_granted) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "This client is not permitted to resume streams");
      return;
    }

    auto dual_display_pair = prepare_dual_display_pair_after_gates(
      dual_display_request,
      {
        .no_active_sessions = no_active_sessions,
        .no_pending_launch_sessions = rtsp_stream::pending_launch_session_count() == 0,
        .input_only = launch_session->input_only,
        .permission_granted = view_permission_granted,
        .encoder_supports_dual_display_capture = video::active_encoder_supports_dual_display_capture(),
        .authenticated_client_identity = launch_session->authenticated_client_identity,
      },
      get_dual_display_prepared_pair_provider(),
      dual_display_failure
    );
    if (!dual_display_failure.empty()) {
      set_dual_display_failure(tree, dual_display_failure);
      return;
    }
    launch_session->dual_display_pair = std::move(dual_display_pair);

    if (no_active_sessions) {
      if (should_apply_legacy_display_configuration(launch_session->dual_display_pair)) {
        // We want to prepare display only if there are no active sessions at
        // the moment. This should be done before probing encoders as it could
        // change the active displays.
        display_device::configure_display(config::video, *launch_session);
      }

      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
      if (should_probe_encoders_after_display_preparation(static_cast<bool>(launch_session->dual_display_pair)) && video::probe_encoders()) {
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Failed to initialize video capture/encoding. Is a display connected and turned on?");

        return;
      }
    }

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put(
      "root.sessionUrl0",
      std::format(
        "{}{}:{}",
        launch_session->rtsp_url_scheme,
        rtsp_session_host(request),
        static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
      )
    );
    tree.put("root.resume", 1);
    if (launch_session->dual_display_pair) {
      tree.put("root.dualDisplayManifest", dual_display_launch::serialize_manifest(launch_session->dual_display_pair->manifest));
    }

    rtsp_stream::launch_session_raise(launch_session);
  }

  void cancel(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    tree.put("root.cancel", 1);
    tree.put("root.<xmlattr>.status_code", 200);

    rtsp_stream::terminate_sessions();

    if (proc::proc.running() > 0) {
      proc::proc.terminate();
    }

    // The config needs to be reverted regardless of whether "proc::proc.terminate()" was called or not.
    display_device::revert_configuration();
  }

  void appasset(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    auto args = request->parse_query_string();
    auto app_image = proc::proc.get_app_image((int) util::from_view(get_arg(args, "appid")));

    std::ifstream in(app_image, std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
    response->close_connection_after_response = true;
  }

  bool client_has_connected_session(const named_cert_t &named_cert) {
    auto connected_uuids = rtsp_stream::get_all_session_uuids();
    return std::find(std::begin(connected_uuids), std::end(connected_uuids), named_cert.uuid) != std::end(connected_uuids);
  }

  bool should_reset_running_app_state_for_launch(int current_appid, int active_session_count, bool is_input_only) {
    return current_appid > 0 && active_session_count == 0 && !is_input_only;
  }

  void getClipboard(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    auto named_cert = get_verified_cert(request);
    if (!config::sunshineseat.apollo_clipboard_enabled || !named_cert) {
      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      response->close_connection_after_response = true;
      return;
    }

    const auto perm = static_cast<crypto::PERM>(named_cert->apollo_perm) & crypto::PERM::_all;
    if (!(perm & crypto::PERM::_allow_view) || !(perm & crypto::PERM::clipboard_read)) {
      BOOST_LOG(debug) << "Permission Read Clipboard denied for ["sv << named_cert->name << ']';
      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      response->close_connection_after_response = true;
      return;
    }

    auto args = request->parse_query_string();
    if (get_arg(args, "type", "") != "text"sv) {
      response->write(SimpleWeb::StatusCode::client_error_bad_request);
      response->close_connection_after_response = true;
      return;
    }

    if (!client_has_connected_session(*named_cert)) {
      BOOST_LOG(debug) << "Client ["sv << named_cert->name << "] tried to read clipboard without an active stream"sv;
      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      response->close_connection_after_response = true;
      return;
    }

    response->write(platf::get_clipboard());
  }

  void setClipboard(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    auto named_cert = get_verified_cert(request);
    if (!config::sunshineseat.apollo_clipboard_enabled || !named_cert) {
      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      response->close_connection_after_response = true;
      return;
    }

    const auto perm = static_cast<crypto::PERM>(named_cert->apollo_perm) & crypto::PERM::_all;
    if (!(perm & crypto::PERM::_allow_view) || !(perm & crypto::PERM::clipboard_set)) {
      BOOST_LOG(debug) << "Permission Write Clipboard denied for ["sv << named_cert->name << ']';
      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      response->close_connection_after_response = true;
      return;
    }

    auto args = request->parse_query_string();
    if (get_arg(args, "type", "") != "text"sv) {
      response->write(SimpleWeb::StatusCode::client_error_bad_request);
      response->close_connection_after_response = true;
      return;
    }

    if (!client_has_connected_session(*named_cert)) {
      BOOST_LOG(debug) << "Client ["sv << named_cert->name << "] tried to write clipboard without an active stream"sv;
      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      response->close_connection_after_response = true;
      return;
    }

    if (!platf::set_clipboard(request->content.string())) {
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error);
      response->close_connection_after_response = true;
      return;
    }

    response->write();
  }

  void setup(const std::string &pkey, const std::string &cert) {
    conf_intern.pkey = pkey;
    conf_intern.servercert = cert;
  }

  bool is_client_enabled(const std::string_view cert_pem);

  void start() {
    platf::set_thread_name("nvhttp");
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    if (!get_dual_display_prepared_pair_provider()) {
      set_dual_display_prepared_pair_provider(
        [](const dual_display_launch::request_t &request, const std::string_view authenticated_client_identity, std::string &failure) {
          return platf::prepare_dual_virtual_display_pair(request, authenticated_client_identity, failure);
        }
      );
    }

    auto port_http = net::map_port(PORT_HTTP);
    auto port_https = net::map_port(PORT_HTTPS);
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);

    bool clean_slate = config::sunshine.flags[config::flag::FRESH_STATE];

    if (!clean_slate) {
      load_state();
    }

    auto pkey = file_handler::read_file(config::nvhttp.pkey.c_str());
    auto cert = file_handler::read_file(config::nvhttp.cert.c_str());
    setup(pkey, cert);

    auto add_cert = std::make_shared<safe::queue_t<crypto::x509_t>>(30);

    // resume doesn't always get the parameter "localAudioPlayMode"
    // launch will store it in host_audio
    bool host_audio {};

    https_server_t https_server {config::nvhttp.cert, config::nvhttp.pkey};
    http_server_t http_server;

    // Verify certificates after establishing connection
    https_server.verify = [add_cert](SSL *ssl) -> p_named_cert_t {
      crypto::x509_t x509 {
#if OPENSSL_VERSION_MAJOR >= 3
        SSL_get1_peer_certificate(ssl)
#else
        SSL_get_peer_certificate(ssl)
#endif
      };
      if (!x509) {
        BOOST_LOG(info) << "unknown -- denied"sv;
        return nullptr;
      }

      p_named_cert_t verified_client;

      auto fg = util::fail_guard([&]() {
        char subject_name[256];

        X509_NAME_oneline(X509_get_subject_name(x509.get()), subject_name, sizeof(subject_name));

        BOOST_LOG(debug) << subject_name << " -- "sv << (verified_client ? "verified"sv : "denied"sv);
      });

      while (add_cert->peek()) {
        char subject_name[256];

        auto cert = add_cert->pop();
        X509_NAME_oneline(X509_get_subject_name(cert.get()), subject_name, sizeof(subject_name));

        BOOST_LOG(debug) << "Added cert ["sv << subject_name << ']';
        cert_chain.add(std::move(cert));
      }

      auto err_str = cert_chain.verify(x509.get());
      if (err_str) {
        BOOST_LOG(warning) << "SSL Verification error :: "sv << err_str;

        return nullptr;
      }

      // Check if this client is enabled
      auto pem = crypto::pem(x509);
      verified_client = find_named_device_by_cert(pem);
      if (!verified_client || !verified_client->enabled) {
        BOOST_LOG(info) << "Client is disabled -- denied"sv;
        return nullptr;
      }

      last_verified_client_cert = pem;
      return verified_client;
    };

    https_server.on_verify_failed = [](resp_https_t resp, req_https_t req) {
      pt::ptree tree;
      auto g = util::fail_guard([&]() {
        std::ostringstream data;

        pt::write_xml(data, tree);
        resp->write(data.str());
        resp->close_connection_after_response = true;
      });

      tree.put("root.<xmlattr>.status_code"s, 401);
      tree.put("root.<xmlattr>.query"s, req->path);
      tree.put("root.<xmlattr>.status_message"s, "The client is not authorized. Certificate verification failed."s);
    };

    https_server.default_resource["GET"] = not_found<SunshineHTTPS>;
    https_server.resource["^/serverinfo$"]["GET"] = serverinfo<SunshineHTTPS>;
    https_server.resource["^/pair$"]["GET"] = [&add_cert](auto resp, auto req) {
      pair<SunshineHTTPS>(add_cert, resp, req);
    };
    https_server.resource["^/applist$"]["GET"] = applist;
    https_server.resource["^/appasset$"]["GET"] = appasset;
    https_server.resource["^/launch$"]["GET"] = [&host_audio](auto resp, auto req) {
      launch(host_audio, resp, req);
    };
    https_server.resource["^/resume$"]["GET"] = [&host_audio](auto resp, auto req) {
      resume(host_audio, resp, req);
    };
    https_server.resource["^/cancel$"]["GET"] = cancel;
    https_server.resource["^/actions/clipboard$"]["GET"] = getClipboard;
    https_server.resource["^/actions/clipboard$"]["POST"] = setClipboard;
    https_server.resource["^/networktest/v1/capabilities$"]["GET"] = networktest_capabilities;
    https_server.resource["^/networktest/v1/route-jobs$"]["POST"] = networktest_create_route_job;
    https_server.resource["^/networktest/v1/route-jobs/([^/]+)/ping$"]["GET"] = networktest_ping_route_job;
    https_server.resource["^/networktest/v1/route-jobs/([^/]+)/download$"]["GET"] = networktest_download_route_job;
    https_server.resource["^/networktest/v1/route-jobs/([^/]+)/upload$"]["POST"] = networktest_upload_route_job;
    https_server.resource["^/networktest/v1/route-jobs/([^/]+)$"]["DELETE"] = networktest_cancel_route_job;
    https_server.resource["^/networktest/v1/host-public-jobs$"]["POST"] = networktest_create_host_public_job;
    https_server.resource["^/networktest/v1/host-public-jobs/([^/]+)$"]["GET"] = networktest_host_public_job_status;
    https_server.resource["^/networktest/v1/host-public-jobs/([^/]+)$"]["DELETE"] = networktest_cancel_host_public_job;
    https_server.resource["^/networktest/v1/client-results$"]["POST"] = networktest_submit_client_result;
    https_server.resource["^/networktest/v1/results/latest$"]["GET"] = networktest_latest_results;

    https_server.config.reuse_address = true;
    https_server.config.address = net::get_bind_address(address_family);
    https_server.config.port = port_https;

    http_server.default_resource["GET"] = not_found<SimpleWeb::HTTP>;
    http_server.resource["^/serverinfo$"]["GET"] = serverinfo<SimpleWeb::HTTP>;
    http_server.resource["^/pair$"]["GET"] = [&add_cert](auto resp, auto req) {
      pair<SimpleWeb::HTTP>(add_cert, resp, req);
    };

    http_server.config.reuse_address = true;
    http_server.config.address = net::get_bind_address(address_family);
    http_server.config.port = port_http;

    auto accept_and_run = [&](auto *http_server) {
      try {
        std::string name = "nvhttp::" + std::to_string(http_server->config.port);
        platf::set_thread_name(name);
        http_server->start();
      } catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling http_server->stop() from a different thread
        if (shutdown_event->peek()) {
          return;
        }

        BOOST_LOG(fatal) << "Couldn't start http server on ports ["sv << port_https << ", "sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    std::thread ssl {accept_and_run, &https_server};
    std::thread tcp {accept_and_run, &http_server};

    // Wait for any event
    shutdown_event->view();

    https_server.stop();
    http_server.stop();

    ssl.join();
    tcp.join();
  }

  void erase_all_clients() {
    client_t client;
    client_root = client;
    cert_chain.clear();
    save_state();
    network_test_jobs().remove_all_paired_clients();
  }

  bool unpair_client(const std::string_view uuid) {
    bool removed = false;
    client_t &client = client_root;
    for (auto it = client.named_devices.begin(); it != client.named_devices.end();) {
      if ((*it).uuid == uuid) {
        it = client.named_devices.erase(it);
        removed = true;
      } else {
        ++it;
      }
    }

    save_state();
    load_state();
    if (removed) {
      network_test_jobs().remove_paired_client(uuid);
    }
    return removed;
  }

  bool set_client_enabled(const std::string_view uuid, bool enabled) {
    client_t &client = client_root;
    for (auto &named_cert : client.named_devices) {
      if (named_cert.uuid == uuid) {
        named_cert.enabled = enabled;
        save_state();
        return true;
      }
    }
    return false;
  }

  std::string get_cert_by_uuid(const std::string_view uuid) {
    for (const auto &named_cert : client_root.named_devices) {
      if (named_cert.uuid == uuid) {
        return named_cert.cert;
      }
    }
    return {};
  }

  bool is_client_enabled(const std::string_view cert_pem) {
    const client_t &client = client_root;
    for (const auto &named_cert : client.named_devices) {
      if (named_cert.cert == cert_pem) {
        return named_cert.enabled;
      }
    }
    return true;
  }
}  // namespace nvhttp
