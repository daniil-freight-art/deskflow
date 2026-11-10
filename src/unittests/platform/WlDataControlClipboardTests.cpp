/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "WlDataControlClipboardTests.h"

#include "deskflow/Clipboard.h"
#include "platform/WlDataControlClipboard.h"

#include <QBuffer>
#include <QByteArray>
#include <QImage>
#include <QProcess>
#include <QStandardPaths>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using deskflow::WlDataControlClipboard;
using Format = IClipboard::Format;
using Formats = WlDataControlClipboard::Formats;
using Mode = WlDataControlClipboard::Mode;
using Protocol = WlDataControlClipboard::Protocol;

namespace {

constexpr auto idx(Format format)
{
  return static_cast<std::size_t>(format);
}

std::uint32_t readLe32(const std::string &data, std::size_t offset)
{
  std::uint32_t value = 0;
  for (int i = 3; i >= 0; --i) {
    value = (value << 8) | static_cast<unsigned char>(data[offset + i]);
  }
  return value;
}

void writeLe32(std::string &data, std::size_t offset, std::uint32_t value)
{
  for (int i = 0; i < 4; ++i) {
    data[offset + i] = static_cast<char>((value >> (8 * i)) & 0xFF);
  }
}

void writeLe16(std::string &data, std::size_t offset, std::uint16_t value)
{
  data[offset] = static_cast<char>(value & 0xFF);
  data[offset + 1] = static_cast<char>((value >> 8) & 0xFF);
}

// 3x2 image with a distinct color in every pixel
QImage testImage()
{
  QImage image(3, 2, QImage::Format_ARGB32);
  image.setPixel(0, 0, qRgba(255, 0, 0, 255));
  image.setPixel(1, 0, qRgba(0, 255, 0, 255));
  image.setPixel(2, 0, qRgba(0, 0, 255, 255));
  image.setPixel(0, 1, qRgba(10, 20, 30, 255));
  image.setPixel(1, 1, qRgba(40, 50, 60, 255));
  image.setPixel(2, 1, qRgba(70, 80, 90, 255));
  return image;
}

std::string encode(const QImage &image, const char *format)
{
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, format);
  return bytes.toStdString();
}

// DIB with a BITMAPINFOHEADER and the given header fields, followed by extra bytes
std::string dibHeader(
    std::uint32_t headerSize, std::uint16_t bitCount, std::uint32_t compression, std::uint32_t colorsUsed,
    std::size_t extra
)
{
  std::string dib(headerSize + extra, '\0');
  writeLe32(dib, 0, headerSize);
  writeLe32(dib, 4, 1);  // width
  writeLe32(dib, 8, 1);  // height
  writeLe16(dib, 12, 1); // planes
  writeLe16(dib, 14, bitCount);
  writeLe32(dib, 16, compression);
  writeLe32(dib, 32, colorsUsed);
  return dib;
}

// Collects the selections reported by one WlDataControlClipboard
class Recorder
{
public:
  void add(Formats formats, std::uint64_t serial)
  {
    std::scoped_lock lock{m_mutex};
    m_selections.push_back({std::move(formats), serial});
  }

  std::size_t count() const
  {
    std::scoped_lock lock{m_mutex};
    return m_selections.size();
  }

  std::pair<Formats, std::uint64_t> last() const
  {
    std::scoped_lock lock{m_mutex};
    return m_selections.back();
  }

private:
  mutable std::mutex m_mutex;
  std::vector<std::pair<Formats, std::uint64_t>> m_selections;
};

// wl-copy serving a selection until another client takes it
class WlCopy
{
public:
  WlCopy(const QString &program, const QString &mimeType, const QByteArray &data)
  {
    m_process.start(program, {"--foreground", "--type", mimeType});
    if (!m_process.waitForStarted(5000)) {
      return;
    }
    m_process.write(data);
    while (m_process.bytesToWrite() > 0 && m_process.waitForBytesWritten(5000)) {
    }
    m_process.closeWriteChannel();
  }

