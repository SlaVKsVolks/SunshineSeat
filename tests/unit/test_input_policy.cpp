/**
 * @file tests/unit/test_input_policy.cpp
 * @brief Tests the bounded SunshineSeat input routing policy.
 */
#include "../tests_common.h"

#include <src/input.h>

TEST(InputSeatPolicyTests, AllowsOnlyProvenSingleUserRouting) {
  EXPECT_TRUE(input::allows_input_routing("sunshine"));
  EXPECT_FALSE(input::allows_input_routing("isolated"));
  EXPECT_FALSE(input::allows_input_routing("disabled"));
  EXPECT_FALSE(input::allows_input_routing("unexpected"));
}
