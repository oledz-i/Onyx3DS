// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "MainPage.g.h"
#include "onyx/game_info.h"
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
    void BuildXmb();
    void StepCategory(int delta);
    void UpdateXmbPanel(const onyx::GameEntry& game);
    void ShowBackdrop(const onyx::GameEntry& game);
    void SetBackdropImage(const std::string& path);
    void HideBackdrop();
    void ScrollXmbTo(Windows::UI::Xaml::Controls::Button const& tile);
    bool BoxArtOn() const;
    void FillXmbInfo(const onyx::GameEntry& game, const onyx::GameInfo& info);
    bool Xmb() const;
    Windows::UI::Xaml::Controls::Button MakeTile(const onyx::GameEntry& game, double w, double h);
    Windows::UI::Xaml::UIElement MakeEmptySlot(double w, double h);
    void ShowEmptyState();
    void ShowRetroEmptyState();
    Windows::UI::Xaml::UIElement MakeSystemTabs();
    void UpdateSystemTabs();
    void SwitchSystem(onyx::GameSystem system);
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
    Windows::UI::Xaml::Controls::Grid xmb_panel_{nullptr};
    Windows::UI::Xaml::Controls::StackPanel xmb_info_{nullptr};
    Windows::UI::Xaml::DispatcherTimer info_timer_{nullptr};
    Windows::UI::Xaml::DispatcherTimer xmb_show_timer_{nullptr};
    std::string xmb_shown_;
    Windows::UI::Xaml::DispatcherTimer backdrop_timer_{nullptr};
    onyx::GameEntry backdrop_entry_;
    std::string backdrop_game_;   // the game the backdrop was asked for
    std::string backdrop_file_;   // the picture currently showing
    bool backdrop_front_ = false; // which of the two cross-fading layers is on top
    Windows::UI::Xaml::Controls::Border xmb_cover_{nullptr};
    Windows::UI::Xaml::Controls::ScrollViewer xmb_scroll_{nullptr};
    double xmb_stride_ = 132;     // height of one crossbar row, for scrolling
    bool launching_ = false;
    bool box_prefetch_started_ = false;
    onyx::GameEntry xmb_pending_;
    int xmb_special_ = 0; // XMB layout: 0 = the home system's games, 1 = recent, 2 = favourites
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