  ~WlCopy()
  {
    if (m_process.state() != QProcess::NotRunning) {
      m_process.kill();
      m_process.waitForFinished(5000);
    }
  }

  WlCopy(const WlCopy &) = delete;
  WlCopy &operator=(const WlCopy &) = delete;

  bool waitForExit(int msecs)
  {
    return m_process.state() == QProcess::NotRunning || m_process.waitForFinished(msecs);
  }

private:
  QProcess m_process;
};

// Runs wl-paste and returns what it printed, or nothing if it failed
std::optional<QByteArray> wlPaste(const QString &program, const QStringList &arguments)
{
  QProcess process;
  process.start(program, arguments);
  process.closeWriteChannel();
  QByteArray output;
  while (process.waitForReadyRead(5000)) {
    output += process.readAllStandardOutput();
  }
  if (process.state() != QProcess::NotRunning && !process.waitForFinished(5000)) {
    process.kill();
    process.waitForFinished(5000);
    return std::nullopt;
  }
  output += process.readAllStandardOutput();
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    return std::nullopt;
  }
  return output;
}

} // namespace

void WlDataControlClipboardTests::initTestCase()
{
  m_arch.init();
  m_log.setFilter(LogLevel::Level::Debug);
}

void WlDataControlClipboardTests::parseConfig_values()
{
  const auto check = [](const char *value, Mode mode, Protocol protocol) {
    const auto config = WlDataControlClipboard::parseConfig(value);
    return config.mode == mode && config.protocol == protocol;
  };

  QVERIFY(check("", Mode::Auto, Protocol::Any));
  QVERIFY(check("auto", Mode::Auto, Protocol::Any));
  QVERIFY(check(" AUTO ", Mode::Auto, Protocol::Any));
  QVERIFY(check("off", Mode::Disabled, Protocol::Any));
  QVERIFY(check("0", Mode::Disabled, Protocol::Any));
  QVERIFY(check("false", Mode::Disabled, Protocol::Any));
  QVERIFY(check("on", Mode::Enabled, Protocol::Any));
  QVERIFY(check("1", Mode::Enabled, Protocol::Any));
  QVERIFY(check("Yes", Mode::Enabled, Protocol::Any));
  QVERIFY(check("ext", Mode::Enabled, Protocol::Ext));
  QVERIFY(check("wlr", Mode::Enabled, Protocol::Wlr));
  QVERIFY(check("something-else", Mode::Auto, Protocol::Any));
}

void WlDataControlClipboardTests::chooseMimeTypes_prefersUtf8TextAndPng()
{
  const std::vector<std::string> offered = {
      "TEXT",
      "text/plain",
      "text/plain; charset=UTF-8",
      "image/jpeg",
      "image/png",
      "text/html",
      "x-special/gnome-copied-files",
      "STRING",
      "chromium/x-web-custom-data"
  };

  const auto chosen = WlDataControlClipboard::chooseMimeTypes(offered);

  QCOMPARE(chosen[idx(Format::Text)], std::optional<std::string>("text/plain; charset=UTF-8"));
  QCOMPARE(chosen[idx(Format::HTML)], std::optional<std::string>("text/html"));
  QCOMPARE(chosen[idx(Format::Bitmap)], std::optional<std::string>("image/png"));
}

void WlDataControlClipboardTests::chooseMimeTypes_unknownTypes_nothingChosen()
{
  const auto chosen = WlDataControlClipboard::chooseMimeTypes({"application/x-foo", "text/uri-list"});

  QVERIFY(!chosen[idx(Format::Text)]);
  QVERIFY(!chosen[idx(Format::HTML)]);
  QVERIFY(!chosen[idx(Format::Bitmap)]);
}

