/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/WlDataControlClipboard.h"

#include "base/Log.h"

#include <QBuffer>
#include <QByteArray>
#include <QImage>
#include <QImageReader>
#include <QString>

#include <wayland-client.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <system_error>
#include <thread>
#include <unistd.h>

namespace deskflow {

namespace {

using Clock = std::chrono::steady_clock;

// Selections bigger than this are never read, whatever the configured limit
constexpr std::size_t kReadCap = 128 * 1024 * 1024;

// A transfer that makes no progress for this long is abandoned
constexpr auto kTransferTimeout = std::chrono::seconds(5);

// Other clients reading our selection at the same time
constexpr std::size_t kMaxWriters = 32;

constexpr std::size_t kChunkSize = 64 * 1024;
constexpr std::size_t kDibHeaderSize = 40;
constexpr std::size_t kBmpFileHeaderSize = 14;

// Mime types announced by one offer that are remembered
constexpr std::size_t kMaxOfferMimeTypes = 256;

constexpr const char *kTextMimeType = "text/plain;charset=utf-8";
constexpr const char *kHtmlMimeType = "text/html";
constexpr const char *kPngMimeType = "image/png";
constexpr const char *kHtmlCharsetMeta = R"(<meta http-equiv="content-type" content="text/html; charset=utf-8">)";

//
// Interface tables for ext-data-control-v1 and wlr-data-control-unstable-v1,
// equivalent to what wayland-scanner generates from the protocol XML. They are
// written out here so that building doesn't need wayland-scanner or the
// wlr-protocols package. Both protocols have the same requests and events,
// only the names and versions differ.
//

// request opcodes
constexpr std::uint32_t kManagerCreateDataSource = 0;
constexpr std::uint32_t kManagerGetDataDevice = 1;
constexpr std::uint32_t kManagerDestroy = 2;
constexpr std::uint32_t kDeviceSetSelection = 0;
constexpr std::uint32_t kDeviceDestroy = 1;
constexpr std::uint32_t kSourceOffer = 0;
constexpr std::uint32_t kSourceDestroy = 1;
constexpr std::uint32_t kOfferReceive = 0;
constexpr std::uint32_t kOfferDestroy = 1;

const wl_interface *nullTypes[] = {nullptr, nullptr};

// ext-data-control-v1

const wl_message extSourceRequests[] = {
    {"offer", "s", nullTypes},
    {"destroy", "", nullTypes},
};
const wl_message extSourceEvents[] = {
    {"send", "sh", nullTypes},
    {"cancelled", "", nullTypes},
};
const wl_interface extSourceInterface = {"ext_data_control_source_v1", 1, 2, extSourceRequests, 2, extSourceEvents};

const wl_message extOfferRequests[] = {
    {"receive", "sh", nullTypes},
    {"destroy", "", nullTypes},
};
const wl_message extOfferEvents[] = {
    {"offer", "s", nullTypes},
};
const wl_interface extOfferInterface = {"ext_data_control_offer_v1", 1, 2, extOfferRequests, 1, extOfferEvents};

const wl_interface *extSourceTypes[] = {&extSourceInterface};
const wl_interface *extOfferTypes[] = {&extOfferInterface};

const wl_message extDeviceRequests[] = {
    {"set_selection", "?o", extSourceTypes},
    {"destroy", "", nullTypes},
    {"set_primary_selection", "?o", extSourceTypes},
};
const wl_message extDeviceEvents[] = {
    {"data_offer", "n", extOfferTypes},
    {"selection", "?o", extOfferTypes},
    {"finished", "", nullTypes},
    {"primary_selection", "?o", extOfferTypes},
};
const wl_interface extDeviceInterface = {"ext_data_control_device_v1", 1, 3, extDeviceRequests, 4, extDeviceEvents};

const wl_interface *extGetDeviceTypes[] = {&extDeviceInterface, &wl_seat_interface};

const wl_message extManagerRequests[] = {
    {"create_data_source", "n", extSourceTypes},
    {"get_data_device", "no", extGetDeviceTypes},
    {"destroy", "", nullTypes},
};
const wl_interface extManagerInterface = {"ext_data_control_manager_v1", 1, 3, extManagerRequests, 0, nullptr};

// wlr-data-control-unstable-v1

const wl_message wlrSourceRequests[] = {
    {"offer", "s", nullTypes},
    {"destroy", "", nullTypes},
};
const wl_message wlrSourceEvents[] = {
    {"send", "sh", nullTypes},
    {"cancelled", "", nullTypes},
};
const wl_interface wlrSourceInterface = {"zwlr_data_control_source_v1", 1, 2, wlrSourceRequests, 2, wlrSourceEvents};

const wl_message wlrOfferRequests[] = {
    {"receive", "sh", nullTypes},
    {"destroy", "", nullTypes},
};
const wl_message wlrOfferEvents[] = {
    {"offer", "s", nullTypes},
};
const wl_interface wlrOfferInterface = {"zwlr_data_control_offer_v1", 1, 2, wlrOfferRequests, 1, wlrOfferEvents};

const wl_interface *wlrSourceTypes[] = {&wlrSourceInterface};
const wl_interface *wlrOfferTypes[] = {&wlrOfferInterface};

const wl_message wlrDeviceRequests[] = {
    {"set_selection", "?o", wlrSourceTypes},
    {"destroy", "", nullTypes},
    {"set_primary_selection", "2?o", wlrSourceTypes},
};
const wl_message wlrDeviceEvents[] = {
    {"data_offer", "n", wlrOfferTypes},
    {"selection", "?o", wlrOfferTypes},
    {"finished", "", nullTypes},
    {"primary_selection", "2?o", wlrOfferTypes},
};
const wl_interface wlrDeviceInterface = {"zwlr_data_control_device_v1", 2, 3, wlrDeviceRequests, 4, wlrDeviceEvents};

const wl_interface *wlrGetDeviceTypes[] = {&wlrDeviceInterface, &wl_seat_interface};

const wl_message wlrManagerRequests[] = {
    {"create_data_source", "n", wlrSourceTypes},
    {"get_data_device", "no", wlrGetDeviceTypes},
    {"destroy", "", nullTypes},
};
const wl_interface wlrManagerInterface = {"zwlr_data_control_manager_v1", 2, 3, wlrManagerRequests, 0, nullptr};

struct ProtocolTables
{
  const char *name;
  const wl_interface *manager;
  const wl_interface *source;
};

const ProtocolTables kExtTables = {"ext-data-control-v1", &extManagerInterface, &extSourceInterface};
const ProtocolTables kWlrTables = {"wlr-data-control-unstable-v1", &wlrManagerInterface, &wlrSourceInterface};

// Event listeners, laid out like the ones wayland-scanner generates
struct DeviceListener
{
  void (*dataOffer)(void *data, wl_proxy *device, wl_proxy *offer);
  void (*selection)(void *data, wl_proxy *device, wl_proxy *offer);
  void (*finished)(void *data, wl_proxy *device);
  void (*primarySelection)(void *data, wl_proxy *device, wl_proxy *offer);
};

struct OfferListener
{
  void (*offer)(void *data, wl_proxy *offer, const char *mimeType);
};

struct SourceListener
{
  void (*send)(void *data, wl_proxy *source, const char *mimeType, std::int32_t fd);
  void (*cancelled)(void *data, wl_proxy *source);
};

template <typename Listener> int addListener(wl_proxy *proxy, const Listener *listener, void *data)
{
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): how libwayland takes listeners
  return wl_proxy_add_listener(proxy, reinterpret_cast<void (**)(void)>(const_cast<Listener *>(listener)), data);
}

void destroyProxy(wl_proxy *proxy, std::uint32_t destructorOpcode)
{
  wl_proxy_marshal_flags(proxy, destructorOpcode, nullptr, wl_proxy_get_version(proxy), WL_MARSHAL_FLAG_DESTROY);
}

void closeFd(int &fd)
{
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

bool setNonBlocking(int fd)
{
  const int flags = fcntl(fd, F_GETFL);
  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// Writing to a pipe whose reader went away raises SIGPIPE, which is blocked
// in the worker thread; this discards it once the write has failed with EPIPE.
void discardPendingSigpipe()
{
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGPIPE);
  const timespec noWait = {0, 0};
  while (sigtimedwait(&set, nullptr, &noWait) > 0) {
    // keep going until none is pending
  }
}

const char *formatName(IClipboard::Format format)
{
  switch (format) {
  case IClipboard::Format::Text:
    return "text";
  case IClipboard::Format::HTML:
    return "html";
  case IClipboard::Format::Bitmap:
    return "bitmap";
  default:
    return "unknown";
  }
}

constexpr std::size_t index(IClipboard::Format format)
{
  return static_cast<std::size_t>(format);
}

// Lower case without blanks, so "text/plain; charset=UTF-8" matches "text/plain;charset=utf-8"
std::string normalized(std::string_view value)
{
  std::string result;
  result.reserve(value.size());
  for (const char c : value) {
    if (c == ' ' || c == '\t') {
      continue;
    }
    result.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
  }
  return result;
}

// Replaces invalid UTF-8 sequences with U+FFFD
std::string validUtf8(std::string text)
{
  const auto *bytes = reinterpret_cast<const unsigned char *>(text.data());
  const std::size_t size = text.size();

  // length of the valid sequence at i, 0 if invalid
  const auto sequenceLength = [bytes, size](std::size_t i) -> std::size_t {
    const unsigned char lead = bytes[i];
    if (lead < 0x80) {
      return 1;
    }
    std::size_t length = 0;
    unsigned char low = 0x80;
    unsigned char high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
      length = 2;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
      length = 3;
      if (lead == 0xE0) {
        low = 0xA0;
      } else if (lead == 0xED) {
        high = 0x9F;
      }
    } else if (lead >= 0xF0 && lead <= 0xF4) {
      length = 4;
      if (lead == 0xF0) {
        low = 0x90;
      } else if (lead == 0xF4) {
        high = 0x8F;
      }
    } else {
      return 0;
    }
    if (i + length > size || bytes[i + 1] < low || bytes[i + 1] > high) {
      return 0;
    }
    for (std::size_t j = 2; j < length; ++j) {
      if ((bytes[i + j] & 0xC0) != 0x80) {
        return 0;
      }
    }
    return length;
  };

  std::size_t i = 0;
  while (i < size) {
    const auto length = sequenceLength(i);
    if (length == 0) {
      break;
    }
    i += length;
  }
  if (i == size) {
    return text;
  }

  std::string result(text, 0, i);
  result.reserve(size + 16);
  while (i < size) {
    if (const auto length = sequenceLength(i); length != 0) {
      result.append(text, i, length);
      i += length;
    } else {
      result.append("\xEF\xBF\xBD");
      ++i;
    }
  }
  return result;
}

std::string crlfToLf(std::string text)
{
  if (text.find("\r\n") == std::string::npos) {
    return text;
  }
  std::string result;
  result.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
      continue;
    }
    result.push_back(text[i]);
  }
  return result;
}

