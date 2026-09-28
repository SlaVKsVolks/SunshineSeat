/**
 * @file src/input.h
 * @brief Declarations for gamepad, keyboard, and mouse input handling.
 */
#pragma once

// standard includes
#include <functional>
#include <string_view>

// local includes
#include "platform/common.h"
#include "crypto.h"
#include "thread_safe.h"

namespace input {
  /**
   * Decide whether the configured seat policy can use the legacy host-wide
   * input injection path. Only "sunshine" has a proven implementation here;
   * isolated routing requires a session-bound backend.
   */
  [[nodiscard]] bool allows_input_routing(std::string_view policy) noexcept;

  struct input_t;

  void print(void *input);
  /**
   * Release all pressed keyboard and mouse state before a session ends.
   *
   * @return true when queued releases completed promptly; false when the
   *         direct fail-safe fallback was needed.
   */
  bool reset(std::shared_ptr<input_t> &input);
  void passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data, crypto::PERM permission = crypto::PERM::_all_inputs);

  [[nodiscard]] std::unique_ptr<platf::deinit_t> init();

  bool probe_gamepads();

  std::shared_ptr<input_t> alloc(safe::mail_t mail);

  struct touch_port_t: public platf::touch_port_t {
    int env_width;
    int env_height;

    // Offset x and y coordinates of the client
    float client_offsetX;
    float client_offsetY;

    float scalar_inv;
    float scalar_tpcoords;

    int env_logical_width;
    int env_logical_height;

    explicit operator bool() const {
      return width != 0 && height != 0 && env_width != 0 && env_height != 0;
    }
  };

  /**
   * @brief Scale the ellipse axes according to the provided size.
   * @param val The major and minor axis pair.
   * @param rotation The rotation value from the touch/pen event.
   * @param scalar The scalar cartesian coordinate pair.
   * @return The major and minor axis pair.
   */
  std::pair<float, float> scale_client_contact_area(const std::pair<float, float> &val, uint16_t rotation, const std::pair<float, float> &scalar);
}  // namespace input
