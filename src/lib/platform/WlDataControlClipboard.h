/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/IClipboard.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace deskflow {

//! Clipboard access through the Wayland data-control protocols
/*!
Compositors without the portal Clipboard interface (Hyprland, Sway and other
wlroots-based desktops) let clients read and set the selection through
ext-data-control-v1 or wlr-data-control-unstable-v1. This class speaks those
protocols over its own Wayland connection on a worker thread. It does not
spawn helper processes, so nothing outlives deskflow or inherits its sockets.

Selections set by other clients are read and handed to the selection callback,
which runs on the worker thread. Selections set with setSelection() are served
to other clients from memory.
*/
class WlDataControlClipboard
{
public:
  static constexpr std::size_t kFormatCount = static_cast<std::size_t>(IClipboard::Format::TotalFormats);

  //! Clipboard data indexed by IClipboard::Format, in IClipboard encodings
  using Formats = std::array<std::optional<std::string>, kFormatCount>;

  //! Called on the worker thread when another client sets the selection
  /*!
  The serial tells whether the selection is still current, see isCurrent().
  */
  using SelectionCallback = std::function<void(Formats formats, std::uint64_t serial)>;

  enum class Protocol
  {
    Any, //!< ext-data-control-v1 when available, else wlr-data-control
    Ext,
    Wlr
  };

  enum class Mode
  {
    Auto,     //!< use data control when the compositor has it
    Disabled, //!< never use data control, use the portal clipboard
    Enabled   //!< like Auto, but warn when the compositor doesn't have it
  };

  struct Config
  {
    Mode mode = Mode::Auto;
    Protocol protocol = Protocol::Any;
  };

  //! Name of the environment variable that overrides the automatic choice
  static constexpr const char *kEnvironmentVariable = "DESKFLOW_WL_CLIPBOARD";

  //! Mime type offered with every selection set by deskflow
  static constexpr const char *kMarkerMimeType = "application/x-deskflow-selection";

  //! Parses a DESKFLOW_WL_CLIPBOARD value; unknown values mean Auto
  static Config parseConfig(std::string_view value);

  //! Reads the configuration from the environment
  static Config configFromEnvironment();

  //! Connects to the compositor and starts the worker thread
  /*!
  Returns nullptr when there is no Wayland display, the compositor has no
  data-control manager for \p protocol, or there is no seat.
  \p maxBytes limits the data read from other clients, 0 disables reading.
  When \p readInitialSelection is false, the selection that exists when
  this is called is not reported, only the ones set later.
  */
  static std::unique_ptr<WlDataControlClipboard> create(
      SelectionCallback callback, std::size_t maxBytes, Protocol protocol = Protocol::Any,
      bool readInitialSelection = true
  );

  ~WlDataControlClipboard();

  WlDataControlClipboard(const WlDataControlClipboard &) = delete;
  WlDataControlClipboard &operator=(const WlDataControlClipboard &) = delete;

  //! Takes the selection and serves \p formats to other clients
  /*!
  Thread-safe. Empty formats are ignored, so the local selection is never
  cleared by deskflow.
  */
  void setSelection(const Formats &formats);

  //! Forgets the last selection seen
  /*!
  Thread-safe. Call this when another computer takes the clipboard, so the
  next local selection is reported even if it has the same content as the
  last one. A selection still being read is dropped, as it was set before
  the other computer took the clipboard.
  */
  void forgetSelection();

  //! Tells whether a reported selection is still the latest clipboard change
  /*!
  Thread-safe. Selections are reported on the worker thread and usually
  handled later on another one. Returns false if setSelection() or
  forgetSelection() was called after the selection with \p serial was
  reported, in which case that call is the newer change and the reported
  selection must not be shared.
  */
  bool isCurrent(std::uint64_t serial) const;

  //! Sets the limit for clipboard data read from other clients, 0 disables reading (thread-safe)
  void setMaxBytes(std::size_t maxBytes);

  //! Protocol in use, e.g. "ext-data-control-v1"
  const char *protocolName() const;

  //! @name Conversion helpers (public for tests)
  //@{

  //! Picks the mime type to read for each format from the offered ones
  static std::array<std::optional<std::string>, kFormatCount> chooseMimeTypes(const std::vector<std::string> &offered);

  //! Mime types offered for each format when deskflow sets the selection
  static std::vector<std::string> mimeTypesFor(IClipboard::Format format);

  //! Converts selection text to IClipboard text (UTF-8 with LF newlines)
  static std::string textFromSelection(std::string data);

  //! Converts selection HTML to IClipboard HTML (UTF-8 with LF newlines)
  static std::string htmlFromSelection(std::string data);

  //! Decodes an image (PNG, BMP, ...) to an IClipboard bitmap (32-bit BI_RGB DIB)
  /*!
  Returns an empty string if the image can't be decoded or the bitmap would
  be larger than \p maxBytes.
  */
  static std::string bitmapFromImage(const std::string &encoded, std::size_t maxBytes);

  //! Encodes an IClipboard bitmap (DIB) as PNG; empty on failure
  static std::string pngFromBitmap(const std::string &dib);

  //! Offset of the pixel data in a DIB, or 0 if the header isn't valid
  static std::size_t dibPixelOffset(std::string_view dib);

  //! Size of the formats when marshalled by deskflow::Clipboard
  static std::size_t marshalledSize(const Formats &formats);

  //! Drops the largest formats until the marshalled size is below \p maxBytes
  /*!
  The server refuses clipboard data of \p maxBytes or more, so this keeps
  the formats that fit instead of losing all of them. Returns the dropped
  formats.
  */
  static std::vector<IClipboard::Format> fitToLimit(Formats &formats, std::size_t maxBytes);

  //@}

private:
  class Impl;
  explicit WlDataControlClipboard(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> m_impl;
};

} // namespace deskflow
