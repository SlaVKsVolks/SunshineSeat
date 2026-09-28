/**
 * @file tests/unit/test_audio.cpp
 * @brief Test src/audio.*.
 */
#include "../tests_common.h"

#include <src/audio.h>

using namespace audio;

TEST(AudioSeatPolicyTests, AllowsOnlyProvenSingleUserRouting) {
  EXPECT_TRUE(allows_audio_routing("sunshine"));
  EXPECT_FALSE(allows_audio_routing("isolated"));
  EXPECT_FALSE(allows_audio_routing("disabled"));
  EXPECT_FALSE(allows_audio_routing("unexpected"));
}

#ifdef _WIN32
#include <mmdeviceapi.h>

namespace platf::audio {
  bool should_skip_redundant_sink_assignment(
    std::string_view assigned_sink,
    std::string_view requested_sink,
    bool requested_device_is_default_for_all_roles
  );
  bool should_process_render_device_change(EDataFlow flow, ERole role);
  bool should_ignore_self_triggered_render_device_change(
    std::wstring_view expected_device_id,
    std::wstring_view observed_device_id
  );
}
#endif

struct AudioTest: PlatformTestSuite, testing::WithParamInterface<std::tuple<std::basic_string_view<char>, config_t>> {
  void SetUp() override {
    m_config = std::get<1>(GetParam());
    m_mail = std::make_shared<safe::mail_raw_t>();
  }

  config_t m_config;
  safe::mail_t m_mail;
};

constexpr std::bitset<config_t::MAX_FLAGS> config_flags(const int flag = -1) {
  auto result = std::bitset<config_t::MAX_FLAGS>();
  if (flag >= 0) {
    result.set(flag);
  }
  return result;
}

#ifdef _WIN32
TEST(AudioReliabilityTests, IgnoresSelfTriggeredVirtualSinkDefaultChange) {
  constexpr std::string_view sink = "virtual-Stereo";

  EXPECT_TRUE(platf::audio::should_skip_redundant_sink_assignment(sink, sink, true));
  EXPECT_FALSE(platf::audio::should_skip_redundant_sink_assignment(sink, sink, false));
  EXPECT_FALSE(platf::audio::should_skip_redundant_sink_assignment({}, sink, true));
  EXPECT_FALSE(platf::audio::should_skip_redundant_sink_assignment(sink, "virtual-Surround", true));
}

TEST(AudioReliabilityTests, TracksOnlyConsoleRenderDeviceChanges) {
  EXPECT_TRUE(platf::audio::should_process_render_device_change(eRender, eConsole));
  EXPECT_FALSE(platf::audio::should_process_render_device_change(eCapture, eConsole));
  EXPECT_FALSE(platf::audio::should_process_render_device_change(eRender, eMultimedia));
  EXPECT_FALSE(platf::audio::should_process_render_device_change(eRender, eCommunications));
}

TEST(AudioReliabilityTests, IgnoresOnlyExpectedSelfTriggeredRenderDeviceChange) {
  constexpr std::wstring_view expected = L"{expected-device}";

  EXPECT_TRUE(platf::audio::should_ignore_self_triggered_render_device_change(expected, expected));
  EXPECT_FALSE(platf::audio::should_ignore_self_triggered_render_device_change(expected, L"{external-device}"));
  EXPECT_FALSE(platf::audio::should_ignore_self_triggered_render_device_change(expected, {}));
  EXPECT_FALSE(platf::audio::should_ignore_self_triggered_render_device_change({}, expected));
}
#endif

INSTANTIATE_TEST_SUITE_P(
  Configurations,
  AudioTest,
  testing::Values(
    std::make_tuple("HIGH_STEREO", config_t {5, 2, 0x3, {0}, config_flags(config_t::HIGH_QUALITY)}),
    std::make_tuple("SURROUND51", config_t {5, 6, 0x3F, {0}, config_flags()}),
    std::make_tuple("SURROUND71", config_t {5, 8, 0x63F, {0}, config_flags()}),
    std::make_tuple("SURROUND51_CUSTOM", config_t {5, 6, 0x3F, {6, 4, 2, {0, 1, 4, 5, 2, 3}}, config_flags(config_t::CUSTOM_SURROUND_PARAMS)})
  ),
  [](const auto &info) {
    return std::string(std::get<0>(info.param));
  }
);

TEST_P(AudioTest, TestEncode) {
  std::thread timer([&] {
    // Terminate the audio capture after 100 ms
    std::this_thread::sleep_for(100ms);
    const auto shutdown_event = m_mail->event<bool>(mail::shutdown);
    const auto audio_packets = m_mail->queue<packet_t>(mail::audio_packets);
    shutdown_event->raise(true);
    audio_packets->stop();
  });
  std::thread capture([&] {
    const auto packets = m_mail->queue<packet_t>(mail::audio_packets);
    const auto shutdown_event = m_mail->event<bool>(mail::shutdown);
    while (const auto packet = packets->pop()) {
      if (shutdown_event->peek()) {
        break;
      }
      if (auto packet_data = packet->second; packet_data.size() == 0) {
        FAIL() << "Empty packet data";
      }
    }
  });
  audio::capture(m_mail, m_config, nullptr);

  timer.join();
  capture.join();
}