std::uint32_t readLe32(std::string_view data, std::size_t offset)
{
  return static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset])) |
         static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 1])) << 8 |
         static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 2])) << 16 |
         static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 3])) << 24;
}

std::uint16_t readLe16(std::string_view data, std::size_t offset)
{
  return static_cast<std::uint16_t>(
      static_cast<unsigned char>(data[offset]) | static_cast<unsigned char>(data[offset + 1]) << 8
  );
}

template <typename Buffer> void appendLe32(Buffer &buffer, std::uint32_t value)
{
  for (int shift = 0; shift < 32; shift += 8) {
    buffer.push_back(static_cast<char>((value >> shift) & 0xFF));
  }
}

template <typename Buffer> void appendLe16(Buffer &buffer, std::uint16_t value)
{
  buffer.push_back(static_cast<char>(value & 0xFF));
  buffer.push_back(static_cast<char>((value >> 8) & 0xFF));
}

} // namespace

//
// WlDataControlClipboard::Impl
//

class WlDataControlClipboard::Impl
{
public:
  Impl(SelectionCallback callback, std::size_t maxBytes) : m_callback{std::move(callback)}, m_maxBytes{maxBytes}
  {
    // nothing else
  }
  ~Impl();

  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;

  bool connect(Protocol protocol, bool readInitialSelection);
  bool start();
  void post(std::function<void()> command);

  // run on the worker thread
  void applySelection(const Formats &formats, std::uint64_t serial);
  void forgetSelection(std::uint64_t serial);

  const char *protocolName() const
  {
    return m_tables ? m_tables->name : "none";
  }

  void setMaxBytes(std::size_t maxBytes)
  {
    m_maxBytes = maxBytes;
  }

  // numbers setSelection() and forgetSelection() calls, see isCurrent()
  std::uint64_t newRequest()
  {
    return ++m_requests;
  }

  std::uint64_t lastRequest() const
  {
    return m_requests;
  }

private:
  // Data of one selection by mime type
  struct Selection
  {
    std::map<std::string, std::shared_ptr<const std::string>, std::less<>> data;
    std::string bitmap;                     // served as PNG
    std::shared_ptr<const std::string> png; // the bitmap as PNG, once a client has asked for it
  };

  struct Reader
  {
    wl_proxy *offer = nullptr;
    bool initial = false;                                            // the selection that existed on startup
    std::vector<std::pair<IClipboard::Format, std::string>> pending; // formats left to read
    IClipboard::Format format = IClipboard::Format::Text;
    std::string mimeType;
    int fd = -1;
    std::string buffer;
    std::size_t cap = 0;
    std::size_t maxBytes = 0;
    Clock::time_point lastProgress;
    std::shared_ptr<Selection> raw = std::make_shared<Selection>();
    std::map<IClipboard::Format, std::string> mimeTypes; // read successfully, by format
  };

  struct Writer
  {
    int fd = -1;
    std::shared_ptr<const std::string> data;
    std::size_t offset = 0;
    Clock::time_point lastProgress;
  };

  // event handlers
  static void onDataOffer(void *data, wl_proxy *device, wl_proxy *offer);
  static void onSelection(void *data, wl_proxy *device, wl_proxy *offer);
  static void onFinished(void *data, wl_proxy *device);
  static void onPrimarySelection(void *data, wl_proxy *device, wl_proxy *offer);
  static void onOffer(void *data, wl_proxy *offer, const char *mimeType);
  static void onSend(void *data, wl_proxy *source, const char *mimeType, std::int32_t fd);
  static void onCancelled(void *data, wl_proxy *source);
  static void
  onRegistryGlobal(void *data, wl_registry *registry, std::uint32_t name, const char *interface, std::uint32_t version);
  static void onRegistryGlobalRemove(void *data, wl_registry *registry, std::uint32_t name);

