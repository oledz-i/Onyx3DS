// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "EmulationPage.g.h"
#include "onyx/library.h"

namespace onyx::app::ui {
class OnScreenKeyboard;
}
namespace onyx::app {
struct KeyboardRequestInfo;
}

namespace winrt::ONYX3DS::implementation {

struct EmulationPage : EmulationPageT<EmulationPage> {
    EmulationPage() = default;
    void InitializeComponent();
    void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs const& e);
    void OnNavigatedFrom(Windows::UI::Xaml::Navigation::NavigationEventArgs const& e);

private:
    void StartGame();
    void OnStarted();
    void OnStopped(const std::string& reason);
    void OnHotkey(int hotkey);
    void OpenMenu();
    void CloseMenu();
    void BuildMenu();
    void ShowMenuCheats();
    void ShowMenuStates(bool loading);
    void ShowMenuQuickSettings();
    void Quit();
    void UpdateFps();
    void OnPointer(Windows::UI::Xaml::Input::PointerRoutedEventArgs const& e, bool pressed);

    // The on-screen keyboard for 3DS games that ask for text (see Ui/OnScreenKeyboard.h).
    // Created and shown only while a request is open.
    void InstallKeyboard();
    void ShowKeyboard(const onyx::app::KeyboardRequestInfo& info);
    void HideKeyboard();
    bool KeyboardOpen() const;
    std::shared_ptr<onyx::app::ui::OnScreenKeyboard> kb_;

    onyx::GameEntry game_;
    std::string launch_mode_;
    bool menu_open_ = false;
    bool show_fps_ = false;
    bool quitting_ = false;
    bool started_ = false;
    Windows::UI::Xaml::DispatcherTimer fps_timer_{nullptr};
    // Software frames drawn through a XAML Image.
    // Software-view frames are pulled once per XAML composition frame (vsync),
    // not on a 16 ms timer: a timer drifts against the 60 Hz display and shows
    // a duplicated or skipped frame every half second or so (visible judder).
    winrt::event_token sw_render_token_{};
    bool sw_render_active_ = false;
    void StartSoftwareView();
    void StopSoftwareView();
    Windows::UI::Xaml::Media::Imaging::WriteableBitmap sw_bitmap_{nullptr};
    std::vector<uint8_t> sw_pixels_;
    uint64_t sw_seq_ = 0;
    uint64_t sw_shown_ = 0;
    void UpdateSoftwareView();
    std::chrono::steady_clock::time_point menu_opened_at_{};
};

} // namespace winrt::ONYX3DS::implementation

namespace winrt::ONYX3DS::factory_implementation {
struct EmulationPage : EmulationPageT<EmulationPage, implementation::EmulationPage> {};
} // namespace winrt::ONYX3DS::factory_implementation
