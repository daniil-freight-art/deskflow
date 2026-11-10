/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "arch/Arch.h"
#include "base/Log.h"

#include <QTest>

class WlDataControlClipboardTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void initTestCase();

  void parseConfig_values();
  void chooseMimeTypes_prefersUtf8TextAndPng();
  void chooseMimeTypes_unknownTypes_nothingChosen();
  void mimeTypesFor_formats();
  void textFromSelection_normalizes();
  void htmlFromSelection_decodesUtf16AndBom();
  void bitmapFromImage_png_bottomUpBgraDib();
  void bitmapFromImage_invalidOrTooLarge_empty();
  void pngFromBitmap_roundTrip();
  void dibPixelOffset_headers();
  void marshalledSize_matchesClipboard();
  void fitToLimit_dropsLargestFirst();

  // Needs a compositor with data control, see the test for how to run it
  void compositor_sharesBetweenClients();
  void compositor_interoperatesWithWlClipboard();

private:
  Arch m_arch;
  Log m_log;
};