  static const DeviceListener s_deviceListener;
  static const OfferListener s_offerListener;
  static const SourceListener s_sourceListener;
  static const wl_registry_listener s_registryListener;

  void handleSelection(wl_proxy *offer);
  void handleSend(wl_proxy *source, const char *mimeType, int fd);
  void handleCancelled(wl_proxy *source);
  void destroyOffer(wl_proxy *offer);

  void run();
  bool step();
  int pollTimeout() const;
  void checkTimeouts();
  void runCommands();
  void reportDisplayError(const char *operation) const;
  void cleanup();
  void wake() const;

  void readSelection(wl_proxy *offer, bool initial);
  void startRead(wl_proxy *offer, std::size_t maxBytes, bool initial);
  void readNextFormat();
  void readAvailable();
  void finishFormat(bool complete);
  void finishRead();
  void abortRead();
  bool sameAsLastSeen(const Selection &selection) const;

  std::shared_ptr<const std::string> servedData(const std::string &mimeType);
  bool writeAvailable(Writer &writer) const;

  SelectionCallback m_callback;
  std::atomic<std::size_t> m_maxBytes;

  // requests made, and the last one the worker thread has handled
  std::atomic<std::uint64_t> m_requests = 0;
  std::uint64_t m_handledRequests = 0;

  const ProtocolTables *m_tables = nullptr;
  wl_display *m_display = nullptr;
  wl_registry *m_registry = nullptr;
  wl_proxy *m_seat = nullptr;
  wl_proxy *m_manager = nullptr;
  wl_proxy *m_device = nullptr;

  // registry results
  std::uint32_t m_seatName = 0;
  std::uint32_t m_extManagerName = 0;
  std::uint32_t m_wlrManagerName = 0;

  // offers made by the compositor and their mime types
  std::map<wl_proxy *, std::vector<std::string>> m_offers;

  // the selection that exists on startup is read once the worker thread runs
  bool m_connecting = true;
  bool m_readInitialSelection = true;
  wl_proxy *m_initialOffer = nullptr;

  // our selection, while it is the current one
  wl_proxy *m_source = nullptr;
  std::shared_ptr<Selection> m_served;

  // the last selection read or set, to tell whether a new one is different
  std::shared_ptr<Selection> m_lastSeen;

  std::optional<Reader> m_reader;
  std::vector<Writer> m_writers;

  std::thread m_thread;
  std::atomic<bool> m_stop = false;
  std::atomic<bool> m_running = false;
  int m_wakeRead = -1;
  int m_wakeWrite = -1;
  std::mutex m_commandMutex;
  std::vector<std::function<void()>> m_commands;
};

const DeviceListener WlDataControlClipboard::Impl::s_deviceListener = {
    &onDataOffer, &onSelection, &onFinished, &onPrimarySelection
};
const OfferListener WlDataControlClipboard::Impl::s_offerListener = {&onOffer};
const SourceListener WlDataControlClipboard::Impl::s_sourceListener = {&onSend, &onCancelled};
const wl_registry_listener WlDataControlClipboard::Impl::s_registryListener = {
    &onRegistryGlobal, &onRegistryGlobalRemove
};

WlDataControlClipboard::Impl::~Impl()
{
  if (m_thread.joinable()) {
    m_stop = true;
    wake();
    m_thread.join();
  } else {
    cleanup();
  }
  closeFd(m_wakeRead);
  closeFd(m_wakeWrite);
}

bool WlDataControlClipboard::Impl::connect(Protocol protocol, bool readInitialSelection)
{
  m_display = wl_display_connect(nullptr);
  if (!m_display) {
    LOG_DEBUG("wayland clipboard: can't connect to the wayland display: %s", strerror(errno));
    return false;
  }

  m_registry = wl_display_get_registry(m_display);
  if (!m_registry) {
    return false;
  }
  wl_registry_add_listener(m_registry, &s_registryListener, this);
  if (wl_display_roundtrip(m_display) < 0) {
    reportDisplayError("roundtrip");
    return false;
  }

  std::uint32_t managerName = 0;
  if (protocol != Protocol::Wlr && m_extManagerName != 0) {
    m_tables = &kExtTables;
    managerName = m_extManagerName;
  } else if (protocol != Protocol::Ext && m_wlrManagerName != 0) {
    m_tables = &kWlrTables;
    managerName = m_wlrManagerName;
  } else {
    LOG_DEBUG(
        "wayland clipboard: the compositor has no %s", protocol == Protocol::Ext   ? "ext-data-control-v1"
                                                       : protocol == Protocol::Wlr ? "wlr-data-control-unstable-v1"
                                                                                   : "data control protocol"
    );
    return false;
  }
  if (m_seatName == 0) {
    LOG_DEBUG("wayland clipboard: the compositor has no seat");
    return false;
  }

  m_seat = static_cast<wl_proxy *>(wl_registry_bind(m_registry, m_seatName, &wl_seat_interface, 1));
  m_manager = static_cast<wl_proxy *>(wl_registry_bind(m_registry, managerName, m_tables->manager, 1));
  wl_registry_destroy(m_registry);
  m_registry = nullptr;
  if (!m_seat || !m_manager) {
    return false;
  }

  const wl_interface *deviceInterface = m_tables->manager->methods[kManagerGetDataDevice].types[0];
  m_device = wl_proxy_marshal_flags(
      m_manager, kManagerGetDataDevice, deviceInterface, wl_proxy_get_version(m_manager), 0, nullptr, m_seat
  );
  if (!m_device) {
    return false;
  }
  addListener(m_device, &s_deviceListener, this);

  // the compositor sends the current selection right away, and a roundtrip
  // shows protocol errors here instead of later on the worker thread
  m_readInitialSelection = readInitialSelection;
  if (wl_display_roundtrip(m_display) < 0) {
    reportDisplayError("roundtrip");
    return false;
  }

  int wakePipe[2];
  if (pipe2(wakePipe, O_CLOEXEC | O_NONBLOCK) != 0) {
    LOG_ERR("wayland clipboard: can't create a pipe: %s", strerror(errno));
    return false;
  }
  m_wakeRead = wakePipe[0];
  m_wakeWrite = wakePipe[1];
  return true;
}

bool WlDataControlClipboard::Impl::start()
{
  m_running = true;
  try {
    m_thread = std::thread([this] { run(); });
  } catch (const std::system_error &e) {
    LOG_ERR("wayland clipboard: can't start thread: %s", e.what());
    m_running = false;
    return false;
  }
  return true;
}

void WlDataControlClipboard::Impl::post(std::function<void()> command)
{
  if (!m_running) {
    return;
  }
  {
    std::scoped_lock lock{m_commandMutex};
    m_commands.push_back(std::move(command));
  }
  wake();
}

void WlDataControlClipboard::Impl::wake() const
{
  if (m_wakeWrite >= 0) {
    const char byte = 0;
    // when the pipe is full a wakeup is pending anyway
    [[maybe_unused]] const auto written = ::write(m_wakeWrite, &byte, 1);
  }
}

