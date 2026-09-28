#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

  [[noreturn]] void fail(const std::string_view message) {
    std::cerr << "dual-idr-broadcaster-contract: FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }

  std::string read_source(const std::filesystem::path &path) {
    std::ifstream input {path, std::ios::binary};
    return {std::istreambuf_iterator<char> {input}, std::istreambuf_iterator<char> {}};
  }

}  // namespace

int main() {
  const std::filesystem::path source_root {SUNSHINE_SOURCE_DIR};
  const auto stream = read_source(source_root / "src" / "stream.cpp");
  const auto video = read_source(source_root / "src" / "video.cpp");
  const auto broadcaster_start = stream.find("void videoBroadcastThread");
  const auto broadcaster_end = stream.find("void audioBroadcastThread", broadcaster_start);
  if (broadcaster_start == std::string::npos || broadcaster_end == std::string::npos) {
    fail("video broadcaster boundaries");
  }

  const auto broadcaster = stream.substr(broadcaster_start, broadcaster_end - broadcaster_start);
  if (broadcaster.find("dual_idr") != std::string::npos ||
      broadcaster.find("last_dual_keyframe_track_index") != std::string::npos ||
      broadcaster.find("last_dual_keyframe_sent_at") != std::string::npos) {
    fail("dual-IDR timing state leaked into the shared broadcaster");
  }
  if (stream.find("last_dual_keyframe_track_index") != std::string::npos ||
      stream.find("last_dual_keyframe_sent_at") != std::string::npos) {
    fail("obsolete dual-keyframe broadcaster state was restored");
  }
  if (video.find("dual_idr_scheduler->claim_due_idr") == std::string::npos) {
    fail("encoder loop no longer owns due-IDR claims");
  }

  std::cout << "dual-idr-broadcaster-contract: PASS\n";
  return EXIT_SUCCESS;
}