void WlDataControlClipboardTests::mimeTypesFor_formats()
{
  const auto text = WlDataControlClipboard::mimeTypesFor(Format::Text);
  QVERIFY(!text.empty());
  QCOMPARE(text.front(), std::string("text/plain;charset=utf-8"));
  QVERIFY(std::ranges::find(text, std::string("UTF8_STRING")) != text.end());

  QCOMPARE(WlDataControlClipboard::mimeTypesFor(Format::HTML), std::vector<std::string>{"text/html"});
  QCOMPARE(WlDataControlClipboard::mimeTypesFor(Format::Bitmap), std::vector<std::string>{"image/png"});
}

void WlDataControlClipboardTests::textFromSelection_normalizes()
{
  QCOMPARE(WlDataControlClipboard::textFromSelection("plain"), std::string("plain"));
  QCOMPARE(WlDataControlClipboard::textFromSelection("a\r\nb\r\n"), std::string("a\nb\n"));
  QCOMPARE(WlDataControlClipboard::textFromSelection(std::string("nul\0\0", 5)), std::string("nul"));
  QCOMPARE(WlDataControlClipboard::textFromSelection("caf\xC3\xA9"), std::string("caf\xC3\xA9"));
  QCOMPARE(WlDataControlClipboard::textFromSelection("bad\xFF"), std::string("bad\xEF\xBF\xBD"));
  QCOMPARE(WlDataControlClipboard::textFromSelection("cut\xE2\x82"), std::string("cut\xEF\xBF\xBD\xEF\xBF\xBD"));
}

void WlDataControlClipboardTests::htmlFromSelection_decodesUtf16AndBom()
{
  QCOMPARE(WlDataControlClipboard::htmlFromSelection("<b>x</b>"), std::string("<b>x</b>"));
  QCOMPARE(WlDataControlClipboard::htmlFromSelection("\xEF\xBB\xBF<i>y</i>"), std::string("<i>y</i>"));

  const std::string utf16le("\xFF\xFE<\0b\0>\0\xE9\0", 10);
  QCOMPARE(WlDataControlClipboard::htmlFromSelection(utf16le), std::string("<b>\xC3\xA9"));

  const std::string utf16be("\xFE\xFF\0<\0p\0>", 8);
  QCOMPARE(WlDataControlClipboard::htmlFromSelection(utf16be), std::string("<p>"));
}

void WlDataControlClipboardTests::bitmapFromImage_png_bottomUpBgraDib()
{
  const auto dib = WlDataControlClipboard::bitmapFromImage(encode(testImage(), "PNG"), 1024 * 1024);

  QCOMPARE(dib.size(), std::size_t{40 + 3 * 2 * 4});
  QCOMPARE(readLe32(dib, 0), 40u); // header size
  QCOMPARE(readLe32(dib, 4), 3u);  // width
  QCOMPARE(readLe32(dib, 8), 2u);  // height, bottom-up
  QCOMPARE(dib[14], char(32));     // bits per pixel
  QCOMPARE(readLe32(dib, 16), 0u); // BI_RGB
  QCOMPARE(WlDataControlClipboard::dibPixelOffset(dib), std::size_t{40});

  // first row in the DIB is the bottom row of the image, pixels are BGRA
  QCOMPARE(dib.substr(40, 4), std::string("\x1E\x14\x0A\xFF", 4));
  QCOMPARE(dib.substr(40 + 3 * 4, 4), std::string("\x00\x00\xFF\xFF", 4));
}

void WlDataControlClipboardTests::bitmapFromImage_invalidOrTooLarge_empty()
{
  QVERIFY(WlDataControlClipboard::bitmapFromImage("", 1024).empty());
  QVERIFY(WlDataControlClipboard::bitmapFromImage("not an image", 1024 * 1024).empty());

  // 3x2 pixels need 40 + 24 bytes
  const auto png = encode(testImage(), "PNG");
  QVERIFY(WlDataControlClipboard::bitmapFromImage(png, 63).empty());
  QVERIFY(!WlDataControlClipboard::bitmapFromImage(png, 64).empty());
}

