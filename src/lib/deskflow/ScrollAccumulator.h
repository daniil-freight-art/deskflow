/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "common/Coordinate.h"

#include <cstdint>

namespace deskflow {

/**
 * @brief Turns fine-grained scroll input into whole wheel clicks.
 *
 * Not every client can handle a fraction of a wheel click, so a primary
 * that receives hi-res wheel steps or smooth pixel deltas collects them here
 * and only sends whole clicks (multiples of 120).
 *
 * Input is collected per hardware frame. A compositor may report the same
 * wheel step both as a discrete value and as a smooth pixel delta, so when a
 * frame has a discrete value its pixel delta is ignored.
 *
 * A click is sent once half of it has been scrolled, so a short scroll
 * still moves. When the scroll direction reverses, whatever is left over
 * from the previous direction is dropped, so the first movement in the
 * new direction is not spent cancelling it.
 */
class ScrollAccumulator
{
public:
  //! One wheel click, in the 120-based units used by libei and deskflow
  static constexpr int32_t s_unitsPerClick = 120;
  //! 10 pixels of smooth scroll make one wheel click
  static constexpr double s_unitsPerPixel = s_unitsPerClick / 10.0;

  //! Add a discrete scroll in 120-based units, a fraction or multiple of a click
  void addDiscrete(int32_t dx, int32_t dy);

  //! Add a smooth scroll in pixels
  void addSmooth(double dx, double dy);

  //! A scroll sequence stopped, so drop what is left over on that axis once the frame ends
  void stop(bool x, bool y);

  /**
   * @brief End the current hardware frame
   * @return The whole clicks to send, in 120-based units with the same sign as the input
   */
  ScrollDelta endFrame();

private:
  struct Axis
  {
    int32_t frameDiscrete = 0;
    double frameSmooth = 0.0;
    double leftover = 0.0;
    int direction = 0;
    bool stopped = false;

    int32_t endFrame(bool useDiscrete);
  };

  Axis m_x;
  Axis m_y;
  bool m_frameHasDiscrete = false;
};

} // namespace deskflow
