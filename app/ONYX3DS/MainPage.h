// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "MainPage.g.h"
#include "onyx/library.h"

namespace winrt::ONYX3DS::implementation {

struct MainPage : MainPageT<MainPage> {
    MainPage() = default;
    void InitializeComponent();

    void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs const& e);
    void OnNavigatedFrom(Windows::UI::Xaml::Navigation::NavigationEventArgs const& e);

private:
    void ApplyTheme();
    void Rebuild(bool keep_focus = true);
    void BuildPage();
    Windows::UI::Xaml::Controls::Button MakeTile(const onyx::GameEntry& game, double w, double h);
    Windows::UI::Xaml::UIElement MakeEmptySlot(double w, double h);
    void ShowEmptyState();
    void BuildBarButtons();
    void OnTileFocused(Windows::UI::Xaml::Controls::Button const& tile, const onyx::GameEntry& game,
                       bool focused);
    void OpenGame(const onyx::GameEntry& game);
    void ShowToolsMenu();
    void ChangePage(int delta);
    void UpdateClock();
    void StartAmbientMotion();
    void BuildSparkles();
    void UpdateGreeting();
    void MoveParallax(float nx, float ny);
    void HandleKeyDown(Windows::Foundation::IInspectable const& sender, Windows::UI::Xaml::Input::KeyRoutedEventArgs const& e);

    std::vector<onyx::GameEntry> games_;
    int page_ = 0;
    int columns_ = 4;
    int rows_ = 3;
    std::string focused_path_;
    Windows::UI::Xaml::DispatcherTimer clock_{nullptr};
    Windows::UI::Xaml::Media::ThemeShadow shadow_{nullptr};
    Windows::UI::Xaml::Media::TranslateTransform far_par_{nullptr};
    Windows::UI::Xaml::Media::TranslateTransform near_par_{nullptr};
    Windows::UI::Xaml::Media::Animation::Storyboard sparkles_{nullptr};
    bool animate_entrance_ = true; // fade the tiles in on the next BuildPage
    bool initialised_ = false;
    bool resume_checked_ = false;
    std::string theme_applied_;
    event_token key_token_{};
};

} // namespace winrt::ONYX3DS::implementation

namespace winrt::ONYX3DS::factory_implementation {
struct MainPage : MainPageT<MainPage, implementation::MainPage> {};
} // namespace winrt::ONYX3DS::factory_implementation