void WlDataControlClipboardTests::pngFromBitmap_roundTrip()
{
  const auto dib = WlDataControlClipboard::bitmapFromImage(encode(testImage(), "BMP"), 1024 * 1024);
  QVERIFY(!dib.empty());

  const auto png = WlDataControlClipboard::pngFromBitmap(dib);
  QVERIFY(png.starts_with("\x89PNG"));

  QImage decoded;
  QVERIFY(decoded.loadFromData(QByteArray::fromStdString(png), "PNG"));
  QCOMPARE(decoded.size(), QSize(3, 2));
  const auto expected = testImage();
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 3; ++x) {
      QCOMPARE(QColor(decoded.pixel(x, y)), QColor(expected.pixel(x, y)));
    }
  }

  // and back again, as when the server's bitmap is pasted and copied on another computer
  QCOMPARE(WlDataControlClipboard::bitmapFromImage(png, 1024 * 1024), dib);

  QVERIFY(WlDataControlClipboard::pngFromBitmap("").empty());
  QVERIFY(WlDataControlClipboard::pngFromBitmap("garbage").empty());
}

void WlDataControlClipboardTests::dibPixelOffset_headers()
{
  // 32-bit BI_RGB, no palette
  QCOMPARE(WlDataControlClipboard::dibPixelOffset(dibHeader(40, 32, 0, 0, 4)), std::size_t{40});
  // 8-bit with a full palette of 4 byte entries
  QCOMPARE(WlDataControlClipboard::dibPixelOffset(dibHeader(40, 8, 0, 0, 1024 + 4)), std::size_t{40 + 1024});
  // 8-bit with a short palette
  QCOMPARE(WlDataControlClipboard::dibPixelOffset(dibHeader(40, 8, 0, 16, 64 + 4)), std::size_t{40 + 64});
  // BI_BITFIELDS masks follow a BITMAPINFOHEADER
  QCOMPARE(WlDataControlClipboard::dibPixelOffset(dibHeader(40, 32, 3, 0, 12 + 4)), std::size_t{52});
  // BITMAPV5HEADER contains its masks
  QCOMPARE(WlDataControlClipboard::dibPixelOffset(dibHeader(124, 32, 3, 0, 4)), std::size_t{124});

  // BITMAPCOREHEADER, 8-bit with a palette of 3 byte entries
  std::string core(12 + 768 + 1, '\0');
  writeLe32(core, 0, 12);
  writeLe16(core, 10, 8);
  QCOMPARE(WlDataControlClipboard::dibPixelOffset(core), std::size_t{12 + 768});

  // too short, unknown header size, palette beyond the data
  QCOMPARE(WlDataControlClipboard::dibPixelOffset("short"), std::size_t{0});
  QCOMPARE(WlDataControlClipboard::dibPixelOffset(dibHeader(20, 32, 0, 0, 30)), std::size_t{0});
  QCOMPARE(WlDataControlClipboard::dibPixelOffset(dibHeader(40, 8, 0, 0, 100)), std::size_t{0});
}

void WlDataControlClipboardTests::marshalledSize_matchesClipboard()
{
  Formats formats;
  formats[idx(Format::Text)] = "hello";
  formats[idx(Format::HTML)] = "<b>hello</b>";

  Clipboard clipboard;
  clipboard.open(0);
  clipboard.empty();
  clipboard.add(Format::Text, *formats[idx(Format::Text)]);
  clipboard.add(Format::HTML, *formats[idx(Format::HTML)]);
  clipboard.close();

  QCOMPARE(WlDataControlClipboard::marshalledSize(formats), clipboard.marshall().size());
  QCOMPARE(WlDataControlClipboard::marshalledSize(Formats{}), std::size_t{4});
}

