// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "EmulationPage.g.h"
#include "onyx/library.h"

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

    onyx::GameEntry game_;
    std::string launch_mode_;
    bool menu_open_ = false;
    bool show_fps_ = false;
    bool quitting_ = false;
    bool started_ = false;
    Windows::UI::Xaml::DispatcherTimer fps_timer_{nullptr};
    std::chrono::steady_clock::time_point menu_opened_at_{};
};

} // namespace winrt::ONYX3DS::implementation

namespace winrt::ONYX3DS::factory_implementation {
struct EmulationPage : EmulationPageT<EmulationPage, implementation::EmulationPage> {};
} // namespace winrt::ONYX3DS::factory_implementation