void WlDataControlClipboard::Impl::runCommands()
{
  std::vector<std::function<void()>> commands;
  {
    std::scoped_lock lock{m_commandMutex};
    commands.swap(m_commands);
  }
  for (const auto &command : commands) {
    command();
  }
}

void WlDataControlClipboard::Impl::run()
{
#ifdef __linux__
  pthread_setname_np(pthread_self(), "wl-clipboard");
#endif

  // a client that closes its end of a pipe early must not kill deskflow
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &set, nullptr);

  // so the selection callback only ever runs on this thread
  m_connecting = false;
  if (auto *offer = std::exchange(m_initialOffer, nullptr); offer) {
    readSelection(offer, true);
  }

  while (!m_stop && step()) {
    // keep going
  }

  m_running = false;
  cleanup();
  LOG_DEBUG("wayland clipboard: stopped");
}

bool WlDataControlClipboard::Impl::step()
{
  runCommands();

  while (wl_display_prepare_read(m_display) != 0) {
    if (wl_display_dispatch_pending(m_display) < 0) {
      reportDisplayError("dispatch");
      return false;
    }
  }

  bool flushPending = false;
  if (wl_display_flush(m_display) < 0) {
    if (errno == EAGAIN) {
      flushPending = true;
    } else {
      wl_display_cancel_read(m_display);
      reportDisplayError("flush");
      return false;
    }
  }

  // reader and writers are polled after the display and the wake pipe
  std::vector<pollfd> fds;
  fds.reserve(3 + m_writers.size());
  fds.push_back({wl_display_get_fd(m_display), static_cast<short>(POLLIN | (flushPending ? POLLOUT : 0)), 0});
  fds.push_back({m_wakeRead, POLLIN, 0});
  const int readerFd = m_reader ? m_reader->fd : -1;
  if (readerFd >= 0) {
    fds.push_back({readerFd, POLLIN, 0});
  }
  const std::size_t firstWriter = fds.size();
  for (const auto &writer : m_writers) {
    fds.push_back({writer.fd, POLLOUT, 0});
  }

  if (poll(fds.data(), fds.size(), pollTimeout()) < 0) {
    if (errno != EINTR) {
      LOG_ERR("wayland clipboard: poll failed: %s", strerror(errno));
      wl_display_cancel_read(m_display);
      return false;
    }
    for (auto &fd : fds) {
      fd.revents = 0;
    }
  }

  if (fds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
    if (wl_display_read_events(m_display) < 0) {
      reportDisplayError("read");
      return false;
    }
  } else {
    wl_display_cancel_read(m_display);
  }

  if (fds[1].revents & POLLIN) {
    char buffer[64];
    while (::read(m_wakeRead, buffer, sizeof(buffer)) > 0) {
      // drain
    }
  }

  // nothing has been dispatched since poll, so the reader and writers are the polled ones
  if (readerFd >= 0 && fds[2].revents != 0) {
    readAvailable();
  }
  for (std::size_t i = 0; i < m_writers.size(); ++i) {
    if (fds[firstWriter + i].revents != 0 && !writeAvailable(m_writers[i])) {
      closeFd(m_writers[i].fd);
    }
  }
  std::erase_if(m_writers, [](const Writer &writer) { return writer.fd < 0; });

  checkTimeouts();

  if (wl_display_dispatch_pending(m_display) < 0) {
    reportDisplayError("dispatch");
    return false;
  }
  return m_device != nullptr;
}

int WlDataControlClipboard::Impl::pollTimeout() const
{
  std::optional<Clock::time_point> deadline;
  if (m_reader && m_reader->fd >= 0) {
    deadline = m_reader->lastProgress + kTransferTimeout;
  }
  for (const auto &writer : m_writers) {
    const auto writerDeadline = writer.lastProgress + kTransferTimeout;
    if (!deadline || writerDeadline < *deadline) {
      deadline = writerDeadline;
    }
  }
  if (!deadline) {
    return -1;
  }
  const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(*deadline - Clock::now()).count();
  return static_cast<int>(std::clamp<decltype(remaining)>(remaining, 0, 60 * 1000));
}

void WlDataControlClipboard::Impl::checkTimeouts()
{
  const auto now = Clock::now();
  if (m_reader && m_reader->fd >= 0 && now - m_reader->lastProgress >= kTransferTimeout) {
    LOG_WARN("wayland clipboard: timed out reading %s from the selection", m_reader->mimeType.c_str());
    finishFormat(false);
  }
  for (auto &writer : m_writers) {
    if (now - writer.lastProgress >= kTransferTimeout) {
      LOG_DEBUG("wayland clipboard: timed out sending the selection to another client");
      closeFd(writer.fd);
    }
  }
  std::erase_if(m_writers, [](const Writer &writer) { return writer.fd < 0; });
}

void WlDataControlClipboard::Impl::reportDisplayError(const char *operation) const
{
  const int error = wl_display_get_error(m_display);
  if (error == EPROTO) {
    const wl_interface *interface = nullptr;
    std::uint32_t id = 0;
    const auto code = wl_display_get_protocol_error(m_display, &interface, &id);
    LOG_ERR(
        "wayland clipboard: protocol error %u on %s@%u during %s", code, interface ? interface->name : "unknown", id,
        operation
    );
  } else {
    LOG_WARN(
        "wayland clipboard: lost the connection to the compositor during %s: %s", operation,
        strerror(error != 0 ? error : errno)
    );
  }
}

void WlDataControlClipboard::Impl::cleanup()
{
  abortRead();
  for (auto &writer : m_writers) {
    closeFd(writer.fd);
  }
  m_writers.clear();
  for (const auto &[offer, mimeTypes] : m_offers) {
    destroyProxy(offer, kOfferDestroy);
  }
  m_offers.clear();
  if (m_source) {
    destroyProxy(m_source, kSourceDestroy);
    m_source = nullptr;
  }
  m_served.reset();
  if (m_device) {
    destroyProxy(m_device, kDeviceDestroy);
    m_device = nullptr;
  }
  if (m_manager) {
    destroyProxy(m_manager, kManagerDestroy);
    m_manager = nullptr;
  }
  if (m_seat) {
    // wl_seat version 1 has no release request
    wl_proxy_destroy(m_seat);
    m_seat = nullptr;
  }
  if (m_registry) {
    wl_registry_destroy(m_registry);
    m_registry = nullptr;
  }
  if (m_display) {
    wl_display_flush(m_display);
    wl_display_disconnect(m_display);
    m_display = nullptr;
  }
}

//
// Wayland events
//

void WlDataControlClipboard::Impl::onRegistryGlobal(
    void *data, wl_registry *, std::uint32_t name, const char *interface, std::uint32_t version
)
{
  auto *impl = static_cast<Impl *>(data);
  const std::string_view interfaceName = interface;
  if (interfaceName == wl_seat_interface.name && impl->m_seatName == 0) {
    impl->m_seatName = name;
  } else if (interfaceName == extManagerInterface.name && version >= 1) {
    impl->m_extManagerName = name;
  } else if (interfaceName == wlrManagerInterface.name && version >= 1) {
    impl->m_wlrManagerName = name;
  }
}

