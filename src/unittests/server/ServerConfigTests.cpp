/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2014 - 2016 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ServerConfigTests.h"

#include "server/Config.h"

#include <QFile>
#include <QTemporaryDir>

class OnlySystemFilter : public InputFilter::Condition
{
public:
  Condition *clone() const override
  {
    return new OnlySystemFilter();
  }
  std::string format() const override
  {
    return "";
  }

  InputFilter::FilterStatus match(const Event &ev) override
  {
    return ev.getType() == EventTypes::System ? InputFilter::FilterStatus::Activate
                                              : InputFilter::FilterStatus::NoMatch;
  }
};

using namespace deskflow::server;

void ServerConfigTests::equalityCheck()
{
  Config a(nullptr);
  Config b(nullptr);
  QVERIFY(a.addScreen("screenA"));
  QVERIFY(a != b);

  QVERIFY(b.addScreen("screenB"));
  QVERIFY(a != b);

  QVERIFY(a.addScreen("screenB"));
  QVERIFY(a.addScreen("screenC"));
  QVERIFY(a.connect("screenA", Direction::Bottom, 0.0f, 0.5f, "screenB", 0.5f, 1.0f));
  QVERIFY(a.connect("screenB", Direction::Left, 0.0f, 0.5f, "screenB", 0.5f, 1.0f));
  QVERIFY(b.addScreen("screenA"));
  QVERIFY(b.addScreen("screenC"));
  QVERIFY(b.connect("screenA", Direction::Bottom, 0.0f, 0.5f, "screenB", 0.5f, 1.0f));
  QVERIFY(b.connect("screenB", Direction::Left, 0.0f, 0.5f, "screenB", 0.5f, 1.0f));
  QVERIFY(a.addOption("screenA", kOptionClipboardSharing, 1));
  QVERIFY(b.addOption("screenA", kOptionClipboardSharing, 1));
  QVERIFY(a.addOption(std::string(), kOptionClipboardSharing, 1));
  QVERIFY(b.addOption(std::string(), kOptionClipboardSharing, 1));

  a.getInputFilter()->addFilterRule(InputFilter::Rule{new OnlySystemFilter()});
  b.getInputFilter()->addFilterRule(InputFilter::Rule{new OnlySystemFilter()});
  QVERIFY(a.addAlias("screenA", "aliasA"));
  QVERIFY(b.addAlias("screenA", "aliasA"));
  /* TODO Fix linking to the proper libs
  NetworkAddress addr1("localhost", 8080);
  addr1.resolve();
  NetworkAddress addr2("localhost", 8080);
  addr2.resolve();
  a.setDeskflowAddress(addr1);
  b.setDeskflowAddress(addr2);
  */
  QVERIFY(a == b);
}

void ServerConfigTests::equalityCheck_diff_options()
{
  Config a(nullptr);
  Config b(nullptr);

  QVERIFY(a.addScreen("screenA"));
  QVERIFY(b.addScreen("screenA"));
  QVERIFY(a.addOption("screenA", kOptionClipboardSharing, 0));
  QVERIFY(b.addOption("screenA", kOptionClipboardSharing, 1));
  QVERIFY(a != b);
}

void ServerConfigTests::equalityCheck_diff_alias()
{
  Config a(nullptr);
  Config b(nullptr);

  QVERIFY(a.addScreen("screenA"));
  QVERIFY(b.addScreen("screenA"));
  QVERIFY(b.addAlias("screenA", "aliasA"));
  QVERIFY(a != b);

  QVERIFY(a.addAlias("screenA", "aliasA"));
  QVERIFY(b.addAlias("screenA", "aliasB"));
  QVERIFY(a != b);
}

void ServerConfigTests::equalityCheck_diff_filters()
{
  Config a(nullptr);
  Config b(nullptr);
  QVERIFY(a.addScreen("screenA"));
  QVERIFY(b.addScreen("screenA"));

  a.getInputFilter()->addFilterRule(InputFilter::Rule{new OnlySystemFilter()});
  QVERIFY(a != b);
}

// TODO FIX
/*
void ServerConfigTests::equalityCheck_diff_address()
{
  Config a(nullptr);
  Config b(nullptr);
  QVERIFY(a.addScreen("screenA"));
  QVERIFY(b.addScreen("screenA"));
  a.setDeskflowAddress(NetworkAddress(8000));
  b.setDeskflowAddress(NetworkAddress(9000));
  QVERIFY(a != b);
}
*/

void ServerConfigTests::equalityCheck_diff_neighbours1()
{
  Config a(nullptr);
  Config b(nullptr);
  QVERIFY(a.addScreen("screenA"));
  QVERIFY(a.addScreen("screenB"));
  QVERIFY(a.connect("screenA", Direction::Bottom, 0.0f, 0.5f, "screenB", 0.5f, 1.0f));
  QVERIFY(b.addScreen("screenA"));
  QVERIFY(b.addScreen("screenB"));
  QVERIFY(a != b);
  QVERIFY(b != a);
}

