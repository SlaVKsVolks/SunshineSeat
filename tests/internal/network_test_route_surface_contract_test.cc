#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

  [[noreturn]] void fail(const std::string_view message) {
    std::cerr << "network-test-route-surface-contract: FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }

  std::string read_source(const std::filesystem::path &path) {
    std::ifstream input {path, std::ios::binary};
    return {std::istreambuf_iterator<char> {input}, std::istreambuf_iterator<char> {}};
  }

  void require_contains(const std::string &source, const std::string_view expected) {
    if (source.find(expected) == std::string::npos) {
      fail(expected);
    }
  }

}  // namespace

int main() {
  const std::filesystem::path source_root {SUNSHINE_SOURCE_DIR};
  const auto nvhttp = read_source(source_root / "src" / "nvhttp.cpp");
  const auto confighttp = read_source(source_root / "src" / "confighttp.cpp");

  for (const auto route : {
         "^/networktest/v1/capabilities$",
         "^/networktest/v1/route-jobs$",
         "^/networktest/v1/route-jobs/([^/]+)/ping$",
         "^/networktest/v1/route-jobs/([^/]+)/download$",
         "^/networktest/v1/route-jobs/([^/]+)/upload$",
         "^/networktest/v1/route-jobs/([^/]+)$",
         "^/networktest/v1/host-public-jobs$",
         "^/networktest/v1/host-public-jobs/([^/]+)$",
         "^/networktest/v1/client-results$",
         "^/networktest/v1/results/latest$",
       }) {
    require_contains(nvhttp, route);
  }
  require_contains(nvhttp, "get_verified_cert(request)");
  require_contains(nvhttp, "https_server.resource[\"^/networktest/v1/capabilities$\"]");
  if (nvhttp.find("http_server.resource[\"^/networktest") != std::string::npos) {
    fail("network-test routes leaked onto the unauthenticated HTTP server");
  }

  require_contains(confighttp, "^/api/network-test/v1/results$");
  require_contains(confighttp, "^/api/network-test/v1/host-public-jobs$");
  require_contains(confighttp, "validate_csrf_token(response, request, client_id)");

  std::cout << "network-test-route-surface-contract: PASS\n";
  return EXIT_SUCCESS;
}