void WlDataControlClipboard::Impl::onRegistryGlobalRemove(void *, wl_registry *, std::uint32_t)
{
  // the registry is only used while connecting
}

void WlDataControlClipboard::Impl::onDataOffer(void *data, wl_proxy *, wl_proxy *offer)
{
  auto *impl = static_cast<Impl *>(data);
  if (!offer) {
    return;
  }
  impl->m_offers[offer];
  addListener(offer, &s_offerListener, impl);
}

void WlDataControlClipboard::Impl::onOffer(void *data, wl_proxy *offer, const char *mimeType)
{
  auto *impl = static_cast<Impl *>(data);
  if (auto it = impl->m_offers.find(offer);
      it != impl->m_offers.end() && mimeType && it->second.size() < kMaxOfferMimeTypes) {
    it->second.emplace_back(mimeType);
  }
}

void WlDataControlClipboard::Impl::onSelection(void *data, wl_proxy *, wl_proxy *offer)
{
  static_cast<Impl *>(data)->handleSelection(offer);
}

void WlDataControlClipboard::Impl::onPrimarySelection(void *data, wl_proxy *, wl_proxy *offer)
{
  // the primary selection (middle click paste) isn't shared
  if (offer) {
    static_cast<Impl *>(data)->destroyOffer(offer);
  }
}

void WlDataControlClipboard::Impl::onFinished(void *data, wl_proxy *)
{
  auto *impl = static_cast<Impl *>(data);
  LOG_WARN("wayland clipboard: the compositor ended clipboard access, the clipboard won't be shared");
  impl->abortRead();
  destroyProxy(impl->m_device, kDeviceDestroy);
  impl->m_device = nullptr;
}

void WlDataControlClipboard::Impl::onSend(void *data, wl_proxy *source, const char *mimeType, std::int32_t fd)
{
  static_cast<Impl *>(data)->handleSend(source, mimeType ? mimeType : "", fd);
}

void WlDataControlClipboard::Impl::onCancelled(void *data, wl_proxy *source)
{
  static_cast<Impl *>(data)->handleCancelled(source);
}

void WlDataControlClipboard::Impl::destroyOffer(wl_proxy *offer)
{
  if (m_reader && m_reader->offer == offer) {
    abortRead();
  }
  if (m_initialOffer == offer) {
    m_initialOffer = nullptr;
  }
  m_offers.erase(offer);
  destroyProxy(offer, kOfferDestroy);
}

void WlDataControlClipboard::Impl::handleSelection(wl_proxy *offer)
{
  abortRead();
  m_initialOffer = nullptr;

  // a new selection invalidates all earlier offers
  for (auto it = m_offers.begin(); it != m_offers.end();) {
    if (it->first == offer) {
      ++it;
      continue;
    }
    destroyProxy(it->first, kOfferDestroy);
    it = m_offers.erase(it);
  }

  if (!offer) {
    // the owner went away; there's nothing new to share
    LOG_DEBUG("wayland clipboard: selection cleared");
    return;
  }

  if (m_connecting) {
    if (m_readInitialSelection) {
      m_initialOffer = offer;
    }
    return;
  }

  readSelection(offer, false);
}

//
// Reading the selection of other clients
//

void WlDataControlClipboard::Impl::readSelection(wl_proxy *offer, bool initial)
{
  const auto &mimeTypes = m_offers[offer];
  const bool hasMarker = std::ranges::find(mimeTypes, std::string_view{kMarkerMimeType}) != mimeTypes.end();
  if (hasMarker && m_source) {
    LOG_DEBUG("wayland clipboard: selection is our own");
    return;
  }

  const std::size_t maxBytes = m_maxBytes;
  if (maxBytes == 0) {
    LOG_DEBUG("wayland clipboard: clipboard sharing is off, not reading the selection");
    return;
  }

  startRead(offer, maxBytes, initial);
}

void WlDataControlClipboard::Impl::startRead(wl_proxy *offer, std::size_t maxBytes, bool initial)
{
  const auto &mimeTypes = m_offers[offer];
  const auto chosen = chooseMimeTypes(mimeTypes);

  Reader reader;
  reader.offer = offer;
  reader.initial = initial;
  reader.maxBytes = maxBytes;
  reader.cap = std::min(maxBytes, kReadCap);
  for (std::size_t i = 0; i < kFormatCount; ++i) {
    if (chosen[i]) {
      reader.pending.emplace_back(static_cast<IClipboard::Format>(i), *chosen[i]);
    }
  }

  if (reader.pending.empty()) {
    std::string offered;
    for (const auto &mimeType : mimeTypes) {
      offered += offered.empty() ? mimeType : ", " + mimeType;
    }
    LOG_DEBUG("wayland clipboard: no supported format in the selection (%s)", offered.c_str());
  }

  m_reader = std::move(reader);
  readNextFormat();
}

void WlDataControlClipboard::Impl::readNextFormat()
{
  while (m_reader && !m_reader->pending.empty()) {
    auto [format, mimeType] = std::move(m_reader->pending.front());
    m_reader->pending.erase(m_reader->pending.begin());

    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
      LOG_ERR("wayland clipboard: can't create a pipe: %s", strerror(errno));
      continue;
    }
    if (!setNonBlocking(fds[0])) {
      ::close(fds[0]);
      ::close(fds[1]);
      continue;
    }

    // libwayland sends a duplicate of the write end, so ours is closed right away
    wl_proxy_marshal_flags(
        m_reader->offer, kOfferReceive, nullptr, wl_proxy_get_version(m_reader->offer), 0, mimeType.c_str(), fds[1]
    );
    ::close(fds[1]);

    m_reader->format = format;
    m_reader->mimeType = std::move(mimeType);
    m_reader->fd = fds[0];
    m_reader->buffer.clear();
    m_reader->lastProgress = Clock::now();
    LOG_VERBOSE("wayland clipboard: reading %s", m_reader->mimeType.c_str());
    return;
  }

  if (m_reader) {
    finishRead();
  }
}

void WlDataControlClipboard::Impl::readAvailable()
{
  char chunk[kChunkSize];
  while (m_reader && m_reader->fd >= 0) {
    const auto n = ::read(m_reader->fd, chunk, sizeof(chunk));
    if (n > 0) {
      if (m_reader->buffer.size() + static_cast<std::size_t>(n) > m_reader->cap) {
        LOG_WARN(
            "wayland clipboard: %s in the selection is over the %zu byte limit, not sharing it",
            m_reader->mimeType.c_str(), m_reader->cap
        );
        finishFormat(false);
        return;
      }
      m_reader->buffer.append(chunk, static_cast<std::size_t>(n));
      m_reader->lastProgress = Clock::now();
    } else if (n == 0) {
      finishFormat(true);
      return;
    } else if (errno == EINTR) {
      continue;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    } else {
      LOG_WARN("wayland clipboard: can't read %s: %s", m_reader->mimeType.c_str(), strerror(errno));
      finishFormat(false);
      return;
    }
  }
}

void WlDataControlClipboard::Impl::finishFormat(bool complete)
{
  closeFd(m_reader->fd);
  if (complete) {
    LOG_VERBOSE("wayland clipboard: read %zu bytes of %s", m_reader->buffer.size(), m_reader->mimeType.c_str());
    m_reader->mimeTypes[m_reader->format] = m_reader->mimeType;
    m_reader->raw->data[m_reader->mimeType] = std::make_shared<const std::string>(std::move(m_reader->buffer));
  }
  m_reader->buffer = {};
  readNextFormat();
}