void ServerConfigTests::equalityCheck_diff_neighbours2()
{
  Config a(nullptr);
  Config b(nullptr);
  QVERIFY(a.addScreen("screenA"));
  QVERIFY(a.addScreen("screenB"));
  QVERIFY(a.connect("screenA", Direction::Bottom, 0.0f, 0.5f, "screenB", 0.5f, 1.0f));
  QVERIFY(b.addScreen("screenA"));
  QVERIFY(b.addScreen("screenB"));
  QVERIFY(b.connect("screenA", Direction::Bottom, 0.0f, 0.25f, "screenB", 0.25f, 1.0f));
  QVERIFY(a != b);
}

void ServerConfigTests::equalityCheck_diff_neighbours3()
{
  Config a(nullptr);
  Config b(nullptr);
  QVERIFY(a.addScreen("screenA"));
  QVERIFY(a.addScreen("screenB"));
  QVERIFY(a.addScreen("screenC"));
  QVERIFY(a.connect("screenA", Direction::Bottom, 0.0f, 0.5f, "screenB", 0.5f, 1.0f));
  QVERIFY(b.addScreen("screenA"));
  QVERIFY(b.addScreen("screenB"));
  QVERIFY(b.addScreen("screenC"));
  QVERIFY(b.connect("screenA", Direction::Bottom, 0.0f, 0.5f, "screenC", 0.5f, 1.0f));
  QVERIFY(a != b);
}

namespace {

// writes a script that creates a "marker" file in its working directory
QString writeScript(const QTemporaryDir &dir, const QString &name, QFileDevice::Permissions permissions)
{
  const auto path = dir.filePath(name);
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    return {};
  file.write("#!/bin/sh\necho ran > marker\n");
  file.close();
  file.setPermissions(permissions);
  return path;
}

const auto kOwnerOnly = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;

} // namespace

void ServerConfigTests::runScriptAction_valid()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QVERIFY(!writeScript(dir, QStringLiteral("my-script.sh"), kOwnerOnly).isEmpty());

  InputFilter::RunScriptAction action("my-script.sh", dir.path().toStdString());
  QCOMPARE(action.format(), "runScript(my-script.sh)");

  action.perform(Event());
  QTRY_VERIFY(QFile::exists(dir.filePath(QStringLiteral("marker"))));
}

void ServerConfigTests::runScriptAction_invalidNames()
{
  QVERIFY(InputFilter::RunScriptAction::isValidScriptName("my-script.sh"));
  QVERIFY(InputFilter::RunScriptAction::isValidScriptName("Backup_2"));

  QVERIFY(!InputFilter::RunScriptAction::isValidScriptName(""));
  QVERIFY(!InputFilter::RunScriptAction::isValidScriptName(".hidden"));
  QVERIFY(!InputFilter::RunScriptAction::isValidScriptName(".."));
  QVERIFY(!InputFilter::RunScriptAction::isValidScriptName("../evil"));
  QVERIFY(!InputFilter::RunScriptAction::isValidScriptName("/bin/sh"));
  QVERIFY(!InputFilter::RunScriptAction::isValidScriptName("a b"));
  QVERIFY(!InputFilter::RunScriptAction::isValidScriptName("a;rm -rf ~"));
  QVERIFY(!InputFilter::RunScriptAction::isValidScriptName("$(id)"));
}

void ServerConfigTests::runScriptAction_unsafeScript()
{
#ifdef Q_OS_WIN
  QSKIP("permission checks are unix only");
#endif
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QVERIFY(!writeScript(dir, QStringLiteral("shared.sh"), kOwnerOnly | QFileDevice::WriteOther).isEmpty());
  QVERIFY(!writeScript(dir, QStringLiteral("not-executable.sh"), QFileDevice::ReadOwner).isEmpty());

  InputFilter::RunScriptAction("shared.sh", dir.path().toStdString()).perform(Event());
  InputFilter::RunScriptAction("not-executable.sh", dir.path().toStdString()).perform(Event());
  InputFilter::RunScriptAction("missing.sh", dir.path().toStdString()).perform(Event());

  QTest::qWait(500);
  QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("marker"))));
}

void ServerConfigTests::runScriptAction_symlink()
{
#ifdef Q_OS_WIN
  QSKIP("permission checks are unix only");
#endif
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const auto target = writeScript(dir, QStringLiteral("target.sh"), kOwnerOnly);
  QVERIFY(QFile::link(target, dir.filePath(QStringLiteral("link.sh"))));

  InputFilter::RunScriptAction("link.sh", dir.path().toStdString()).perform(Event());

  QTest::qWait(500);
  QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("marker"))));
}

void ServerConfigTests::runScriptAction_unsafeDirectory()
{
#ifdef Q_OS_WIN
  QSKIP("permission checks are unix only");
#endif
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QVERIFY(!writeScript(dir, QStringLiteral("my-script.sh"), kOwnerOnly).isEmpty());
  QVERIFY(QFile::setPermissions(dir.path(), kOwnerOnly | QFileDevice::WriteGroup | QFileDevice::ExeGroup));

  InputFilter::RunScriptAction("my-script.sh", dir.path().toStdString()).perform(Event());

  QTest::qWait(500);
  QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("marker"))));
}

QTEST_MAIN(ServerConfigTests)
