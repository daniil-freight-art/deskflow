/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "base/Log.h"

#include <QTest>

class ServerConfigTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void equalityCheck();
  void equalityCheck_diff_options();
  void equalityCheck_diff_alias();
  void equalityCheck_diff_filters();
  //  void equalityCheck_diff_address();
  void equalityCheck_diff_neighbours1();
  void equalityCheck_diff_neighbours2();
  void equalityCheck_diff_neighbours3();
  void runScriptAction_valid();
  void runScriptAction_invalidNames();
  void runScriptAction_unsafeScript();
  void runScriptAction_symlink();
  void runScriptAction_unsafeDirectory();

private:
  Log m_log;
};
