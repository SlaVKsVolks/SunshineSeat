#include "../tests_common.h"

#include <tools/sunshineseat_agent_common.h>

namespace {
  TEST(SunshineSeatAgentCommonTest, SunshineSeatWebCodeTreatsSessionLoginResponsesAsHealthy) {
    EXPECT_TRUE(sunshineseat_agent::sunshineseat_web_code_is_healthy(200));
    EXPECT_TRUE(sunshineseat_agent::sunshineseat_web_code_is_healthy(307));
    EXPECT_TRUE(sunshineseat_agent::sunshineseat_web_code_is_healthy(401));
    EXPECT_FALSE(sunshineseat_agent::sunshineseat_web_code_is_healthy(500));
  }

  TEST(SunshineSeatAgentCommonTest, HealthProbeUsesUnauthenticatedGameStreamServerInfoEndpoint) {
    EXPECT_EQ(
      sunshineseat_agent::gamestream_serverinfo_url(15636),
      "http://127.0.0.1:15636/serverinfo"
    );
    EXPECT_EQ(
      sunshineseat_agent::gamestream_serverinfo_url(47989),
      "http://127.0.0.1:47989/serverinfo"
    );
  }

  TEST(SunshineSeatAgentCommonTest, MissingRemoteTcpAloneDoesNotMarkSessionStale) {
    const auto assessment = sunshineseat_agent::classify_stale_stream_session(
      1,
      true,
      121,
      false
    );

    EXPECT_FALSE(assessment.stale_session);
    EXPECT_EQ(assessment.reason, "none");
  }

  TEST(SunshineSeatAgentCommonTest, ActiveRemoteTcpPreventsFastStaleRecovery) {
    const auto assessment = sunshineseat_agent::classify_stale_stream_session(
      1,
      true,
      121,
      true
    );

    EXPECT_FALSE(assessment.stale_session);
    EXPECT_EQ(assessment.reason, "none");
  }

  TEST(SunshineSeatAgentCommonTest, RecentConnectWithoutRemoteTcpWaitsBeforeStaleRecovery) {
    const auto assessment = sunshineseat_agent::classify_stale_stream_session(
      1,
      true,
      30,
      false
    );

    EXPECT_FALSE(assessment.stale_session);
    EXPECT_EQ(assessment.reason, "none");
  }

  TEST(SunshineSeatAgentCommonTest, OldActiveSessionWithoutRemoteTcpAloneIsNotStale) {
    const auto assessment = sunshineseat_agent::classify_stale_stream_session(
      1,
      true,
      181,
      false
    );

    EXPECT_FALSE(assessment.stale_session);
    EXPECT_EQ(assessment.reason, "none");
  }

  TEST(SunshineSeatAgentCommonTest, NoActiveSessionIsNotStale) {
    const auto assessment = sunshineseat_agent::classify_stale_stream_session(
      0,
      false,
      -1,
      false
    );

    EXPECT_FALSE(assessment.stale_session);
    EXPECT_EQ(assessment.reason, "none");
  }

  TEST(SunshineSeatAgentCommonTest, MultipleActiveSessionsRemainStale) {
    const auto assessment = sunshineseat_agent::classify_stale_stream_session(
      2,
      true,
      5,
      true
    );

    EXPECT_TRUE(assessment.stale_session);
    EXPECT_EQ(assessment.reason, "multiple active sessions reported");
  }
}  // namespace