void WlDataControlClipboardTests::fitToLimit_dropsLargestFirst()
{
  Formats formats;
  formats[idx(Format::Text)] = std::string(10, 't');
  formats[idx(Format::HTML)] = std::string(20, 'h');
  formats[idx(Format::Bitmap)] = std::string(100, 'b');
  // 4 + (8 + 10) + (8 + 20) + (8 + 100) = 158

  auto all = formats;
  QVERIFY(WlDataControlClipboard::fitToLimit(all, 159).empty());
  QVERIFY(all[idx(Format::Bitmap)]);

  // the server refuses data of exactly the limit
  auto noBitmap = formats;
  QCOMPARE(WlDataControlClipboard::fitToLimit(noBitmap, 158), std::vector<Format>{Format::Bitmap});
  QVERIFY(noBitmap[idx(Format::Text)] && noBitmap[idx(Format::HTML)] && !noBitmap[idx(Format::Bitmap)]);

  auto textOnly = formats;
  QCOMPARE(WlDataControlClipboard::fitToLimit(textOnly, 40), (std::vector<Format>{Format::Bitmap, Format::HTML}));
  QVERIFY(textOnly[idx(Format::Text)]);

  auto none = formats;
  QCOMPARE(WlDataControlClipboard::fitToLimit(none, 0).size(), std::size_t{3});
  QCOMPARE(WlDataControlClipboard::marshalledSize(none), std::size_t{4});
}

void WlDataControlClipboardTests::compositor_sharesBetweenClients()
{
  // This test sets the clipboard, so it only runs against a compositor named
  // explicitly, e.g. a headless one started for testing:
  //   WLR_BACKENDS=headless WLR_RENDERER=pixman sway &
  //   DESKFLOW_TEST_WAYLAND_DISPLAY=wayland-1 ./WlDataControlClipboardTests
  // DESKFLOW_WL_CLIPBOARD=wlr or ext selects the protocol to test.
  const auto display = qgetenv("DESKFLOW_TEST_WAYLAND_DISPLAY");
  if (display.isEmpty()) {
    QSKIP("DESKFLOW_TEST_WAYLAND_DISPLAY is not set");
  }
  qputenv("WAYLAND_DISPLAY", display);
  const auto protocol = WlDataControlClipboard::configFromEnvironment().protocol;

  Recorder recordedA;
  Recorder recordedB;
  auto a = WlDataControlClipboard::create(
      [&recordedA](Formats formats, std::uint64_t serial) { recordedA.add(std::move(formats), serial); }, 1024 * 1024,
      protocol, false
  );
  auto b = WlDataControlClipboard::create(
      [&recordedB](Formats formats, std::uint64_t serial) { recordedB.add(std::move(formats), serial); }, 1024 * 1024,
      protocol, false
  );
  QVERIFY2(a && b, "the compositor has no data control");
  qInfo("protocol: %s", a->protocolName());

  // text, html and a bitmap set by one client reach the other one
  Formats formats;
  formats[idx(Format::Text)] = "hello from a\nsecond line";
  formats[idx(Format::HTML)] = "<b>hello</b>";
  formats[idx(Format::Bitmap)] = WlDataControlClipboard::bitmapFromImage(encode(testImage(), "PNG"), 1024 * 1024);
  a->setSelection(formats);

  QTRY_COMPARE_WITH_TIMEOUT(recordedB.count(), std::size_t{1}, 5000);
  auto [received, serial] = recordedB.last();
  QCOMPARE(received[idx(Format::Text)], formats[idx(Format::Text)]);
  QVERIFY(received[idx(Format::HTML)]);
  QVERIFY(received[idx(Format::HTML)]->ends_with("<b>hello</b>"));
  QVERIFY(received[idx(Format::HTML)]->find("charset=utf-8") != std::string::npos);
  QCOMPARE(received[idx(Format::Bitmap)], formats[idx(Format::Bitmap)]);
  QVERIFY(b->isCurrent(serial));

  // the client that set the selection doesn't report it back
  QTest::qWait(300);
  QCOMPARE(recordedA.count(), std::size_t{0});

  // setting the same data again changes nothing
  a->setSelection(formats);
  QTest::qWait(300);
  QCOMPARE(recordedB.count(), std::size_t{1});

  // another client offering the same content, like a clipboard manager
  // taking over the selection, isn't reported as a change
  b->setSelection(received);
  QTest::qWait(300);
  QCOMPARE(recordedA.count(), std::size_t{0});
  QCOMPARE(recordedB.count(), std::size_t{1});

  // and the other way round
  Formats reply;
  reply[idx(Format::Text)] = "reply from b";
  b->setSelection(reply);
  QTRY_COMPARE_WITH_TIMEOUT(recordedA.count(), std::size_t{1}, 5000);
  QCOMPARE(recordedA.last().first[idx(Format::Text)], reply[idx(Format::Text)]);
  QVERIFY(!recordedA.last().first[idx(Format::HTML)]);
  QVERIFY(!recordedA.last().first[idx(Format::Bitmap)]);

  // with sharing off nothing is read
  a->setMaxBytes(0);
  b->setSelection(formats);
  QTest::qWait(300);
  QCOMPARE(recordedA.count(), std::size_t{1});
  a->setMaxBytes(1024 * 1024);

  // the content seen last isn't reported again...
  b->setSelection(reply);
  QTest::qWait(300);
  QCOMPARE(recordedA.count(), std::size_t{1});

  // ...unless another computer took the clipboard since, which makes
  // the reported selection stale
  const auto staleSerial = recordedA.last().second;
  QVERIFY(a->isCurrent(staleSerial));
  a->forgetSelection();
  QVERIFY(!a->isCurrent(staleSerial));

  a->setMaxBytes(0);
  b->setSelection(formats);
  QTest::qWait(300);
  a->setMaxBytes(1024 * 1024);
  b->setSelection(reply);
  QTRY_COMPARE_WITH_TIMEOUT(recordedA.count(), std::size_t{2}, 5000);
  QCOMPARE(recordedA.last().first[idx(Format::Text)], reply[idx(Format::Text)]);
  QVERIFY(a->isCurrent(recordedA.last().second));

  // formats over the limit are dropped, the rest is shared
  a->setMaxBytes(100);
  b->setSelection(formats);
  QTRY_COMPARE_WITH_TIMEOUT(recordedA.count(), std::size_t{3}, 5000);
  const auto limited = recordedA.last().first;
  QCOMPARE(limited[idx(Format::Text)], formats[idx(Format::Text)]);
  QVERIFY(!limited[idx(Format::HTML)]);
  QVERIFY(!limited[idx(Format::Bitmap)]);
  QVERIFY(WlDataControlClipboard::marshalledSize(limited) < 100);

  a.reset();
  b.reset();
}

