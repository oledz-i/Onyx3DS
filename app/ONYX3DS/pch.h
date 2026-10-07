// ONYX 3DS - Xbox (UWP) frontend for the Azahar 3DS emulator
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#include <restrictederrorinfo.h>
#include <hstring.h>

// windows.h defines GetCurrentTime() as a macro, which breaks
// Storyboard::GetCurrentTime in the C++/WinRT XAML headers.
#undef GetCurrentTime

// Win32 that UWP is allowed to use
#include <fileapifromapp.h>
#include <gamingdeviceinformation.h>

// Graphics and audio
#include <d3d12.h>
#include <dxgi1_6.h>
#include <xaudio2.h>
#include <windows.ui.xaml.media.dxinterop.h>

// C++/WinRT
#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Globalization.DateTimeFormatting.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.System.Power.h>
#include <winrt/Windows.System.Threading.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Documents.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Animation.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.Xaml.Navigation.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>

// Vulkan types for the Dozen bridge (no loader is linked; see DozenDriver)
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <libretro.h>
#include <libretro_vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
