#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

  [[noreturn]] void fail(const std::string_view expected) {
    std::cerr << "troubleshooting-network-test-ui-contract: FAIL: " << expected << '\n';
    std::exit(EXIT_FAILURE);
  }

  std::string read_file(const std::filesystem::path &path) {
    std::ifstream input {path, std::ios::binary};
    return {std::istreambuf_iterator<char> {input}, std::istreambuf_iterator<char> {}};
  }

  void require_contains(const std::string &content, const std::string_view expected) {
    if (content.find(expected) == std::string::npos) {
      fail(expected);
    }
  }

}  // namespace

int main() {
  const std::filesystem::path source_root {SUNSHINE_SOURCE_DIR};
  const auto page = read_file(source_root / "src_assets" / "common" / "assets" / "web" / "troubleshooting.html");
  const auto locale = read_file(source_root / "src_assets" / "common" / "assets" / "web" / "public" / "assets" / "locale" / "en.json");

  for (const auto expected : {
         "id=\"network-diagnostics\"",
         "aria-live=\"polite\"",
         "ref=\"networkTestStart\"",
         "@click=\"runHostNetworkTest\"",
         "@click=\"cancelHostNetworkTest\"",
         "./api/network-test/v1/results",
         "./api/network-test/v1/host-public-jobs",
         "networkTest.hostPublicResult",
         "networkTest.clientResults",
       }) {
    require_contains(page, expected);
  }
  if (page.find("raw_ip") != std::string::npos) {
    fail("the troubleshooting UI rendered a raw network address");
  }
  for (const auto expected : {
         "\"network_diagnostics\"",
         "\"network_run_host_test\"",
         "\"network_cancel_test\"",
         "\"network_stream_active\"",
       }) {
    require_contains(locale, expected);
  }

  std::cout << "troubleshooting-network-test-ui-contract: PASS\n";
  return EXIT_SUCCESS;
}
