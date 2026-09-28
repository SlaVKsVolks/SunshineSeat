#pragma once

#include <utility>

namespace platf::dxgi::detail {

  // Move the previous frame out of the producer slot before the caller
  // releases it.  WinRT frame Close() may block, so destruction must happen
  // after the slot lock is released by the caller.
  template<typename Frame>
  [[nodiscard]] Frame replace_produced_frame(Frame &slot, Frame next) {
    auto previous = std::move(slot);
    slot = std::move(next);
    return previous;
  }

}  // namespace platf::dxgi::detail