void WlDataControlClipboard::Impl::finishRead()
{
  Reader reader = std::move(*m_reader);
  m_reader.reset();

  // a request that's still queued was made after this selection was set, so it wins
  const auto serial = m_requests.load();
  if (serial != m_handledRequests) {
    LOG_DEBUG("wayland clipboard: not sharing the selection, the clipboard changed while reading it");
    return;
  }

  if (sameAsLastSeen(*reader.raw)) {
    LOG_DEBUG("wayland clipboard: selection changed but its content didn't, not sharing it again");
    return;
  }

  // the selection found on startup was never shared, so sharing it later must not count as a repeat
  m_lastSeen = reader.initial ? nullptr : reader.raw;

  const auto raw = [&reader](IClipboard::Format format) -> const std::string * {
    const auto mimeType = reader.mimeTypes.find(format);
    if (mimeType == reader.mimeTypes.end()) {
      return nullptr;
    }
    const auto data = reader.raw->data.find(mimeType->second);
    return data == reader.raw->data.end() ? nullptr : data->second.get();
  };

  Formats formats;
  if (const auto *text = raw(IClipboard::Format::Text); text) {
    if (auto converted = textFromSelection(*text); !converted.empty()) {
      formats[index(IClipboard::Format::Text)] = std::move(converted);
    }
  }
  if (const auto *html = raw(IClipboard::Format::HTML); html) {
    if (auto converted = htmlFromSelection(*html); !converted.empty()) {
      formats[index(IClipboard::Format::HTML)] = std::move(converted);
    }
  }
  if (const auto *image = raw(IClipboard::Format::Bitmap); image) {
    if (auto bitmap = bitmapFromImage(*image, reader.maxBytes); !bitmap.empty()) {
      formats[index(IClipboard::Format::Bitmap)] = std::move(bitmap);
    } else {
      LOG_WARN(
          "wayland clipboard: can't share the %s image in the selection, it can't be decoded or is over the size "
          "limit",
          reader.mimeTypes[IClipboard::Format::Bitmap].c_str()
      );
    }
  }

  for (const auto format : fitToLimit(formats, reader.maxBytes)) {
    LOG_WARN(
        "wayland clipboard: not sharing %s from the selection, the clipboard would be over the size limit",
        formatName(format)
    );
  }

  std::string names;
  for (std::size_t i = 0; i < kFormatCount; ++i) {
    if (formats[i]) {
      names += names.empty() ? "" : ", ";
      names += formatName(static_cast<IClipboard::Format>(i));
    }
  }
  LOG_DEBUG(
      "wayland clipboard: selection changed, %zu bytes (%s)", marshalledSize(formats),
      names.empty() ? "no supported data" : names.c_str()
  );

  if (m_callback) {
    m_callback(std::move(formats), serial);
  }
}

void WlDataControlClipboard::Impl::abortRead()
{
  if (m_reader) {
    closeFd(m_reader->fd);
    m_reader.reset();
  }
}

bool WlDataControlClipboard::Impl::sameAsLastSeen(const Selection &selection) const
{
  if (!m_lastSeen) {
    return false;
  }
  bool any = false;
  for (const auto &[mimeType, data] : selection.data) {
    if (data->empty()) {
      continue;
    }
    const std::string *seen = nullptr;
    if (const auto it = m_lastSeen->data.find(mimeType); it != m_lastSeen->data.end()) {
      seen = it->second.get();
    } else if (mimeType == kPngMimeType && m_lastSeen->png) {
      // a clipboard manager that copied our selection read the image as PNG
      seen = m_lastSeen->png.get();
    }
    if (!seen || *seen != *data) {
      return false;
    }
    any = true;
  }
  return any;
}

//
// Serving our selection to other clients
//

void WlDataControlClipboard::Impl::applySelection(const Formats &formats, std::uint64_t serial)
{
  m_handledRequests = serial;
  if (!m_device) {
    return;
  }

  auto selection = std::make_shared<Selection>();
  if (const auto &text = formats[index(IClipboard::Format::Text)]; text && !text->empty()) {
    const auto data = std::make_shared<const std::string>(*text);
    for (const auto &mimeType : mimeTypesFor(IClipboard::Format::Text)) {
      selection->data[mimeType] = data;
    }
  }
  if (const auto &html = formats[index(IClipboard::Format::HTML)]; html && !html->empty()) {
    // without a charset some applications don't read HTML as UTF-8
    auto data = std::make_shared<std::string>(*html);
    if (normalized(*data).find("charset") == std::string::npos) {
      data->insert(0, kHtmlCharsetMeta);
    }
    selection->data[kHtmlMimeType] = std::move(data);
  }
  if (const auto &bitmap = formats[index(IClipboard::Format::Bitmap)]; bitmap && dibPixelOffset(*bitmap) != 0) {
    selection->bitmap = *bitmap;
  }

  if (selection->data.empty() && selection->bitmap.empty()) {
    LOG_DEBUG("wayland clipboard: no supported data to put in the selection");
    return;
  }

  // nothing to do when the same data is still being served
  if (m_source && m_served && m_served->bitmap == selection->bitmap &&
      std::ranges::equal(m_served->data, selection->data, [](const auto &a, const auto &b) {
        return a.first == b.first && *a.second == *b.second;
      })) {
    LOG_DEBUG("wayland clipboard: selection already has this data");
    m_lastSeen = m_served;
    return;
  }

  // a selection still being read is older than this one
  abortRead();

  const wl_interface *sourceInterface = m_tables->source;
  auto *source = wl_proxy_marshal_flags(
      m_manager, kManagerCreateDataSource, sourceInterface, wl_proxy_get_version(m_manager), 0, nullptr
  );
  if (!source) {
    LOG_ERR("wayland clipboard: can't create a data source");
    return;
  }
  addListener(source, &s_sourceListener, this);

  const auto offer = [source](const std::string &mimeType) {
    wl_proxy_marshal_flags(source, kSourceOffer, nullptr, wl_proxy_get_version(source), 0, mimeType.c_str());
  };
  for (const auto &[mimeType, data] : selection->data) {
    offer(mimeType);
  }
  if (!selection->bitmap.empty()) {
    offer(kPngMimeType);
  }
  offer(kMarkerMimeType);

  wl_proxy_marshal_flags(m_device, kDeviceSetSelection, nullptr, wl_proxy_get_version(m_device), 0, source);

  // replaced only now, so the selection is never briefly empty
  if (m_source) {
    destroyProxy(m_source, kSourceDestroy);
  }
  m_source = source;
  m_served = selection;
  m_lastSeen = selection;

  const auto size = [&formats](IClipboard::Format format) {
    const auto &data = formats[index(format)];
    return data ? data->size() : std::size_t{0};
  };
  LOG_DEBUG(
      "wayland clipboard: set the selection (%zu bytes of text, %zu bytes of html, %zu bytes of bitmap)",
      size(IClipboard::Format::Text), size(IClipboard::Format::HTML), selection->bitmap.size()
  );
}

