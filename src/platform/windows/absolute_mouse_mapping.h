#pragma once

namespace platf::windows_input {
  /**
   * Scale a coordinate that is already relative to the Windows virtual-desktop
   * origin into the normalized coordinate range required by SendInput().
   */
  int scale_virtual_desktop_axis(
    float coordinate_from_virtual_origin,
    int virtual_extent,
    int normalized_extent
  ) noexcept;
}  // namespace platf::windows_input
