/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QTest>

class ScrollAccumulatorTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void wholeClick_sentInSameFrame();
  void hiResSteps_oneClickPerNotch();
  void halfClick_alreadyScrolls();
  void reversal_dropsLeftover();
  void wobble_doesNotScroll();
  void discreteAndSmoothInFrame_countedOnce();
  void smoothOnly_tenPixelsPerClick();
  void stop_dropsLeftoverAfterFrame();
  void stop_onlyAffectsItsAxis();
  void emptyFrame_sendsNothing();
};