void WlDataControlClipboard::Impl::forgetSelection(std::uint64_t serial)
{
  m_handledRequests = serial;

  // another computer took the clipboard after the selection being read was set
  abortRead();
  m_lastSeen.reset();
}

std::shared_ptr<const std::string> WlDataControlClipboard::Impl::servedData(const std::string &mimeType)
{
  if (const auto it = m_served->data.find(mimeType); it != m_served->data.end()) {
    return it->second;
  }
  if (mimeType == kPngMimeType && !m_served->bitmap.empty()) {
    if (!m_served->png) {
      m_served->png = std::make_shared<const std::string>(pngFromBitmap(m_served->bitmap));
      if (m_served->png->empty()) {
        LOG_WARN("wayland clipboard: can't convert the shared bitmap to png");
      }
    }
    return m_served->png;
  }
  return nullptr;
}

void WlDataControlClipboard::Impl::handleSend(wl_proxy *source, const char *mimeType, int fd)
{
  if (source != m_source || !m_served) {
    ::close(fd);
    return;
  }

  const auto data = servedData(mimeType);
  if (!data || data->empty()) {
    ::close(fd);
    return;
  }
  if (m_writers.size() >= kMaxWriters) {
    LOG_WARN("wayland clipboard: too many clients reading the selection at once");
    ::close(fd);
    return;
  }
  if (!setNonBlocking(fd)) {
    ::close(fd);
    return;
  }
  LOG_VERBOSE("wayland clipboard: sending %zu bytes of %s", data->size(), mimeType);

  Writer writer{fd, data, 0, Clock::now()};
  if (writeAvailable(writer)) {
    m_writers.push_back(std::move(writer));
  } else {
    closeFd(writer.fd);
  }
}

bool WlDataControlClipboard::Impl::writeAvailable(Writer &writer) const
{
  while (writer.offset < writer.data->size()) {
    const auto size = std::min(kChunkSize, writer.data->size() - writer.offset);
    const auto n = ::write(writer.fd, writer.data->data() + writer.offset, size);
    if (n > 0) {
      writer.offset += static_cast<std::size_t>(n);
      writer.lastProgress = Clock::now();
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return true;
    } else {
      if (n < 0 && errno == EPIPE) {
        discardPendingSigpipe();
      }
      LOG_VERBOSE("wayland clipboard: client stopped reading the selection");
      return false;
    }
  }
  return false;
}

void WlDataControlClipboard::Impl::handleCancelled(wl_proxy *source)
{
  // another client has set the selection
  destroyProxy(source, kSourceDestroy);
  if (source == m_source) {
    m_source = nullptr;
    m_served.reset();
  }
}

//
// WlDataControlClipboard
//

WlDataControlClipboard::WlDataControlClipboard(std::unique_ptr<Impl> impl) : m_impl{std::move(impl)}
{
  // nothing else
}

WlDataControlClipboard::~WlDataControlClipboard() = default;

std::unique_ptr<WlDataControlClipboard> WlDataControlClipboard::create(
    SelectionCallback callback, std::size_t maxBytes, Protocol protocol, bool readInitialSelection
)
{
  auto impl = std::make_unique<Impl>(std::move(callback), maxBytes);
  if (!impl->connect(protocol, readInitialSelection) || !impl->start()) {
    return nullptr;
  }
  return std::unique_ptr<WlDataControlClipboard>(new WlDataControlClipboard(std::move(impl)));
}

void WlDataControlClipboard::setSelection(const Formats &formats)
{
  const auto serial = m_impl->newRequest();
  m_impl->post([impl = m_impl.get(), formats, serial] { impl->applySelection(formats, serial); });
}

void WlDataControlClipboard::forgetSelection()
{
  const auto serial = m_impl->newRequest();
  m_impl->post([impl = m_impl.get(), serial] { impl->forgetSelection(serial); });
}

bool WlDataControlClipboard::isCurrent(std::uint64_t serial) const
{
  return m_impl->lastRequest() == serial;
}

void WlDataControlClipboard::setMaxBytes(std::size_t maxBytes)
{
  m_impl->setMaxBytes(maxBytes);
}

const char *WlDataControlClipboard::protocolName() const
{
  return m_impl->protocolName();
}

WlDataControlClipboard::Config WlDataControlClipboard::parseConfig(std::string_view value)
{
  const auto setting = normalized(value);
  const auto isOneOf = [&setting](std::initializer_list<std::string_view> words) {
    return std::ranges::find(words, setting) != words.end();
  };

  Config config;
  if (isOneOf({"", "auto"})) {
    return config;
  }
  if (isOneOf({"0", "off", "false", "no", "disable", "disabled"})) {
    config.mode = Mode::Disabled;
    return config;
  }
  if (isOneOf({"1", "on", "true", "yes", "enable", "enabled"})) {
    config.mode = Mode::Enabled;
    return config;
  }
  if (setting == "ext") {
    config.mode = Mode::Enabled;
    config.protocol = Protocol::Ext;
    return config;
  }
  if (setting == "wlr") {
    config.mode = Mode::Enabled;
    config.protocol = Protocol::Wlr;
    return config;
  }
  LOG_WARN("ignoring %s=%s, expected auto, on, off, ext or wlr", kEnvironmentVariable, std::string(value).c_str());
  return config;
}

WlDataControlClipboard::Config WlDataControlClipboard::configFromEnvironment()
{
  const char *value = std::getenv(kEnvironmentVariable);
  return parseConfig(value ? value : "");
}

std::array<std::optional<std::string>, WlDataControlClipboard::kFormatCount>
WlDataControlClipboard::chooseMimeTypes(const std::vector<std::string> &offered)
{
  const auto pick = [&offered](std::initializer_list<std::string_view> preferred) -> std::optional<std::string> {
    for (const auto wanted : preferred) {
      for (const auto &mimeType : offered) {
        if (normalized(mimeType) == wanted) {
          return mimeType;
        }
      }
    }
    return std::nullopt;
  };

  std::array<std::optional<std::string>, kFormatCount> chosen;
  chosen[index(IClipboard::Format::Text)] = pick({"text/plain;charset=utf-8", "utf8_string", "text/plain"});
  chosen[index(IClipboard::Format::HTML)] = pick({"text/html", "text/html;charset=utf-8"});
  chosen[index(IClipboard::Format::Bitmap)] =
      pick({"image/png", "image/bmp", "image/x-bmp", "image/x-ms-bmp", "image/x-win-bitmap", "image/jpeg"});
  return chosen;
}

std::vector<std::string> WlDataControlClipboard::mimeTypesFor(IClipboard::Format format)
{
  switch (format) {
  case IClipboard::Format::Text:
    return {kTextMimeType, "text/plain", "UTF8_STRING", "STRING", "TEXT"};
  case IClipboard::Format::HTML:
    return {kHtmlMimeType};
  case IClipboard::Format::Bitmap:
    return {kPngMimeType};
  default:
    return {};
  }
}

std::string WlDataControlClipboard::textFromSelection(std::string data)
{
  while (!data.empty() && data.back() == '\0') {
    data.pop_back();
  }
  return crlfToLf(validUtf8(std::move(data)));
}

