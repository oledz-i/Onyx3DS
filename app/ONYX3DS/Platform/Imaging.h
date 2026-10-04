// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "onyx/common.h"

namespace onyx::app {

// RGBA8 -> PNG bytes through Windows.Graphics.Imaging. Blocking; do not call
// on the UI thread.
bool EncodePng(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height, Bytes& png);

// Raw 48x48 RGBA icon (written by the library scanner) -> XAML image source.
// UI thread only.
winrt::Windows::UI::Xaml::Media::ImageSource IconFromRgba(const Bytes& rgba, int width, int height);

// Image file on disk (PNG/JPEG, any folder the app can read) -> image
// source, decoded at the given pixel width to save memory. UI thread only.
// Sources are cached per (path, width), so rebuilding a page reuses decoded
// images instead of reading and decoding the files again.
winrt::Windows::UI::Xaml::Media::ImageSource ImageFromFile(const std::string& path,
                                                           int decode_width = 0);

// Raw RGBA icon file -> image source, read off the UI thread and cached by
// path. Sets `target`'s Source when ready (immediately if cached). UI thread only.
void LoadIconInto(winrt::Windows::UI::Xaml::Controls::Image const& target, const std::string& path,
                  int width, int height);

// Whether a file exists, remembered for the rest of the run (UI thread only).
// Xbox brokers every file call, so repeated checks on the UI thread add up.
bool CachedExists(const std::string& path);

} // namespace onyx::app
