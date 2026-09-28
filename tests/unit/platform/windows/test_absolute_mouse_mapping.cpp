/**
 * @file tests/unit/platform/windows/test_absolute_mouse_mapping.cpp
 * @brief Regression tests for Windows absolute-mouse virtual-desktop mapping.
 */
#include "../../../tests_common.h"

#ifdef _WIN32
  #include <cmath>
  #include <src/platform/windows/absolute_mouse_mapping.h>

TEST(AbsoluteMouseMappingTest, PreservesCoordinateAlreadyNormalizedToNegativeOriginDesktop) {
  constexpr int virtual_left = -1920;
  constexpr int virtual_width = 9046;
  constexpr int normalized_extent = 65535;
  constexpr int streamed_display_left = 0;
  constexpr int client_x = 958;

  // DXGI display offsets are normalized to the virtual-desktop origin before
  // input mapping. For this topology, host x=958 is therefore 2878 pixels
  // from the virtual origin and must not have virtual_left subtracted again.
  const float coordinate_from_virtual_origin =
    static_cast<float>(streamed_display_left - virtual_left + client_x);
  const int expected = std::lround(
    coordinate_from_virtual_origin * normalized_extent / virtual_width
  );

  EXPECT_EQ(
    platf::windows_input::scale_virtual_desktop_axis(
      coordinate_from_virtual_origin,
      virtual_width,
      normalized_extent
    ),
    expected
  );
}

TEST(AbsoluteMouseMappingTest, ClampsCoordinatesToVirtualDesktopBounds) {
  EXPECT_EQ(platf::windows_input::scale_virtual_desktop_axis(-1.0f, 3840, 65535), 0);
  EXPECT_EQ(platf::windows_input::scale_virtual_desktop_axis(5000.0f, 3840, 65535), 65535);
}
#endif