void WlDataControlClipboardTests::compositor_interoperatesWithWlClipboard()
{
  // Checks the clipboard against another data-control client, wl-clipboard,
  // see compositor_sharesBetweenClients for how to run it
  const auto display = qgetenv("DESKFLOW_TEST_WAYLAND_DISPLAY");
  if (display.isEmpty()) {
    QSKIP("DESKFLOW_TEST_WAYLAND_DISPLAY is not set");
  }
  const auto copyProgram = QStandardPaths::findExecutable("wl-copy");
  const auto pasteProgram = QStandardPaths::findExecutable("wl-paste");
  if (copyProgram.isEmpty() || pasteProgram.isEmpty()) {
    QSKIP("wl-clipboard is not installed");
  }
  qputenv("WAYLAND_DISPLAY", display);

  constexpr std::size_t kLimit = 16 * 1024 * 1024;
  Recorder recorded;
  auto clipboard = WlDataControlClipboard::create(
      [&recorded](Formats formats, std::uint64_t serial) { recorded.add(std::move(formats), serial); }, kLimit,
      WlDataControlClipboard::configFromEnvironment().protocol, false
  );
  QVERIFY2(clipboard, "the compositor has no data control");

  // text copied by another program is read and normalized
  auto textCopy = std::make_unique<WlCopy>(copyProgram, "text/plain;charset=utf-8", "from wl-copy\r\nline two");
  QTRY_COMPARE_WITH_TIMEOUT(recorded.count(), std::size_t{1}, 5000);
  QCOMPARE(recorded.last().first[idx(Format::Text)], std::string{"from wl-copy\nline two"});
  QVERIFY(!recorded.last().first[idx(Format::HTML)]);
  QVERIFY(!recorded.last().first[idx(Format::Bitmap)]);

  // html
  auto htmlCopy = std::make_unique<WlCopy>(copyProgram, "text/html", "<i>from wl-copy</i>");
  QTRY_COMPARE_WITH_TIMEOUT(recorded.count(), std::size_t{2}, 5000);
  QVERIFY(recorded.last().first[idx(Format::HTML)]);
  QVERIFY(recorded.last().first[idx(Format::HTML)]->ends_with("<i>from wl-copy</i>"));
  QVERIFY(textCopy->waitForExit(5000));

  // an image
  const auto png = encode(testImage(), "PNG");
  auto imageCopy = std::make_unique<WlCopy>(copyProgram, "image/png", QByteArray::fromStdString(png));
  QTRY_COMPARE_WITH_TIMEOUT(recorded.count(), std::size_t{3}, 5000);
  QCOMPARE(recorded.last().first[idx(Format::Bitmap)], WlDataControlClipboard::bitmapFromImage(png, kLimit));
  QVERIFY(htmlCopy->waitForExit(5000));

  // data larger than a pipe buffer
  std::string large(3 * 1024 * 1024, 'x');
  for (std::size_t i = 0; i < large.size(); i += 1000) {
    large[i] = static_cast<char>('a' + (i / 1000) % 26);
  }
  auto largeCopy = std::make_unique<WlCopy>(copyProgram, "text/plain;charset=utf-8", QByteArray::fromStdString(large));
  QTRY_COMPARE_WITH_TIMEOUT(recorded.count(), std::size_t{4}, 10000);
  QCOMPARE(recorded.last().first[idx(Format::Text)], large);
  QVERIFY(imageCopy->waitForExit(5000));

  // a selection set by deskflow can be pasted by other programs
  Formats formats;
  formats[idx(Format::Text)] = "hello from deskflow";
  formats[idx(Format::HTML)] = "<b>hello</b>";
  formats[idx(Format::Bitmap)] = WlDataControlClipboard::bitmapFromImage(png, kLimit);
  clipboard->setSelection(formats);
  QVERIFY(largeCopy->waitForExit(5000));

  const auto types = wlPaste(pasteProgram, {"--list-types"});
  QVERIFY(types);
  for (const auto *type :
       {"text/plain;charset=utf-8", "text/plain", "text/html", "image/png", WlDataControlClipboard::kMarkerMimeType}) {
    QVERIFY2(types->split('\n').contains(type), type);
  }
  QCOMPARE(
      wlPaste(pasteProgram, {"--no-newline", "--type", "text/plain;charset=utf-8"}).value_or(QByteArray{}),
      QByteArray{"hello from deskflow"}
  );
  const auto html = wlPaste(pasteProgram, {"--no-newline", "--type", "text/html"});
  QVERIFY(html && html->endsWith("<b>hello</b>"));
  const auto pasted = wlPaste(pasteProgram, {"--type", "image/png"});
  QVERIFY(pasted);
  QCOMPARE(QImage::fromData(*pasted, "PNG").convertToFormat(QImage::Format_ARGB32), testImage());

  Formats largeFormats;
  largeFormats[idx(Format::Text)] = large;
  clipboard->setSelection(largeFormats);
  QTRY_COMPARE_WITH_TIMEOUT(
      wlPaste(pasteProgram, {"--no-newline", "--type", "text/plain;charset=utf-8"}).value_or(QByteArray{}).size(),
      qsizetype(large.size()), 5000
  );
  QVERIFY(
      wlPaste(pasteProgram, {"--no-newline", "--type", "text/plain;charset=utf-8"}).value_or(QByteArray{}) ==
      QByteArray::fromStdString(large)
  );

  // deskflow's own selections and pastes aren't reported as changes
  QTest::qWait(300);
  QCOMPARE(recorded.count(), std::size_t{4});

  clipboard.reset();
}

QTEST_GUILESS_MAIN(WlDataControlClipboardTests)
