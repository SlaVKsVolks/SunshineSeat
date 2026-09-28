#include "src/platform/windows/sudovda_dual_display.h"

#include <cstdlib>
#include <iostream>

namespace {

  [[noreturn]] void fail(const char *message) {
    std::cerr << "sudovda-pair-identity-contract: FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }

}

int main() {
  using namespace platf::sudovda::dual;

  display_pair_request_t request;
  request.session_generation = 9;
  request.display_pair_id = "pair-9";
  request.logical_display_ids = {"pane-a", "pane-b"};

  const std::array<display_pair_pane_t, display_pair_size> provisional {{
    {"pane-a", "host-a", "provisional-provider-name"},
    {"pane-b", "host-b", "provisional-provider-name"},
  }};
  if (!provisional_pair_identity_matches(request, provisional)) {
    fail("distinct host identities were rejected because provider names had not remapped yet");
  }

  auto duplicate_host = provisional;
  duplicate_host[1].host_display_identity = duplicate_host[0].host_display_identity;
  if (provisional_pair_identity_matches(request, duplicate_host)) {
    fail("duplicate host identities were accepted before topology commit");
  }

  auto wrong_logical = provisional;
  wrong_logical[1].logical_display_id = "unexpected";
  if (provisional_pair_identity_matches(request, wrong_logical)) {
    fail("mismatched logical identity was accepted before topology commit");
  }

  std::cout << "sudovda-pair-identity-contract: PASS\n";
  return EXIT_SUCCESS;
}
