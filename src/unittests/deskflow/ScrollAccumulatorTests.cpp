/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ScrollAccumulatorTests.h"

#include "deskflow/ScrollAccumulator.h"

#include <vector>

using deskflow::ScrollAccumulator;

namespace {
constexpr int32_t Click = ScrollAccumulator::s_unitsPerClick;
// a hi-res wheel that reports 8 steps per notch
constexpr int32_t HiResStep = Click / 8;

// send each step as its own discrete frame on the y axis, return the clicks sent per frame
std::vector<int32_t> discreteFrames(ScrollAccumulator &accumulator, const std::vector<int32_t> &steps)
{
  std::vector<int32_t> sent;
  for (auto step : steps) {
    accumulator.addDiscrete(0, step);
    sent.push_back(accumulator.endFrame().y);
  }
  return sent;
}

int32_t total(const std::vector<int32_t> &sent)
{
  int32_t sum = 0;
  for (auto value : sent)
    sum += value;
  return sum;
}
} // namespace

void ScrollAccumulatorTests::wholeClick_sentInSameFrame()
{
  ScrollAccumulator accumulator;

  accumulator.addDiscrete(0, Click);
  QCOMPARE(accumulator.endFrame().y, Click);

  accumulator.addDiscrete(0, -2 * Click);
  QCOMPARE(accumulator.endFrame().y, -2 * Click);
}

void ScrollAccumulatorTests::hiResSteps_oneClickPerNotch()
{
  ScrollAccumulator accumulator;

  const std::vector<int32_t> threeNotches(24, HiResStep);
  const auto sent = discreteFrames(accumulator, threeNotches);

  QCOMPARE(total(sent), 3 * Click);
  for (auto value : sent)
    QVERIFY(value == 0 || value == Click);
}

void ScrollAccumulatorTests::halfClick_alreadyScrolls()
{
  ScrollAccumulator accumulator;

  const auto sent = discreteFrames(accumulator, {HiResStep, HiResStep, HiResStep, HiResStep});

  QCOMPARE(sent, std::vector<int32_t>({0, 0, 0, Click}));
}

void ScrollAccumulatorTests::reversal_dropsLeftover()
{
  ScrollAccumulator accumulator;

  // 7 of 8 steps down: one click sent, the rest is left over
  QCOMPARE(total(discreteFrames(accumulator, std::vector<int32_t>(7, HiResStep))), Click);

  // half a notch up scrolls up right away instead of first cancelling the leftover
  const auto sent = discreteFrames(accumulator, {-HiResStep, -HiResStep, -HiResStep, -HiResStep});
  QCOMPARE(sent, std::vector<int32_t>({0, 0, 0, -Click}));
}

void ScrollAccumulatorTests::wobble_doesNotScroll()
{
  ScrollAccumulator accumulator;

  // a wheel resting just past the point where it sent a click, wobbling back and forth
  const auto sent = discreteFrames(
      accumulator, {HiResStep, HiResStep, HiResStep, HiResStep, -HiResStep, HiResStep, -HiResStep, HiResStep}
  );

  QCOMPARE(sent, std::vector<int32_t>({0, 0, 0, Click, 0, 0, 0, 0}));
}

void ScrollAccumulatorTests::discreteAndSmoothInFrame_countedOnce()
{
  ScrollAccumulator accumulator;

  // the same wheel notch reported both ways, in either order
  accumulator.addSmooth(0.0, 15.0);
  accumulator.addDiscrete(0, Click);
  QCOMPARE(accumulator.endFrame().y, Click);

  accumulator.addDiscrete(0, Click);
  accumulator.addSmooth(0.0, 15.0);
  QCOMPARE(accumulator.endFrame().y, Click);

  // a later smooth-only frame is not affected
  accumulator.addSmooth(0.0, 10.0);
  QCOMPARE(accumulator.endFrame().y, Click);
}

void ScrollAccumulatorTests::discreteEqualToPixels_fifteenPixelsPerClick()
{
  ScrollAccumulator accumulator;

  // Hyprland reports a wheel notch as 15 pixels and repeats them as the discrete value
  for (int i = 0; i < 3; ++i) {
    accumulator.addSmooth(0.0, 15.0);
    accumulator.addDiscrete(0, 15);
    QCOMPARE(accumulator.endFrame().y, Click);
  }

  // a hi-res step of 1.875 pixels arrives truncated to 1, so 8 steps are 64 of 120
  std::vector<int32_t> sent;
  for (int i = 0; i < 8; ++i) {
    accumulator.addSmooth(0.0, -1.0);
    accumulator.addDiscrete(0, -1);
    sent.push_back(accumulator.endFrame().y);
  }
  QCOMPARE(sent, std::vector<int32_t>({0, 0, 0, 0, 0, 0, 0, -Click}));
}

void ScrollAccumulatorTests::smoothOnly_tenPixelsPerClick()
{
  ScrollAccumulator accumulator;

  int32_t sum = 0;
  for (int i = 0; i < 30; ++i) {
    accumulator.addSmooth(0.0, -1.0);
    sum += accumulator.endFrame().y;
  }

  QCOMPARE(sum, -3 * Click);
}

void ScrollAccumulatorTests::stop_dropsLeftoverAfterFrame()
{
  ScrollAccumulator accumulator;

  // 3 pixels are not enough for a click, but the stop must not drop them before they are counted
  accumulator.addSmooth(0.0, 3.0);
  QCOMPARE(accumulator.endFrame().y, 0);
  accumulator.addSmooth(0.0, 3.0);
  accumulator.stop(false, true);
  QCOMPARE(accumulator.endFrame().y, Click);

  // the click overshot by 4 pixels; after the stop the next scroll starts from nothing
  accumulator.addSmooth(0.0, 6.0);
  QCOMPARE(accumulator.endFrame().y, Click);
}

void ScrollAccumulatorTests::stop_onlyAffectsItsAxis()
{
  ScrollAccumulator accumulator;

  accumulator.addSmooth(4.0, 4.0);
  accumulator.endFrame();
  accumulator.stop(true, false);
  accumulator.endFrame();

  accumulator.addSmooth(4.0, 4.0);
  const auto sent = accumulator.endFrame();
  QCOMPARE(sent.x, 0);
  QCOMPARE(sent.y, Click);
}

void ScrollAccumulatorTests::emptyFrame_sendsNothing()
{
  ScrollAccumulator accumulator;

  const auto sent = accumulator.endFrame();
  QCOMPARE(sent.x, 0);
  QCOMPARE(sent.y, 0);
}

QTEST_MAIN(ScrollAccumulatorTests)
