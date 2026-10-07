// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "GamePage.g.h"
#include "onyx/cheats.h"
#include "onyx/library.h"

namespace winrt::ONYX3DS::implementation {

struct GamePage : GamePageT<GamePage> {
    GamePage() = default;
    void InitializeComponent();
    void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs const& e);

private:
    void Build();
    void Launch(const std::string& mode);
    void ShowCheats();
    void ShowGameSettings();
    void ShowNesGameSettings();
    void ShowStates();
    void CloseSide();
    void OnBack(Windows::Foundation::IInspectable const&, Windows::UI::Core::BackRequestedEventArgs const& e);

    onyx::GameEntry game_;
    onyx::CheatFile cheats_;
    event_token back_token_{};
};

} // namespace winrt::ONYX3DS::implementation

namespace winrt::ONYX3DS::factory_implementation {
struct GamePage : GamePageT<GamePage, implementation::GamePage> {};
} // namespace winrt::ONYX3DS::factory_implementation
