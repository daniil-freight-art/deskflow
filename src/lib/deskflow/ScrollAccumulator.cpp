/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/ScrollAccumulator.h"

#include <cmath>

namespace deskflow {

void ScrollAccumulator::addDiscrete(int32_t dx, int32_t dy)
{
  m_x.frameDiscrete += dx;
  m_y.frameDiscrete += dy;
  m_frameHasDiscrete = true;
}

void ScrollAccumulator::addSmooth(double dx, double dy)
{
  if (!std::isfinite(dx) || !std::isfinite(dy))
    return;

  m_x.frameSmooth += dx;
  m_y.frameSmooth += dy;
}

void ScrollAccumulator::stop(bool x, bool y)
{
  // applied when the frame ends, after any scroll in the same frame
  m_x.stopped |= x;
  m_y.stopped |= y;
}

ScrollDelta ScrollAccumulator::endFrame()
{
  // a wheel step reported both ways counts once, as the discrete value
  const auto useDiscrete = m_frameHasDiscrete;
  m_frameHasDiscrete = false;
  return {m_x.endFrame(useDiscrete), m_y.endFrame(useDiscrete)};
}

int32_t ScrollAccumulator::Axis::endFrame(bool useDiscrete)
{
  double delta = frameSmooth * s_unitsPerPixel;
  if (useDiscrete) {
    // Hyprland repeats a wheel's pixel delta as its discrete value, so a
    // discrete value equal to the pixel delta is pixels, not 120-based units
    const bool discreteIsPixels = frameDiscrete != 0 && frameDiscrete == frameSmooth;
    delta = discreteIsPixels ? frameSmooth * s_unitsPerWheelPixel : frameDiscrete;
  }
  frameDiscrete = 0;
  frameSmooth = 0.0;

  double clicks = 0.0;
  if (delta != 0.0) {
    // on reversal, drop what is left over from the other direction
    const int newDirection = delta > 0.0 ? 1 : -1;
    if (newDirection != direction) {
      leftover = 0.0;
      direction = newDirection;
    }

    // round half away from zero, so half a click already scrolls; the
    // overshoot carries over and the next click comes a full click later
    leftover += delta;
    clicks = std::round(leftover / s_unitsPerClick);
    leftover -= clicks * s_unitsPerClick;
  }

  if (stopped) {
    leftover = 0.0;
    direction = 0;
    stopped = false;
  }

  return static_cast<int32_t>(clicks) * s_unitsPerClick;
}

} // namespace deskflow