std::string WlDataControlClipboard::htmlFromSelection(std::string data)
{
  const auto byte = [&data](std::size_t i) { return static_cast<unsigned char>(data[i]); };
  if (data.size() >= 2 && ((byte(0) == 0xFF && byte(1) == 0xFE) || (byte(0) == 0xFE && byte(1) == 0xFF))) {
    // some browsers put UTF-16 HTML on the clipboard
    const bool littleEndian = byte(0) == 0xFF;
    std::u16string utf16;
    utf16.reserve(data.size() / 2);
    for (std::size_t i = 2; i + 1 < data.size(); i += 2) {
      const auto first = byte(i);
      const auto second = byte(i + 1);
      utf16.push_back(static_cast<char16_t>(littleEndian ? (first | second << 8) : (first << 8 | second)));
    }
    data = QString::fromUtf16(utf16.data(), static_cast<qsizetype>(utf16.size())).toUtf8().toStdString();
  } else if (data.size() >= 3 && byte(0) == 0xEF && byte(1) == 0xBB && byte(2) == 0xBF) {
    data.erase(0, 3);
  }
  return textFromSelection(std::move(data));
}

std::string WlDataControlClipboard::bitmapFromImage(const std::string &encoded, std::size_t maxBytes)
{
  if (encoded.empty()) {
    return {};
  }

  const auto fits = [maxBytes](std::uint64_t width, std::uint64_t height) {
    return width > 0 && height > 0 && width <= 0x7FFF && height <= 0x7FFF &&
           kDibHeaderSize + width * height * 4 <= maxBytes;
  };

  auto bytes = QByteArray::fromRawData(encoded.data(), static_cast<qsizetype>(encoded.size()));
  QBuffer buffer(&bytes);
  if (!buffer.open(QIODevice::ReadOnly)) {
    return {};
  }
  QImageReader reader(&buffer);
  reader.setAutoTransform(true);
  if (const auto size = reader.size(); size.isValid() && !fits(size.width(), size.height())) {
    return {};
  }
  auto image = reader.read();
  if (image.isNull() || !fits(image.width(), image.height())) {
    return {};
  }
  image = image.convertToFormat(QImage::Format_ARGB32);

  const auto width = static_cast<std::size_t>(image.width());
  const auto height = static_cast<std::size_t>(image.height());
  const auto imageSize = width * height * 4;

  std::string dib;
  dib.reserve(kDibHeaderSize + imageSize);
  appendLe32(dib, kDibHeaderSize);                        // biSize
  appendLe32(dib, static_cast<std::uint32_t>(width));     // biWidth
  appendLe32(dib, static_cast<std::uint32_t>(height));    // biHeight, positive: bottom-up rows
  appendLe16(dib, 1);                                     // biPlanes
  appendLe16(dib, 32);                                    // biBitCount
  appendLe32(dib, 0);                                     // biCompression: BI_RGB
  appendLe32(dib, static_cast<std::uint32_t>(imageSize)); // biSizeImage
  appendLe32(dib, 2835);                                  // biXPelsPerMeter: 72 DPI
  appendLe32(dib, 2835);                                  // biYPelsPerMeter
  appendLe32(dib, 0);                                     // biClrUsed
  appendLe32(dib, 0);                                     // biClrImportant

  dib.resize(kDibHeaderSize + imageSize);
  auto *out = reinterpret_cast<unsigned char *>(dib.data() + kDibHeaderSize);
  for (std::size_t row = 0; row < height; ++row) {
    const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(static_cast<int>(height - 1 - row)));
    for (std::size_t x = 0; x < width; ++x) {
      const QRgb pixel = line[x];
      *out++ = static_cast<unsigned char>(qBlue(pixel));
      *out++ = static_cast<unsigned char>(qGreen(pixel));
      *out++ = static_cast<unsigned char>(qRed(pixel));
      *out++ = static_cast<unsigned char>(qAlpha(pixel));
    }
  }
  return dib;
}

std::string WlDataControlClipboard::pngFromBitmap(const std::string &dib)
{
  const auto pixelOffset = dibPixelOffset(dib);
  if (pixelOffset == 0) {
    return {};
  }

  // a BMP file is the DIB with a file header in front
  const std::uint64_t fileSize = kBmpFileHeaderSize + dib.size();
  if (fileSize > 0xFFFFFFFF) {
    return {};
  }
  QByteArray bmp;
  bmp.reserve(static_cast<qsizetype>(fileSize));
  bmp.append("BM", 2);
  appendLe32(bmp, static_cast<std::uint32_t>(fileSize));
  appendLe32(bmp, 0); // bfReserved1, bfReserved2
  appendLe32(bmp, static_cast<std::uint32_t>(kBmpFileHeaderSize + pixelOffset));
  bmp.append(dib.data(), static_cast<qsizetype>(dib.size()));

  QImage image;
  if (!image.loadFromData(bmp, "BMP")) {
    return {};
  }
  QByteArray png;
  QBuffer buffer(&png);
  if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
    return {};
  }
  return png.toStdString();
}

std::size_t WlDataControlClipboard::dibPixelOffset(std::string_view dib)
{
  if (dib.size() < 12) {
    return 0;
  }
  const std::uint64_t headerSize = readLe32(dib, 0);
  std::uint64_t offset = 0;
  if (headerSize == 12) {
    // BITMAPCOREHEADER, with a palette of 3 byte entries
    const auto bitCount = readLe16(dib, 10);
    offset = 12 + (bitCount <= 8 ? (std::uint64_t{1} << bitCount) * 3 : 0);
  } else if (headerSize >= kDibHeaderSize && headerSize <= dib.size()) {
    // BITMAPINFOHEADER and its larger versions, with a palette of 4 byte entries
    const auto bitCount = readLe16(dib, 14);
    const auto compression = readLe32(dib, 16);
    const std::uint64_t colorsUsed = readLe32(dib, 32);
    offset = headerSize;
    if (headerSize == kDibHeaderSize) {
      // the newer headers contain the masks, this one is followed by them
      if (compression == 3) { // BI_BITFIELDS
        offset += 12;
      } else if (compression == 6) { // BI_ALPHABITFIELDS
        offset += 16;
      }
    }
    const std::uint64_t colors = colorsUsed != 0 ? colorsUsed : (bitCount <= 8 ? std::uint64_t{1} << bitCount : 0);
    offset += colors * 4;
  } else {
    return 0;
  }
  return offset <= dib.size() ? static_cast<std::size_t>(offset) : 0;
}

std::size_t WlDataControlClipboard::marshalledSize(const Formats &formats)
{
  // number of formats, then for each one its id, size and data
  std::size_t size = 4;
  for (const auto &data : formats) {
    if (data) {
      size += 8 + data->size();
    }
  }
  return size;
}

std::vector<IClipboard::Format> WlDataControlClipboard::fitToLimit(Formats &formats, std::size_t maxBytes)
{
  std::vector<IClipboard::Format> dropped;
  while (marshalledSize(formats) >= maxBytes) {
    std::optional<std::size_t> largest;
    for (std::size_t i = 0; i < kFormatCount; ++i) {
      if (formats[i] && (!largest || formats[i]->size() > formats[*largest]->size())) {
        largest = i;
      }
    }
    if (!largest) {
      break;
    }
    formats[*largest].reset();
    dropped.push_back(static_cast<IClipboard::Format>(*largest));
  }
  return dropped;
}

} // namespace deskflow
